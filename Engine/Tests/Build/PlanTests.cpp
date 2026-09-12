#include <Cue/Build/Plan.h>

#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Fatal.h>
#include <Cue/Foundation/Log.h>

#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <winioctl.h>
#endif

#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace
{
constexpr cue::BuildWorkspaceCompatibility k_workspaceCompatibility{
    cue::BuildGenerator::VisualStudio2026, cue::BuildArchitecture::X64, {19U, 51U, 0U, 0U}, 1U};

class TestFatalHandler final : public cue::FatalHandler
{
  public:
    /// @brief Test中の回復不能失敗を終了Codeへ変換する
    [[noreturn]] void terminate() noexcept override
    {
        std::_Exit(90);
    }

    /// @brief Message付き回復不能失敗を終了Codeへ変換する
    [[noreturn]] void terminate(std::string_view) noexcept override
    {
        std::_Exit(91);
    }
};

#if defined(_WIN32)
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

/// @brief 特権不要のDirectory JunctionをReparse Point Fixtureとして作成する
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
#else
/// @brief Portable Symbolic LinkをReparse Point相当のFixtureとして作成する
[[nodiscard]] bool create_directory_link(const std::filesystem::path &a_linkPath,
                                         const std::filesystem::path &a_targetPath)
{
    std::error_code error;
    std::filesystem::create_directory_symlink(a_targetPath, a_linkPath, error);
    return !error;
}
#endif

/// @brief Configurationに対応するProfile、Preset、Outputを検証する
[[nodiscard]] bool test_configuration(cue::BuildConfiguration a_configuration, std::string_view a_configurationName,
                                      std::string_view a_preset, std::string_view a_binaryDirectoryName,
                                      std::string_view a_projectRoot, const cue::AssertContext &a_assertContext)
{
    auto profile = cue::BuildProfile::create(a_configuration, cue::BuildTarget::GameModule, a_assertContext);
    if (!profile)
    {
        return false;
    }
    cue::BuildRequest request{std::string(a_projectRoot), *profile.try_value(), "01234567-89ab-4cde-8f01-23456789abcd",
                              k_workspaceCompatibility};
    auto first = cue::create_build_plan(request, a_assertContext);
    auto second = cue::create_build_plan(request, a_assertContext);
    if (!first || !second || !first.try_value()->equivalent_to(*second.try_value()))
    {
        return false;
    }
    const cue::BuildPlan &plan = *first.try_value();
    const std::string root(a_projectRoot);
    return plan.project_root() == root && plan.profile() == *profile.try_value() &&
           plan.operation_id() == request.operationId && plan.preset_name() == a_preset &&
           plan.workspace_key() == "windows-vs2026-x64-msvc-19.51.0.0-policy-1-" + std::string(a_binaryDirectoryName) &&
           plan.workspace_compatibility() == k_workspaceCompatibility && plan.cmake_target_name() == "CueGameModule" &&
           plan.workspace_lock_file() ==
               root + "/Generated/Build/Locks/GameModule/" + std::string(plan.workspace_key()) + ".lock" &&
           plan.binary_directory() == root + "/Generated/Build/GameModule/" + std::string(plan.workspace_key()) &&
           plan.candidate_directory() ==
               root + "/Generated/Build/Candidates/GameModule/01234567-89ab-4cde-8f01-23456789abcd" &&
           plan.operation_directory() == root + "/Saved/Build/Operations/01234567-89ab-4cde-8f01-23456789abcd" &&
           plan.artifact_store_directory() ==
               root + "/Generated/Artifacts/GameModule/" + std::string(a_configurationName) + "/modular";
}

/// @brief Project Root内外を指す既存Link経由のOutputを拒否する
[[nodiscard]] bool test_reparse_output_rejection(std::string_view a_testRoot, const cue::AssertContext &a_assertContext)
{
    const std::filesystem::path parent = std::filesystem::path(a_testRoot) / "CueBuildPlanReparseTests";
    const std::filesystem::path project = parent / "Project";
    const std::filesystem::path outside = parent / "Outside";
    const std::filesystem::path inside = project / "Internal";
    std::error_code error;
    std::filesystem::remove_all(parent, error);
    if (error || !std::filesystem::create_directories(project, error) || error ||
        !std::filesystem::create_directories(outside, error) || error ||
        !std::filesystem::create_directories(inside, error) || error)
    {
        return false;
    }
    if (!create_directory_link(project / "Generated", outside))
    {
        std::filesystem::remove_all(parent, error);
        return false;
    }
    auto profile =
        cue::BuildProfile::create(cue::BuildConfiguration::Debug, cue::BuildTarget::GameModule, a_assertContext);
    if (!profile)
    {
        std::filesystem::remove_all(parent, error);
        return false;
    }
    const std::string projectRoot = project.generic_string();
    cue::BuildRequest request{projectRoot, *profile.try_value(), "01234567-89ab-4cde-8f01-23456789abcd",
                              k_workspaceCompatibility};
    const bool outsideRejected = !cue::create_build_plan(request, a_assertContext);
    if (!std::filesystem::remove(project / "Generated", error) || error)
    {
        std::filesystem::remove_all(parent, error);
        return false;
    }
    const bool insideLinkCreated = create_directory_link(project / "Generated", inside);
    const bool insideRejected = insideLinkCreated && !cue::create_build_plan(request, a_assertContext);
    if (!std::filesystem::remove(project / "Generated", error) || error)
    {
        std::filesystem::remove_all(parent, error);
        return false;
    }
    const std::filesystem::path projectLink = parent / "ProjectLink";
    const bool projectLinkCreated = create_directory_link(projectLink, project);
    const std::string projectLinkRoot = projectLink.generic_string();
    cue::BuildRequest rootLinkRequest{projectLinkRoot, *profile.try_value(), "11234567-89ab-4cde-8f01-23456789abcd",
                                      k_workspaceCompatibility};
    const bool rootRejected = projectLinkCreated && !cue::create_build_plan(rootLinkRequest, a_assertContext);
    std::filesystem::remove_all(parent, error);
    return outsideRejected && insideRejected && rootRejected && !error;
}

/// @brief 未解決Linkを経由して将来Project Root外へ出力できるPlanを拒否する
[[nodiscard]] bool test_dangling_reparse_output_rejection(std::string_view a_testRoot,
                                                          const cue::AssertContext &a_assertContext)
{
    const std::filesystem::path parent = std::filesystem::path(a_testRoot) / "CueBuildPlanDanglingReparseTests";
    const std::filesystem::path project = parent / "Project";
    const std::filesystem::path missingOutside = parent / "MissingOutside";
    std::error_code error;
    std::filesystem::remove_all(parent, error);
    if (error || !std::filesystem::create_directories(project, error) || error)
    {
        return false;
    }
    if (!create_directory_link(project / "Generated", missingOutside))
    {
        std::filesystem::remove_all(parent, error);
        return false;
    }
    auto profile =
        cue::BuildProfile::create(cue::BuildConfiguration::Debug, cue::BuildTarget::GameModule, a_assertContext);
    if (!profile)
    {
        std::filesystem::remove_all(parent, error);
        return false;
    }
    const std::string projectRoot = project.generic_string();
    cue::BuildRequest request{projectRoot, *profile.try_value(), "01234567-89ab-4cde-8f01-23456789abcd",
                              k_workspaceCompatibility};
    const bool rejected = !cue::create_build_plan(request, a_assertContext);
    std::filesystem::remove_all(parent, error);
    return rejected && !error;
}

/// @brief MAX_PATHを超える通常形式Project RootからPlanを生成できるか検証する
[[nodiscard]] bool test_long_project_root(std::string_view a_testRoot, const cue::AssertContext &a_assertContext)
{
    const std::filesystem::path parent = std::filesystem::path(a_testRoot) / "CueBuildPlanLongPathTests";
    std::error_code error;
    std::filesystem::remove_all(parent, error);
    if (error || !std::filesystem::create_directories(parent, error) || error)
    {
        return false;
    }

    std::filesystem::path current = parent;
#if defined(_WIN32)
    current.make_preferred();
#endif
    std::vector<std::filesystem::path> createdDirectories;
    while (current.native().size() <= 280U)
    {
        current /= "LongProjectPathSegment0123456789";
#if defined(_WIN32)
        std::wstring extended = L"\\\\?\\";
        extended.append(current.native());
        const std::filesystem::path created(std::move(extended));
        if (CreateDirectoryW(created.c_str(), nullptr) == FALSE)
        {
            error = std::error_code(static_cast<int>(GetLastError()), std::system_category());
            break;
        }
        createdDirectories.push_back(created);
#else
        if (!std::filesystem::create_directory(current, error) || error)
        {
            break;
        }
        createdDirectories.push_back(current);
#endif
    }

    auto profile =
        cue::BuildProfile::create(cue::BuildConfiguration::Debug, cue::BuildTarget::GameModule, a_assertContext);
    const std::string projectRoot = current.generic_string();
    bool planAccepted = false;
    if (!error && profile)
    {
        cue::BuildRequest request{projectRoot, *profile.try_value(), "01234567-89ab-4cde-8f01-23456789abcd",
                                  k_workspaceCompatibility};
        auto plan = cue::create_build_plan(request, a_assertContext);
        if (!plan)
        {
            std::fprintf(stderr, "Long Project Root rejected: %.*s\n",
                         static_cast<int>(plan.try_error()->summary().size()), plan.try_error()->summary().data());
        }
        planAccepted = plan && plan.try_value()->project_root() == projectRoot;
    }

    bool cleanupSucceeded = true;
    for (auto directory = createdDirectories.rbegin(); directory != createdDirectories.rend(); ++directory)
    {
#if defined(_WIN32)
        cleanupSucceeded = RemoveDirectoryW(directory->c_str()) != FALSE && cleanupSucceeded;
#else
        cleanupSucceeded = std::filesystem::remove(*directory, error) && !error && cleanupSucceeded;
#endif
    }
    std::filesystem::remove(parent, error);
    if (!planAccepted || projectRoot.size() <= 260U || !cleanupSucceeded || error)
    {
        std::fprintf(stderr, "Long Project Root result: plan=%d size=%zu cleanup=%d error=%d\n", planAccepted ? 1 : 0,
                     projectRoot.size(), cleanupSucceeded ? 1 : 0, error.value());
    }
    return planAccepted && projectRoot.size() > 260U && cleanupSucceeded && !error;
}

/// @brief Profile JSONがMember順に依存せず往復し未知SchemaとMemberを拒否するか検証する
[[nodiscard]] bool test_profile_persistence(const cue::AssertContext &a_assertContext)
{
    auto profile =
        cue::BuildProfile::create(cue::BuildConfiguration::Development, cue::BuildTarget::GameModule, a_assertContext);
    if (!profile)
    {
        return false;
    }
    auto serialized = cue::serialize_build_profile(*profile.try_value(), a_assertContext);
    if (!serialized)
    {
        return false;
    }
    auto roundTrip = cue::parse_build_profile(*serialized.try_value(), a_assertContext);
    auto legacy = cue::parse_build_profile(
        R"json({"target":"GameModule","schemaVersion":1,"configuration":"Release"})json", a_assertContext);
    auto reordered = cue::parse_build_profile(
        R"json({"publisherKeyId":null,"target":"GameModule","schemaVersion":2,"minimumTrustMode":null,"configuration":"Release"})json",
        a_assertContext);
    auto unsignedShipping = cue::parse_build_profile(
        R"json({"schemaVersion":2,"configuration":"Release","target":"ShippingProduct","minimumTrustMode":"UnsignedLocal","publisherKeyId":null})json",
        a_assertContext);
    auto signedShipping = cue::parse_build_profile(
        R"json({"schemaVersion":2,"configuration":"Release","target":"ShippingProduct","minimumTrustMode":"PublisherSigned","publisherKeyId":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"})json",
        a_assertContext);
    auto unknownVersion = cue::parse_build_profile(
        R"json({"schemaVersion":3,"configuration":"Debug","target":"GameModule","minimumTrustMode":null,"publisherKeyId":null})json",
        a_assertContext);
    auto unknownMember = cue::parse_build_profile(
        R"json({"schemaVersion":1,"configuration":"Debug","unexpected":"GameModule"})json", a_assertContext);
    auto duplicate = cue::parse_build_profile(
        R"json({"schemaVersion":1,"configuration":"Debug","configuration":"Release"})json", a_assertContext);
    auto downgraded = cue::parse_build_profile(
        R"json({"schemaVersion":2,"configuration":"Release","target":"ShippingProduct","minimumTrustMode":"UnsignedLocal","publisherKeyId":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"})json",
        a_assertContext);
    auto gameModuleEmptyPublisher = cue::parse_build_profile(
        R"json({"schemaVersion":2,"configuration":"Release","target":"GameModule","minimumTrustMode":null,"publisherKeyId":""})json",
        a_assertContext);
    auto unsignedEmptyPublisher = cue::parse_build_profile(
        R"json({"schemaVersion":2,"configuration":"Release","target":"ShippingProduct","minimumTrustMode":"UnsignedLocal","publisherKeyId":""})json",
        a_assertContext);
    return profile.try_value()->schema_version() == 2U &&
           serialized.try_value()->find("\"schemaVersion\": 2") != std::string::npos && roundTrip &&
           *roundTrip.try_value() == *profile.try_value() && legacy &&
           legacy.try_value()->configuration() == cue::BuildConfiguration::Release && reordered &&
           reordered.try_value()->configuration() == cue::BuildConfiguration::Release && unsignedShipping &&
           unsignedShipping.try_value()->minimum_trust_mode() == cue::ShippingTrustMode::UnsignedLocal &&
           unsignedShipping.try_value()->publisher_key_id().empty() && signedShipping &&
           signedShipping.try_value()->minimum_trust_mode() == cue::ShippingTrustMode::PublisherSigned &&
           signedShipping.try_value()->publisher_key_id() == std::string(64U, 'a') && !unknownVersion &&
           !unknownMember && !duplicate && !downgraded && !gameModuleEmptyPublisher && !unsignedEmptyPublisher;
}

