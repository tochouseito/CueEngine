#include <Cue/Build/Windows/WindowsArtifactPublisher.h>

#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Fatal.h>
#include <Cue/Foundation/Log.h>
#include <Cue/Project/Descriptor.h>

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <winioctl.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <source_location>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{
constexpr std::string_view k_projectId = "41234567-89ab-4cde-8f01-23456789abcd";

/// @brief Junction用Mount Point Reparse BufferのNative Layoutを表す
struct MountPointReparseBuffer final
{
    DWORD reparseTag = IO_REPARSE_TAG_MOUNT_POINT;
    WORD reparseDataLength = 0U;
    WORD reserved = 0U;
    WORD substituteNameOffset = 0U;
    WORD substituteNameLength = 0U;
    WORD printNameOffset = 0U;
    WORD printNameLength = 0U;
    wchar_t pathBuffer[1]{};
};

/// @brief 特権不要のDirectory JunctionをPublisher再検証Fixtureとして作成する
[[nodiscard]] bool create_directory_link(const std::filesystem::path &a_linkPath,
                                         const std::filesystem::path &a_targetPath)
{
    if (CreateDirectoryW(a_linkPath.c_str(), nullptr) == FALSE)
    {
        return false;
    }

    const std::wstring substituteName = L"\\??\\" + a_targetPath.native();
    const std::wstring printName = a_targetPath.native();
    const std::size_t substituteBytes = substituteName.size() * sizeof(wchar_t);
    const std::size_t printBytes = printName.size() * sizeof(wchar_t);
    const std::size_t pathBytes = substituteBytes + sizeof(wchar_t) + printBytes + sizeof(wchar_t);
    const std::size_t totalBytes = offsetof(MountPointReparseBuffer, pathBuffer) + pathBytes;
    if (substituteBytes > MAXWORD || printBytes > MAXWORD || pathBytes + 8U > MAXWORD || totalBytes > MAXDWORD)
    {
        RemoveDirectoryW(a_linkPath.c_str());
        return false;
    }

    std::vector<std::uint32_t> storage((totalBytes + sizeof(std::uint32_t) - 1U) / sizeof(std::uint32_t), 0U);
    auto *buffer = reinterpret_cast<MountPointReparseBuffer *>(storage.data());
    buffer->reparseTag = IO_REPARSE_TAG_MOUNT_POINT;
    buffer->reparseDataLength = static_cast<WORD>(pathBytes + 8U);
    buffer->substituteNameLength = static_cast<WORD>(substituteBytes);
    buffer->printNameOffset = static_cast<WORD>(substituteBytes + sizeof(wchar_t));
    buffer->printNameLength = static_cast<WORD>(printBytes);
    std::memcpy(buffer->pathBuffer, substituteName.data(), substituteBytes);
    std::memcpy(reinterpret_cast<std::byte *>(buffer->pathBuffer) + buffer->printNameOffset, printName.data(),
                printBytes);

    HANDLE link = CreateFileW(a_linkPath.c_str(), GENERIC_WRITE, 0U, nullptr, OPEN_EXISTING,
                              FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (link == INVALID_HANDLE_VALUE)
    {
        RemoveDirectoryW(a_linkPath.c_str());
        return false;
    }
    DWORD returned = 0U;
    const BOOL succeeded = DeviceIoControl(link, FSCTL_SET_REPARSE_POINT, buffer, static_cast<DWORD>(totalBytes),
                                           nullptr, 0U, &returned, nullptr);
    CloseHandle(link);
    if (succeeded == FALSE)
    {
        RemoveDirectoryW(a_linkPath.c_str());
        return false;
    }
    return true;
}

#if CUE_TEST_BUILD_CONFIGURATION == 1
constexpr cue::BuildConfiguration k_configuration = cue::BuildConfiguration::Debug;
constexpr std::string_view k_configurationName = "Debug";
#elif CUE_TEST_BUILD_CONFIGURATION == 2
constexpr cue::BuildConfiguration k_configuration = cue::BuildConfiguration::Development;
constexpr std::string_view k_configurationName = "Development";
#elif CUE_TEST_BUILD_CONFIGURATION == 3
constexpr cue::BuildConfiguration k_configuration = cue::BuildConfiguration::Release;
constexpr std::string_view k_configurationName = "Release";
#else
#error CUE_TEST_BUILD_CONFIGURATION must identify a supported configuration
#endif

class TestFatalHandler final : public cue::FatalHandler
{
  public:
    /// @brief Test中の引数なしFatalを即時失敗として終了する
    [[noreturn]] void terminate() noexcept override
    {
        std::abort();
    }
    /// @brief Test中のFatalを即時失敗として終了する
    [[noreturn]] void terminate(std::string_view) noexcept override
    {
        std::abort();
    }
};

/// @brief Artifact Readerの取消なし経路を提供するTest Cancellation
class TestArtifactReadCancellation final : public cue::BuildArtifactReadCancellation
{
  public:
    /// @brief 取消されていない状態を返す
    [[nodiscard]] bool is_cancel_requested() const noexcept override
    {
        return false;
    }
};

/// @brief 条件違反時にTest Processを失敗終了する
void require(bool a_condition, const std::source_location a_location = std::source_location::current()) noexcept
{
    if (!a_condition)
    {
        std::fprintf(stderr, "Requirement failed at %s:%u\n", a_location.file_name(), a_location.line());
        std::fflush(stderr);
        std::abort();
    }
}

/// @brief Result成功値を所有値として取得する
template <typename T>
[[nodiscard]] T take_value(cue::Result<T> a_result,
                           const std::source_location a_location = std::source_location::current()) noexcept
{
    if (!a_result.has_value() && a_result.try_error() != nullptr)
    {
        const cue::Error &error = *a_result.try_error();
        std::fprintf(stderr, "Result error: %.*s/%lld %.*s\n",
                     static_cast<int>(error.root_code().domain().size()), error.root_code().domain().data(),
                     static_cast<long long>(error.root_code().value()), static_cast<int>(error.summary().size()),
                     error.summary().data());
        for (const cue::ErrorContext &context : error.contexts())
        {
            std::fprintf(stderr, "  Context: %.*s\n", static_cast<int>(context.message().size()),
                         context.message().data());
        }
        if (const cue::NativeError *native = error.try_native_error(); native != nullptr)
        {
            std::fprintf(stderr, "  Native: %.*s/%lld\n", static_cast<int>(native->domain().size()),
                         native->domain().data(), static_cast<long long>(native->value()));
        }
    }
    require(a_result.has_value(), a_location);
    return std::move(*a_result.try_value());
}

/// @brief UTF-8 Path表示をTest入力用stringへ変換する
[[nodiscard]] std::string generic_path(const std::filesystem::path &a_path)
{
    const std::u8string text = a_path.generic_u8string();
    return std::string(reinterpret_cast<const char *>(text.data()), text.size());
}

/// @brief 小さいManifest Fileを検証用文字列として読む
[[nodiscard]] std::string read_text(const std::filesystem::path &a_path)
{
    std::ifstream input(a_path, std::ios::binary);
    require(static_cast<bool>(input));
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

/// @brief 小さい検証Manifestを一回のStream Writeで置換する
void write_text(const std::filesystem::path &a_path, std::string_view a_text)
{
    std::ofstream output(a_path, std::ios::binary | std::ios::trunc);
    output.write(a_text.data(), static_cast<std::streamsize>(a_text.size()));
    require(static_cast<bool>(output));
}

/// @brief 同じCurrent Inventoryを異なるMember順と空白で表すJSONを構築する
[[nodiscard]] std::string make_reordered_current(const cue::BuildArtifactInventory &a_inventory)
{
    std::string json = "{ \n  \"files\" : [";
    for (std::size_t index = 0U; index < a_inventory.files().size(); ++index)
    {
        const cue::BuildArtifactFile &file = a_inventory.files()[index];
        if (index != 0U)
        {
            json.push_back(',');
        }
        json.append("{\"contentHash\":\"");
        json.append(file.contentHash);
        json.append("\",\"hashAlgorithm\":\"sha256\",\"sizeBytes\":");
        json.append(std::to_string(file.byteSize));
        json.append(",\"path\":\"");
        json.append(file.relativePath);
        json.append("\"}");
    }
    json.append("],\n\"configuration\":\"");
    json.append(k_configurationName);
    json.append("\",\"artifactId\":\"");
    json.append(a_inventory.artifact_id());
    json.append("\",\"schemaVersion\":1 }\n");
    return json;
}

/// @brief Test Project契約を満たすDescriptorを構築する
[[nodiscard]] cue::ProjectDescriptor make_descriptor(const cue::AssertContext &a_assertContext)
{
    cue::ProjectId projectId = take_value(cue::ProjectId::parse(k_projectId, a_assertContext));
    return take_value(cue::create_blank_project_descriptor(projectId, "Artifact Publisher Test",
                                                           {{1U, 0U, 0U}, std::nullopt},
                                                           "00000000-0000-4000-8000-000000000099", a_assertContext));
}

/// @brief Test用Build PlanをOperation ID別に構築する
[[nodiscard]] cue::BuildPlan make_plan(const std::filesystem::path &a_projectRoot, std::string a_operationId,
                                       const cue::AssertContext &a_assertContext)
{
    cue::BuildProfile profile =
        take_value(cue::BuildProfile::create(k_configuration, cue::BuildTarget::GameModule, a_assertContext));
    constexpr cue::BuildWorkspaceCompatibility compatibility{
        cue::BuildGenerator::VisualStudio2026, cue::BuildArchitecture::X64, {19U, 51U, 36256U, 0U}, 1U};
    return take_value(cue::create_build_plan(
        {generic_path(a_projectRoot), std::move(profile), std::move(a_operationId), compatibility}, a_assertContext));
}

/// @brief UnsignedLocal Shipping Product用Release Build Planを構築する
[[nodiscard]] cue::BuildPlan make_shipping_plan(const std::filesystem::path &a_projectRoot,
                                                std::string a_operationId,
                                                const cue::AssertContext &a_assertContext)
{
    cue::BuildProfile profile = take_value(cue::BuildProfile::create_shipping_product(
        cue::BuildConfiguration::Release, cue::ShippingTrustMode::UnsignedLocal, {}, a_assertContext));
    constexpr cue::BuildWorkspaceCompatibility compatibility{
        cue::BuildGenerator::VisualStudio2026, cue::BuildArchitecture::X64, {19U, 51U, 36256U, 0U}, 1U};
    return take_value(cue::create_build_plan(
        {generic_path(a_projectRoot), std::move(profile), std::move(a_operationId), compatibility}, a_assertContext));
}

/// @brief #304 Trust Verifier未実装時のPublisherSigned拒否用Build Planを構築する
[[nodiscard]] cue::BuildPlan make_signed_shipping_plan(const std::filesystem::path &a_projectRoot,
                                                       std::string a_operationId,
                                                       const cue::AssertContext &a_assertContext)
{
    cue::BuildProfile profile = take_value(cue::BuildProfile::create_shipping_product(
        cue::BuildConfiguration::Release, cue::ShippingTrustMode::PublisherSigned, std::string(64U, 'a'),
        a_assertContext));
    constexpr cue::BuildWorkspaceCompatibility compatibility{
        cue::BuildGenerator::VisualStudio2026, cue::BuildArchitecture::X64, {19U, 51U, 36256U, 0U}, 1U};
    return take_value(cue::create_build_plan(
        {generic_path(a_projectRoot), std::move(profile), std::move(a_operationId), compatibility}, a_assertContext));
}

/// @brief Plan作成後に追加されたJunction経由のRoot外読書きとRollbackを拒否する
void test_reparse_revalidation(const std::filesystem::path &a_probe,
                              const cue::AssertContext &a_assertContext)
{
    const std::filesystem::path parent =
        std::filesystem::temp_directory_path() /
        ("CueBuildArtifactReparse-" + std::to_string(GetCurrentProcessId()) + "-" +
         std::string(k_configurationName));
    const std::filesystem::path project = parent / "Project";
    const std::filesystem::path outside = parent / "Outside";
    std::error_code error;
    std::filesystem::remove_all(parent, error);
    require(!error);
    require(std::filesystem::create_directories(project, error));
    require(!error);
    require(std::filesystem::create_directories(outside, error));
    require(!error);

    cue::ProjectDescriptor descriptor = make_descriptor(a_assertContext);
    std::unique_ptr<cue::BuildArtifactPublisher> publisher = take_value(
        cue::create_windows_build_artifact_publisher(generic_path(project), descriptor, a_assertContext));
    cue::BuildPlan lockPlan =
        make_plan(project, "91234567-89ab-4cde-8f01-23456789abcd", a_assertContext);
    require(create_directory_link(project / "Generated", outside));
    cue::ChildProcessCancellation cancellation;
    require(!publisher->acquire_build_lease(lockPlan, cancellation, std::nullopt).has_value());
    require(!std::filesystem::exists(outside / "Build"));
    require(std::filesystem::remove(project / "Generated", error));
    require(!error);

    cue::BuildPlan candidatePlan =
        make_plan(project, "a1234567-89ab-4cde-8f01-23456789abcd", a_assertContext);
    const std::filesystem::path output =
        std::filesystem::path(candidatePlan.binary_directory()) / "bin" / k_configurationName;
    require(std::filesystem::create_directories(output, error));
    require(!error);
    require(std::filesystem::copy_file(a_probe, output / "CueGameModule.dll"));
    {
        std::ofstream pdb(output / "CueGameModule.pdb", std::ios::binary | std::ios::trunc);
        pdb << "reparse-test-symbols-" << k_configurationName;
        require(static_cast<bool>(pdb));
    }
    auto candidateLease = take_value(publisher->acquire_build_lease(candidatePlan, cancellation, std::nullopt));
    require(candidateLease.has_value());
    const std::filesystem::path outsideCandidates = outside / "Candidates";
    require(std::filesystem::create_directories(outsideCandidates, error));
    require(!error);
    require(create_directory_link(project / "Generated" / "Build" / "Candidates", outsideCandidates));
    require(!publisher->publish(candidatePlan, cancellation, std::move(*candidateLease), std::nullopt).has_value());
    require(std::filesystem::is_empty(outsideCandidates));
    require(std::filesystem::remove(project / "Generated" / "Build" / "Candidates", error));
    require(!error);

    cue::BuildPlan storePlan =
        make_plan(project, "b1234567-89ab-4cde-8f01-23456789abcd", a_assertContext);
    auto storeLease = take_value(publisher->acquire_build_lease(storePlan, cancellation, std::nullopt));
    require(storeLease.has_value());
    const std::filesystem::path outsideStore = outside / "Store";
    require(std::filesystem::create_directories(outsideStore, error));
    require(!error);
    const std::filesystem::path targetStore = project / "Generated" / "Artifacts" / "GameModule";
    require(std::filesystem::create_directories(targetStore, error));
    require(!error);
    require(create_directory_link(targetStore / k_configurationName, outsideStore));
    require(!publisher->publish(storePlan, cancellation, std::move(*storeLease), std::nullopt).has_value());
    require(std::filesystem::is_empty(outsideStore));
    require(!std::filesystem::exists(std::filesystem::path(storePlan.candidate_directory())));
    require(std::filesystem::remove(targetStore / k_configurationName, error));
    require(!error);

    std::filesystem::remove_all(parent, error);
    require(!error);
}

/// @brief Artifact公開、Lock取消、失敗時Current保全を一つのProject Rootで検証する
void test_windows_artifact_publisher(const std::filesystem::path &a_probe, const std::filesystem::path &a_invalidProbe,
                                     const std::filesystem::path &a_crashingProbe,
                                     const std::filesystem::path &a_hangingProbe,
                                     const std::filesystem::path &a_zeroExitProbe,
                                     const cue::AssertContext &a_assertContext)
{
    const std::filesystem::path projectRoot =
        std::filesystem::temp_directory_path() /
        ("CueBuildArtifactPublisherTests-" + std::to_string(GetCurrentProcessId()) + "-" +
         std::string(k_configurationName));
    std::error_code error;
    std::filesystem::remove_all(projectRoot, error);
    require(!error);
    require(std::filesystem::create_directories(projectRoot));

    cue::ProjectDescriptor descriptor = make_descriptor(a_assertContext);
    std::unique_ptr<cue::BuildArtifactPublisher> publisher = take_value(
        cue::create_windows_build_artifact_publisher(generic_path(projectRoot), descriptor, a_assertContext));
    cue::BuildPlan plan = make_plan(projectRoot, "01234567-89ab-4cde-8f01-23456789abcd", a_assertContext);
    const std::filesystem::path binaryDirectory = std::filesystem::path(plan.binary_directory());
    const std::filesystem::path outputDirectory = binaryDirectory / "bin" / k_configurationName;
    require(std::filesystem::create_directories(outputDirectory));
    require(std::filesystem::copy_file(a_probe, outputDirectory / "CueGameModule.dll"));
    {
        std::ofstream pdb(outputDirectory / "CueGameModule.pdb", std::ios::binary | std::ios::trunc);
        pdb << "test-symbols-" << k_configurationName;
        require(static_cast<bool>(pdb));
    }

    cue::ChildProcessCancellation cancellation;
    auto lease = take_value(publisher->acquire_build_lease(plan, cancellation, std::nullopt));
    require(lease.has_value());
    const std::filesystem::path displacedWorkspace = binaryDirectory.parent_path() / "DisplacedWorkspace";
    require(MoveFileExW(binaryDirectory.c_str(), displacedWorkspace.c_str(), 0U) == FALSE);

    std::unique_ptr<cue::BuildArtifactPublisher> contender = take_value(
        cue::create_windows_build_artifact_publisher(generic_path(projectRoot), descriptor, a_assertContext));
    cue::ChildProcessCancellation contenderCancellation;
    using LeaseResult = cue::Result<std::optional<std::unique_ptr<cue::BuildWorkspaceLease>>>;
    std::unique_ptr<LeaseResult> contenderResult;
    std::thread contenderThread(
        [&]()
        {
            contenderResult = std::make_unique<LeaseResult>(
                contender->acquire_build_lease(plan, contenderCancellation, std::nullopt));
        });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    contenderCancellation.request_cancel();
    contenderThread.join();
    require(contenderResult != nullptr && contenderResult->has_value() && !contenderResult->try_value()->has_value());

    cue::ChildProcessCancellation timeoutCancellation;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(30);
    auto timedOut = contender->acquire_build_lease(plan, timeoutCancellation, deadline);
    require(!timedOut && timedOut.try_error()->root_code().domain() == "Cue.Build.Publisher" &&
            timedOut.try_error()->root_code().value() ==
                static_cast<std::int64_t>(cue::BuildArtifactPublisherError::LockWaitTimedOut));

    auto published = take_value(publisher->publish(plan, cancellation, std::move(*lease), std::nullopt));
    require(published.has_value());
    require(published->artifact_id() == plan.operation_id());
    require(published->files().size() == 3U);
    const std::filesystem::path store = std::filesystem::path(plan.artifact_store_directory());
    const std::filesystem::path version = store / "Versions" / std::string(plan.operation_id());
    require(std::filesystem::is_regular_file(version / "CueGameModule.dll"));
    require(std::filesystem::is_regular_file(version / "CueGameModule.pdb"));
    require(std::filesystem::is_regular_file(version / "CueGameModule.metadata.json"));
    const std::string metadata = read_text(version / "CueGameModule.metadata.json");
    require(metadata.find(std::string(k_projectId)) != std::string::npos);
    require(metadata.find("\"configuration\": \"" + std::string(k_configurationName) + "\"") != std::string::npos);
    require(metadata.find("\"compilerVersion\": 1951") != std::string::npos);
    require(metadata.find("\"fullVersion\": 195136256") != std::string::npos);
    require(metadata.find("\"build\": 0") != std::string::npos);
    const std::filesystem::path currentPath = store / "Current.json";
    const std::string current = read_text(currentPath);
    require(current.find(std::string(plan.operation_id())) != std::string::npos);
    require(current.find("sha256") != std::string::npos);
    require(current.find("CueGameModule.pdb") != std::string::npos);

    std::unique_ptr<cue::BuildArtifactReader> reader = take_value(
        cue::create_windows_build_artifact_reader(generic_path(projectRoot), descriptor, a_assertContext));
    TestArtifactReadCancellation readCancellation;
    auto currentV2ReadLease = take_value(
        reader->acquire_current_read_lease(*published, readCancellation, std::nullopt));
    require(currentV2ReadLease.has_value());
    currentV2ReadLease.reset();
    std::vector<cue::BuildArtifactFile> legacyFiles(published->files().begin(), published->files().end());
    cue::BuildArtifactInventory legacyInventory =
        take_value(cue::BuildArtifactInventory::create_legacy_game_module(
            plan, std::string(published->artifact_id()), std::move(legacyFiles), a_assertContext));
    const std::filesystem::path legacyVersion(legacyInventory.version_directory());
    require(std::filesystem::create_directories(legacyVersion));
    for (const cue::BuildArtifactFile &file : legacyInventory.files())
    {
        require(std::filesystem::copy_file(version / file.relativePath, legacyVersion / file.relativePath));
    }
    const std::filesystem::path legacyStore = legacyVersion.parent_path().parent_path();
    write_text(legacyStore / "Current.json", make_reordered_current(legacyInventory));
    auto legacyReadLease = take_value(
        reader->acquire_current_read_lease(legacyInventory, readCancellation, std::nullopt));
    require(legacyReadLease.has_value());
    legacyReadLease.reset();
    write_text(currentPath, make_reordered_current(*published));
    auto firstReadLease = take_value(
        reader->acquire_current_read_lease(*published, readCancellation, std::nullopt));
    auto secondReadLease = take_value(
        reader->acquire_current_read_lease(*published, readCancellation, std::nullopt));
    require(firstReadLease.has_value() && secondReadLease.has_value());
    HANDLE exclusive = CreateFileW((store / "Access.lock").c_str(), GENERIC_READ | GENERIC_WRITE,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
    require(exclusive != INVALID_HANDLE_VALUE);
    OVERLAPPED overlap{};
    require(LockFileEx(exclusive, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0U, 1U, 0U, &overlap) ==
            FALSE);
    require(GetLastError() == ERROR_LOCK_VIOLATION);
    firstReadLease.reset();
    secondReadLease.reset();
    overlap = {};
    require(LockFileEx(exclusive, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0U, 1U, 0U, &overlap) !=
            FALSE);
    require(UnlockFileEx(exclusive, 0U, 1U, 0U, &overlap) != FALSE);
    require(CloseHandle(exclusive) != FALSE);
    write_text(currentPath, R"json({"schemaVersion":2})json");
    require(!reader->acquire_current_read_lease(*published, readCancellation, std::nullopt).has_value());
    write_text(currentPath, current);

    if (k_configuration != cue::BuildConfiguration::Release)
    {
        cue::BuildPlan missingPdbPlan = make_plan(projectRoot, "31234567-89ab-4cde-8f01-23456789abcd", a_assertContext);
        auto missingPdbLease = take_value(publisher->acquire_build_lease(missingPdbPlan, cancellation, std::nullopt));
        require(missingPdbLease.has_value());
        require(std::filesystem::remove(outputDirectory / "CueGameModule.pdb"));
        require(
            !publisher->publish(missingPdbPlan, cancellation, std::move(*missingPdbLease), std::nullopt).has_value());
        require(read_text(currentPath) == current);
        std::ofstream pdb(outputDirectory / "CueGameModule.pdb", std::ios::binary | std::ios::trunc);
        pdb << "restored-test-symbols-" << k_configurationName;
        require(static_cast<bool>(pdb));
    }

    cue::BuildPlan invalidPlan = make_plan(projectRoot, "11234567-89ab-4cde-8f01-23456789abcd", a_assertContext);
    auto invalidLease = take_value(publisher->acquire_build_lease(invalidPlan, cancellation, std::nullopt));
    require(invalidLease.has_value());
    require(std::filesystem::remove(outputDirectory / "CueGameModule.dll"));
    require(std::filesystem::copy_file(a_invalidProbe, outputDirectory / "CueGameModule.dll"));
    require(!publisher->publish(invalidPlan, cancellation, std::move(*invalidLease), std::nullopt).has_value());
    require(read_text(currentPath) == current);
    require(!std::filesystem::exists(std::filesystem::path(invalidPlan.candidate_directory())));

    cue::BuildPlan crashingPlan = make_plan(projectRoot, "51234567-89ab-4cde-8f01-23456789abcd", a_assertContext);
    auto crashingLease = take_value(publisher->acquire_build_lease(crashingPlan, cancellation, std::nullopt));
    require(crashingLease.has_value());
    require(std::filesystem::remove(outputDirectory / "CueGameModule.dll"));
    require(std::filesystem::copy_file(a_crashingProbe, outputDirectory / "CueGameModule.dll"));
    require(!publisher
                 ->publish(crashingPlan, cancellation, std::move(*crashingLease),
                           std::chrono::steady_clock::now() + std::chrono::seconds(2))
                 .has_value());
    require(read_text(currentPath) == current);
    require(!std::filesystem::exists(std::filesystem::path(crashingPlan.candidate_directory())));

    cue::BuildPlan zeroExitPlan = make_plan(projectRoot, "81234567-89ab-4cde-8f01-23456789abcd", a_assertContext);
    auto zeroExitLease = take_value(publisher->acquire_build_lease(zeroExitPlan, cancellation, std::nullopt));
    require(zeroExitLease.has_value());
    require(std::filesystem::remove(outputDirectory / "CueGameModule.dll"));
    require(std::filesystem::copy_file(a_zeroExitProbe, outputDirectory / "CueGameModule.dll"));
    require(!publisher->publish(zeroExitPlan, cancellation, std::move(*zeroExitLease), std::nullopt).has_value());
    require(read_text(currentPath) == current);
    require(!std::filesystem::exists(std::filesystem::path(zeroExitPlan.candidate_directory())));

    cue::BuildPlan hangingPlan = make_plan(projectRoot, "61234567-89ab-4cde-8f01-23456789abcd", a_assertContext);
    auto hangingLease = take_value(publisher->acquire_build_lease(hangingPlan, cancellation, std::nullopt));
    require(hangingLease.has_value());
    require(std::filesystem::remove(outputDirectory / "CueGameModule.dll"));
    require(std::filesystem::copy_file(a_hangingProbe, outputDirectory / "CueGameModule.dll"));
    auto timedOutProbe = publisher->publish(hangingPlan, cancellation, std::move(*hangingLease),
                                            std::chrono::steady_clock::now() + std::chrono::milliseconds(50));
    require(!timedOutProbe && timedOutProbe.try_error()->root_code().domain() == "Cue.Build.Publisher" &&
            timedOutProbe.try_error()->root_code().value() ==
                static_cast<std::int64_t>(cue::BuildArtifactPublisherError::ModuleProbeTimedOut));
    require(read_text(currentPath) == current);
    require(!std::filesystem::exists(std::filesystem::path(hangingPlan.candidate_directory())));

    cue::BuildPlan cancelledPlan = make_plan(projectRoot, "71234567-89ab-4cde-8f01-23456789abcd", a_assertContext);
    auto cancelledLease = take_value(publisher->acquire_build_lease(cancelledPlan, cancellation, std::nullopt));
    require(cancelledLease.has_value());
    using PublishResult = cue::Result<std::optional<cue::BuildArtifactInventory>>;
    std::unique_ptr<PublishResult> cancelledResult;
    cue::ChildProcessCancellation probeCancellation;
    std::thread probeThread(
        [&]()
        {
            cancelledResult = std::make_unique<PublishResult>(
                publisher->publish(cancelledPlan, probeCancellation, std::move(*cancelledLease),
                                   std::chrono::steady_clock::now() + std::chrono::seconds(2)));
        });
    const std::filesystem::path cancelledCandidate(cancelledPlan.candidate_directory());
    for (std::size_t attempt = 0U; attempt < 50U && !std::filesystem::exists(cancelledCandidate); ++attempt)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    require(std::filesystem::exists(cancelledCandidate));
    const std::filesystem::path displacedOutput = outputDirectory.parent_path() / "DisplacedOutput";
    const std::filesystem::path candidateParent = cancelledCandidate.parent_path();
    const std::filesystem::path displacedCandidate = candidateParent / "DisplacedCandidate";
    const std::filesystem::path displacedCandidates = candidateParent.parent_path() / "DisplacedCandidates";
    require(MoveFileExW(outputDirectory.c_str(), displacedOutput.c_str(), 0U) == FALSE);
    require(MoveFileExW(cancelledCandidate.c_str(), displacedCandidate.c_str(), 0U) == FALSE);
    require(MoveFileExW(candidateParent.c_str(), displacedCandidates.c_str(), 0U) == FALSE);
    probeCancellation.request_cancel();
    probeThread.join();
    require(cancelledResult != nullptr && cancelledResult->has_value() && !cancelledResult->try_value()->has_value());
    require(read_text(currentPath) == current);
    require(!std::filesystem::exists(std::filesystem::path(cancelledPlan.candidate_directory())));

    cue::BuildPlan missingPlan = make_plan(projectRoot, "21234567-89ab-4cde-8f01-23456789abcd", a_assertContext);
    auto missingLease = take_value(publisher->acquire_build_lease(missingPlan, cancellation, std::nullopt));
    require(missingLease.has_value());
    require(std::filesystem::remove(outputDirectory / "CueGameModule.dll"));
    require(!publisher->publish(missingPlan, cancellation, std::move(*missingLease), std::nullopt).has_value());
    require(read_text(currentPath) == current);

    std::filesystem::remove_all(projectRoot, error);
    require(!error);
}

/// @brief Shipping Productの公開、Identity拒否、Current保全、Tamper検出を検証する
void test_shipping_product_publisher(const std::filesystem::path &a_product,
                                     const std::filesystem::path &a_wrongProjectProduct,
                                     const std::filesystem::path &a_wrongConfigurationProduct,
                                     const std::filesystem::path &a_extraFileProduct,
                                     const cue::AssertContext &a_assertContext)
{
    const std::filesystem::path projectRoot =
        std::filesystem::temp_directory_path() /
        ("CueBSA-" + std::to_string(GetCurrentProcessId()) + "-" +
         std::string(k_configurationName));
    std::error_code error;
    std::filesystem::remove_all(projectRoot, error);
    require(!error && std::filesystem::create_directories(projectRoot));
    require(std::filesystem::create_directories(projectRoot / "Source" / "Game"));
    write_text(projectRoot / "CMakeLists.txt", "cmake_minimum_required(VERSION 4.2.0)\n");
    write_text(projectRoot / "CMakePresets.json", "{}\n");
    write_text(projectRoot / "CueProject.json", "{}\n");
    write_text(projectRoot / "Source" / "Game" / "Test.cpp", "int cue_shipping_test = 1;\n");

    cue::ProjectDescriptor descriptor = make_descriptor(a_assertContext);
    std::unique_ptr<cue::BuildArtifactPublisher> publisher = take_value(
        cue::create_windows_build_artifact_publisher(generic_path(projectRoot), descriptor, a_assertContext));
    cue::BuildPlan plan =
        make_shipping_plan(projectRoot, "01234567-89ab-4cde-8f01-23456789abcd", a_assertContext);
    const std::filesystem::path output =
        std::filesystem::path(plan.binary_directory()) / "bin" / "Release";
    require(std::filesystem::create_directories(output));
    require(std::filesystem::copy_file(a_product, output / "CueGameProduct.exe"));
    {
        std::ofstream pdb(output / "CueGameProduct.pdb", std::ios::binary | std::ios::trunc);
        pdb << "shipping-product-development-symbol";
        require(static_cast<bool>(pdb));
    }
    cue::ChildProcessCancellation cancellation;
    auto lease = take_value(publisher->acquire_build_lease(plan, cancellation, std::nullopt));
    require(lease.has_value());
    auto published = take_value(publisher->publish(plan, cancellation, std::move(*lease), std::nullopt));
    require(published.has_value() && published->profile().target() == cue::BuildTarget::ShippingProduct &&
            published->profile().minimum_trust_mode() == cue::ShippingTrustMode::UnsignedLocal &&
            published->files().size() == 3U);
    require(published->files()[0].relativePath == "CueGameProduct.exe" &&
            published->files()[0].purpose == cue::BuildArtifactFilePurpose::DistributionPayload);
    require(published->files()[1].relativePath == "CueGameProduct.metadata.json" &&
            published->files()[1].purpose == cue::BuildArtifactFilePurpose::RuntimeMetadata);
    require(published->files()[2].relativePath == "CueGameProduct.pdb" &&
            published->files()[2].purpose == cue::BuildArtifactFilePurpose::DevelopmentSymbol);

    const std::filesystem::path store(plan.artifact_store_directory());
    const std::filesystem::path version = store / "Versions" / std::string(plan.operation_id());
    const std::filesystem::path currentPath = store / "Current.json";
    const std::filesystem::path metadataPath = version / "CueGameProduct.metadata.json";
    const std::string current = read_text(currentPath);
    const std::string metadata = read_text(metadataPath);
    require(current.find("\"schemaVersion\": 2") != std::string::npos &&
            current.find("\"target\": \"ShippingProduct\"") != std::string::npos &&
            current.find("\"minimumTrustMode\": \"UnsignedLocal\"") != std::string::npos &&
            current.find("\"publisherKeyId\": null") != std::string::npos &&
            current.find("\"purpose\": \"DevelopmentSymbol\"") != std::string::npos);
    require(metadata.find(std::string(k_projectId)) != std::string::npos &&
             metadata.find("\"configuration\": \"Release\"") != std::string::npos &&
             metadata.find("\"architecture\": \"x64\"") != std::string::npos &&
             metadata.find("\"engineBuildPolicyVersion\": 1") != std::string::npos &&
             metadata.find("\"engineCommit\": \"") != std::string::npos &&
             metadata.find("\"engineSourceTreeState\": \"") != std::string::npos &&
             metadata.find("\"engineSourceInventory\": {") != std::string::npos &&
             metadata.find("\"gameSourceInventory\": {") != std::string::npos &&
             metadata.find("\"vcpkgManifestSha256\": \"") != std::string::npos &&
             metadata.find("\"vcpkgBaselineSha256\": \"") != std::string::npos &&
             metadata.find(published->files()[0].contentHash) != std::string::npos);

    std::unique_ptr<cue::BuildArtifactReader> reader = take_value(
        cue::create_windows_build_artifact_reader(generic_path(projectRoot), descriptor, a_assertContext));
    TestArtifactReadCancellation readCancellation;
    auto initialRead = take_value(
        reader->acquire_current_read_lease(*published, readCancellation, std::nullopt));
    require(initialRead.has_value());
    initialRead.reset();

    cue::BuildPlan acquireCancelledPlan =
        make_shipping_plan(projectRoot, "71234567-89ab-4cde-8f01-23456789abcd", a_assertContext);
    cue::ChildProcessCancellation acquireCancellation;
    acquireCancellation.request_cancel();
    auto acquireCancelled =
        publisher->acquire_build_lease(acquireCancelledPlan, acquireCancellation, std::nullopt);
    require(acquireCancelled.has_value() && !acquireCancelled.try_value()->has_value());
    require(read_text(currentPath) == current);

    cue::BuildPlan publishCancelledPlan =
        make_shipping_plan(projectRoot, "81234567-89ab-4cde-8f01-23456789abcd", a_assertContext);
    auto publishCancelledLease = take_value(
        publisher->acquire_build_lease(publishCancelledPlan, cancellation, std::nullopt));
    require(publishCancelledLease.has_value());
    cue::ChildProcessCancellation publishCancellation;
    publishCancellation.request_cancel();
    auto publishCancelled = publisher->publish(publishCancelledPlan, publishCancellation,
                                               std::move(*publishCancelledLease), std::nullopt);
    require(publishCancelled.has_value() && !publishCancelled.try_value()->has_value());
    require(read_text(currentPath) == current &&
            !std::filesystem::exists(std::filesystem::path(publishCancelledPlan.candidate_directory())));

    cue::BuildPlan changedSourcePlan =
        make_shipping_plan(projectRoot, "41234567-89ab-4cde-8f01-23456789abcd", a_assertContext);
    auto changedSourceLease = take_value(
        publisher->acquire_build_lease(changedSourcePlan, cancellation, std::nullopt));
    require(changedSourceLease.has_value());
    write_text(projectRoot / "Source" / "Game" / "Test.cpp", "int cue_shipping_test = 2;\n");
    require(!publisher->publish(changedSourcePlan, cancellation, std::move(*changedSourceLease), std::nullopt)
                 .has_value());
    require(read_text(currentPath) == current &&
            !std::filesystem::exists(std::filesystem::path(changedSourcePlan.candidate_directory())));
    write_text(projectRoot / "Source" / "Game" / "Test.cpp", "int cue_shipping_test = 1;\n");

    cue::BuildPlan signedPlan =
        make_signed_shipping_plan(projectRoot, "51234567-89ab-4cde-8f01-23456789abcd", a_assertContext);
    auto signedLease = take_value(publisher->acquire_build_lease(signedPlan, cancellation, std::nullopt));
    require(signedLease.has_value());
    require(!publisher->publish(signedPlan, cancellation, std::move(*signedLease), std::nullopt).has_value());
    require(read_text(currentPath) == current &&
            !std::filesystem::exists(std::filesystem::path(signedPlan.artifact_store_directory()) / "Current.json"));

    const auto publishRejected = [&](const std::filesystem::path &a_source, std::string a_operationId)
    {
        cue::BuildPlan rejectedPlan = make_shipping_plan(projectRoot, std::move(a_operationId), a_assertContext);
        require(std::filesystem::remove(output / "CueGameProduct.exe"));
        require(std::filesystem::copy_file(a_source, output / "CueGameProduct.exe"));
        auto rejectedLease = take_value(
            publisher->acquire_build_lease(rejectedPlan, cancellation, std::nullopt));
        require(rejectedLease.has_value());
        require(!publisher->publish(rejectedPlan, cancellation, std::move(*rejectedLease), std::nullopt).has_value());
        require(read_text(currentPath) == current &&
                !std::filesystem::exists(std::filesystem::path(rejectedPlan.candidate_directory())));
    };
    publishRejected(a_wrongProjectProduct, "11234567-89ab-4cde-8f01-23456789abcd");
    publishRejected(a_wrongConfigurationProduct, "21234567-89ab-4cde-8f01-23456789abcd");
    publishRejected(a_extraFileProduct, "61234567-89ab-4cde-8f01-23456789abcd");

    cue::BuildPlan incompletePlan =
        make_shipping_plan(projectRoot, "31234567-89ab-4cde-8f01-23456789abcd", a_assertContext);
    require(std::filesystem::remove(output / "CueGameProduct.exe"));
    auto incompleteLease = take_value(
        publisher->acquire_build_lease(incompletePlan, cancellation, std::nullopt));
    require(incompleteLease.has_value());
    require(!publisher->publish(incompletePlan, cancellation, std::move(*incompleteLease), std::nullopt).has_value());
    require(read_text(currentPath) == current);

    auto retainedRead = take_value(
        reader->acquire_current_read_lease(*published, readCancellation, std::nullopt));
    require(retainedRead.has_value());
    retainedRead.reset();
    write_text(metadataPath, metadata + "tampered");
    require(!reader->acquire_current_read_lease(*published, readCancellation, std::nullopt).has_value());

    std::filesystem::remove_all(projectRoot, error);
    require(!error);
}
} // namespace

/// @brief Windows Artifact PublisherのProcess間契約とAtomic Current保全を検証する
int main(int a_argumentCount, char **a_arguments)
{
    require(a_argumentCount == 10);
    TestFatalHandler fatalHandler;
    std::vector<std::unique_ptr<cue::LogSink>> sinks;
    cue::Logger logger(fatalHandler, std::move(sinks));
    cue::AssertContext assertContext(logger, fatalHandler);
    test_reparse_revalidation(std::filesystem::path(a_arguments[1]), assertContext);
    test_windows_artifact_publisher(std::filesystem::path(a_arguments[1]), std::filesystem::path(a_arguments[2]),
                                     std::filesystem::path(a_arguments[3]), std::filesystem::path(a_arguments[4]),
                                     std::filesystem::path(a_arguments[5]), assertContext);
    test_shipping_product_publisher(std::filesystem::path(a_arguments[6]), std::filesystem::path(a_arguments[7]),
                                    std::filesystem::path(a_arguments[8]), std::filesystem::path(a_arguments[9]),
                                    assertContext);
    return 0;
}