/// @brief Release Shipping ProductがTrust IdentityごとにBuild出力を分離するか検証する
[[nodiscard]] bool test_shipping_product(std::string_view a_projectRoot, const cue::AssertContext &a_assertContext)
{
    auto unsignedProfile = cue::BuildProfile::create_shipping_product(
        cue::BuildConfiguration::Release, cue::ShippingTrustMode::UnsignedLocal, {}, a_assertContext);
    const std::string publisherKey(64U, 'a');
    auto signedProfile = cue::BuildProfile::create_shipping_product(
        cue::BuildConfiguration::Release, cue::ShippingTrustMode::PublisherSigned, publisherKey, a_assertContext);
    auto invalidConfiguration = cue::BuildProfile::create_shipping_product(
        cue::BuildConfiguration::Development, cue::ShippingTrustMode::UnsignedLocal, {}, a_assertContext);
    auto missingPublisher = cue::BuildProfile::create_shipping_product(
        cue::BuildConfiguration::Release, cue::ShippingTrustMode::PublisherSigned, {}, a_assertContext);
    auto unexpectedPublisher = cue::BuildProfile::create_shipping_product(
        cue::BuildConfiguration::Release, cue::ShippingTrustMode::UnsignedLocal, publisherKey, a_assertContext);
    auto missingTrust =
        cue::BuildProfile::create(cue::BuildConfiguration::Release, cue::BuildTarget::ShippingProduct, a_assertContext);
    if (!unsignedProfile || !signedProfile || invalidConfiguration || missingPublisher || unexpectedPublisher ||
        missingTrust)
    {
        return false;
    }

    cue::BuildRequest unsignedRequest{std::string(a_projectRoot), *unsignedProfile.try_value(),
                                      "21234567-89ab-4cde-8f01-23456789abcd", k_workspaceCompatibility};
    cue::BuildRequest signedRequest{std::string(a_projectRoot), *signedProfile.try_value(),
                                    "31234567-89ab-4cde-8f01-23456789abcd", k_workspaceCompatibility};
    auto unsignedPlan = cue::create_build_plan(unsignedRequest, a_assertContext);
    auto signedPlan = cue::create_build_plan(signedRequest, a_assertContext);
    if (!unsignedPlan || !signedPlan)
    {
        return false;
    }

    const std::string root(a_projectRoot);
    const cue::BuildPlan &unsignedValue = *unsignedPlan.try_value();
    const cue::BuildPlan &signedValue = *signedPlan.try_value();
    return unsignedValue.cmake_target_name() == "CueGameProduct" &&
           unsignedValue.profile().target() == cue::BuildTarget::ShippingProduct &&
           unsignedValue.workspace_key() == "vs26-x64-m19.51.0.0-p1-r-u" &&
           unsignedValue.workspace_key().size() <= 96U &&
           unsignedValue.binary_directory() ==
               root + "/Generated/Build/ShippingProduct/" + std::string(unsignedValue.workspace_key()) &&
           unsignedValue.workspace_lock_file() == root + "/Generated/Build/Locks/ShippingProduct/" +
                                                      std::string(unsignedValue.workspace_key()) + ".lock" &&
           unsignedValue.candidate_directory() ==
               root + "/Generated/Build/Candidates/ShippingProduct/21234567-89ab-4cde-8f01-23456789abcd" &&
           unsignedValue.artifact_store_directory() ==
               root + "/Generated/Artifacts/ShippingProduct/Release/unsigned-local" &&
           signedValue.workspace_key() == "vs26-x64-m19.51.0.0-p1-r-s-" + publisherKey &&
           signedValue.workspace_key().size() <= 96U &&
           signedValue.artifact_store_directory() ==
               root + "/Generated/Artifacts/ShippingProduct/Release/publisher-aaaaaaaaaaaaaaaa" &&
           signedValue.workspace_key() != unsignedValue.workspace_key() &&
           signedValue.binary_directory() != unsignedValue.binary_directory() &&
           signedValue.artifact_store_directory() != unsignedValue.artifact_store_directory();
}

/// @brief 不正Target、相対Root、Root形式Operation IDをPlan生成前に拒否するか検証する
[[nodiscard]] bool test_invalid_request(std::string_view a_projectRoot, const cue::AssertContext &a_assertContext)
{
    auto profile =
        cue::BuildProfile::create(cue::BuildConfiguration::Debug, cue::BuildTarget::GameModule, a_assertContext);
    auto invalidProfile = cue::BuildProfile::create(static_cast<cue::BuildConfiguration>(255U),
                                                    cue::BuildTarget::GameModule, a_assertContext);
    auto invalidTarget =
        cue::BuildProfile::create(cue::BuildConfiguration::Debug, static_cast<cue::BuildTarget>(255U), a_assertContext);
    if (!profile || invalidProfile || invalidTarget)
    {
        return false;
    }
    cue::BuildRequest relative{"Project", *profile.try_value(), "01234567-89ab-4cde-8f01-23456789abcd",
                               k_workspaceCompatibility};
    cue::BuildRequest traversal{std::string(a_projectRoot), *profile.try_value(), "../../outside",
                                k_workspaceCompatibility};
    cue::BuildRequest root{"C:/", *profile.try_value(), "01234567-89ab-4cde-8f01-23456789abcd",
                           k_workspaceCompatibility};
    cue::BuildWorkspaceCompatibility invalidCompatibility = k_workspaceCompatibility;
    invalidCompatibility.engineBuildPolicyVersion = 0U;
    cue::BuildRequest invalidWorkspace{std::string(a_projectRoot), *profile.try_value(),
                                       "01234567-89ab-4cde-8f01-23456789abcd", invalidCompatibility};
    cue::BuildRequest trailingSlash{std::string(a_projectRoot) + "/", *profile.try_value(),
                                    "01234567-89ab-4cde-8f01-23456789abcd", k_workspaceCompatibility};
    const std::string nativeRoot = std::filesystem::path(a_projectRoot).make_preferred().string();
    cue::BuildRequest trailingBackslash{nativeRoot + "\\", *profile.try_value(), "01234567-89ab-4cde-8f01-23456789abcd",
                                        k_workspaceCompatibility};
    std::string embeddedNullRoot(a_projectRoot);
    embeddedNullRoot.push_back('\0');
    embeddedNullRoot.append("/Other");
    cue::BuildRequest embeddedNull{embeddedNullRoot, *profile.try_value(), "01234567-89ab-4cde-8f01-23456789abcd",
                                   k_workspaceCompatibility};
    const std::filesystem::path regularFile = std::filesystem::path(a_projectRoot) / "NotDirectory";
    {
        std::ofstream output(regularFile, std::ios::binary);
        output.put('x');
        if (!output)
        {
            return false;
        }
    }
    cue::BuildRequest fileRoot{regularFile.generic_string(), *profile.try_value(),
                               "01234567-89ab-4cde-8f01-23456789abcd", k_workspaceCompatibility};
    auto slashPlan = cue::create_build_plan(trailingSlash, a_assertContext);
    auto backslashPlan = cue::create_build_plan(trailingBackslash, a_assertContext);
    const std::filesystem::path outputComponent = std::filesystem::path(a_projectRoot) / "Generated";
    {
        std::ofstream output(outputComponent, std::ios::binary);
        output.put('x');
        if (!output)
        {
            return false;
        }
    }
    auto unsafeOutputComponent =
        cue::create_build_plan({std::string(a_projectRoot), *profile.try_value(),
                                "11234567-89ab-4cde-8f01-23456789abcd", k_workspaceCompatibility},
                               a_assertContext);
    std::error_code removeError;
    const bool removedOutputComponent = std::filesystem::remove(outputComponent, removeError);
    return slashPlan && slashPlan.try_value()->project_root() == a_projectRoot && backslashPlan &&
           backslashPlan.try_value()->project_root() == a_projectRoot &&
           !cue::create_build_plan(fileRoot, a_assertContext) && !cue::create_build_plan(relative, a_assertContext) &&
           !cue::create_build_plan(traversal, a_assertContext) && !cue::create_build_plan(root, a_assertContext) &&
           !cue::create_build_plan(invalidWorkspace, a_assertContext) &&
           !cue::create_build_plan(embeddedNull, a_assertContext) && !unsafeOutputComponent && removedOutputComponent &&
           !removeError;
}

/// @brief ToolsetまたはEngine Policy変更時に別Workspaceを選択するか検証する
[[nodiscard]] bool test_workspace_compatibility(std::string_view a_projectRoot,
                                                const cue::AssertContext &a_assertContext)
{
    auto profile =
        cue::BuildProfile::create(cue::BuildConfiguration::Debug, cue::BuildTarget::GameModule, a_assertContext);
    if (!profile)
    {
        return false;
    }
    cue::BuildRequest baseline{std::string(a_projectRoot), *profile.try_value(), "01234567-89ab-4cde-8f01-23456789abcd",
                               k_workspaceCompatibility};
    cue::BuildRequest changedToolset = baseline;
    changedToolset.workspaceCompatibility.toolsetVersion.minor = 52U;
    cue::BuildRequest changedPolicy = baseline;
    changedPolicy.workspaceCompatibility.engineBuildPolicyVersion = 2U;
    auto baselinePlan = cue::create_build_plan(baseline, a_assertContext);
    auto toolsetPlan = cue::create_build_plan(changedToolset, a_assertContext);
    auto policyPlan = cue::create_build_plan(changedPolicy, a_assertContext);
    return baselinePlan && toolsetPlan && policyPlan &&
           baselinePlan.try_value()->workspace_key() != toolsetPlan.try_value()->workspace_key() &&
           baselinePlan.try_value()->workspace_key() != policyPlan.try_value()->workspace_key() &&
           baselinePlan.try_value()->binary_directory() != toolsetPlan.try_value()->binary_directory() &&
           baselinePlan.try_value()->binary_directory() != policyPlan.try_value()->binary_directory();
}

/// @brief Stage OutcomeとExit Codeの整合を検証する
[[nodiscard]] bool test_stage_results(const cue::AssertContext &a_assertContext)
{
    auto success = cue::BuildStageResult::create(cue::BuildStage::Configure, cue::BuildStageOutcome::Succeeded, 0U,
                                                 a_assertContext);
    auto failed =
        cue::BuildStageResult::create(cue::BuildStage::Build, cue::BuildStageOutcome::Failed, 2U, a_assertContext);
    auto cancelled = cue::BuildStageResult::create(cue::BuildStage::Build, cue::BuildStageOutcome::Cancelled,
                                                   std::nullopt, a_assertContext);
    auto inconsistent =
        cue::BuildStageResult::create(cue::BuildStage::Build, cue::BuildStageOutcome::Cancelled, 1U, a_assertContext);
    return success && success.try_value()->exit_code() == 0U && failed && failed.try_value()->exit_code() == 2U &&
           cancelled && cancelled.try_value()->outcome() == cue::BuildStageOutcome::Cancelled && !inconsistent;
}
} // namespace

/// @brief Build Request、Profile、Plan、Stage ResultのUI非依存契約を検証する
int main(int a_argumentCount, char **a_arguments)
{
    if (a_argumentCount != 2)
    {
        return 2;
    }
    TestFatalHandler fatalHandler;
    std::vector<std::unique_ptr<cue::LogSink>> sinks;
    cue::Logger logger(fatalHandler, std::move(sinks));
    cue::AssertContext assertContext(logger, fatalHandler);

    const std::filesystem::path project = std::filesystem::path(a_arguments[1]) / "CueBuildPlanProject";
    std::error_code error;
    std::filesystem::remove_all(project, error);
    if (error || !std::filesystem::create_directories(project, error) || error)
    {
        return 3;
    }
    const std::string projectRoot = project.generic_string();

    const bool modelSucceeded =
        test_configuration(cue::BuildConfiguration::Debug, "Debug", "windows-vs2026-debug", "debug", projectRoot,
                           assertContext) &&
        test_configuration(cue::BuildConfiguration::Development, "Development", "windows-vs2026-development",
                           "development", projectRoot, assertContext) &&
        test_configuration(cue::BuildConfiguration::Release, "Release", "windows-vs2026-release", "release",
                           projectRoot, assertContext) &&
        test_profile_persistence(assertContext) && test_shipping_product(projectRoot, assertContext) &&
        test_invalid_request(projectRoot, assertContext) && test_workspace_compatibility(projectRoot, assertContext) &&
        test_stage_results(assertContext);
    const bool reparseSucceeded = test_reparse_output_rejection(a_arguments[1], assertContext);
    const bool danglingReparseSucceeded = test_dangling_reparse_output_rejection(a_arguments[1], assertContext);
    const bool longPathSucceeded = test_long_project_root(a_arguments[1], assertContext);
    std::filesystem::remove_all(project, error);
    if (error)
    {
        return 4;
    }
    if (!modelSucceeded)
    {
        return 5;
    }
    if (!reparseSucceeded)
    {
        return 6;
    }
    if (!danglingReparseSucceeded)
    {
        return 7;
    }
    return longPathSucceeded ? 0 : 8;
}
