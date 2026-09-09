#include <Cue/Package/Workflow.h>

#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Fatal.h>
#include <Cue/Foundation/Log.h>
#include <Cue/IO/Windows/WindowsFilesystem.h>
#include <Cue/Project/Generator.h>
#include <Cue/Scene/Identity.h>
#include <Cue/Scene/Instantiation.h>
#include <Cue/Scene/SceneDocument.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <Windows.h>

namespace
{
constexpr std::string_view k_projectId = "00000000-0000-4000-8000-000000000901";
constexpr std::string_view k_sceneId = "10000000-0000-4000-8000-000000000001";
constexpr cue::BuildWorkspaceCompatibility k_workspaceCompatibility{
    cue::BuildGenerator::VisualStudio2026, cue::BuildArchitecture::X64, {19U, 51U, 0U, 0U}, 1U};

/// @brief Test内のFatalを固定Exit Codeへ変換する
class TestFatalHandler final : public cue::FatalHandler
{
  public:
    /// @brief MessageなしFatalを固定Exit Codeへ変換する
    [[noreturn]] void terminate() noexcept override
    {
        std::_Exit(76);
    }

    /// @brief Message付きFatalを固定Exit Codeへ変換する
    [[noreturn]] void terminate(std::string_view) noexcept override
    {
        std::_Exit(76);
    }
};

/// @brief Test所有の二RootをProcess終了時に除去する
class TestDirectory final
{
  public:
    /// @brief Process固有のTemporary Rootを作成する
    TestDirectory()
        : m_path(std::filesystem::temp_directory_path() /
                 (L"CuePackageWorkflowTests-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                  std::to_wstring(GetTickCount64())))
    {
        std::filesystem::create_directories(m_path);
    }

    TestDirectory(const TestDirectory &) = delete;
    TestDirectory &operator=(const TestDirectory &) = delete;

    /// @brief Test所有Rootだけを除去する
    ~TestDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(m_path, error);
    }

    /// @brief Temporary Rootを返す
    [[nodiscard]] const std::filesystem::path &path() const noexcept
    {
        return m_path;
    }

  private:
    std::filesystem::path m_path;
};

enum class RunnerMode : std::uint8_t
{
    Succeed,
    Fail,
    BlockUntilCancelled
};

struct RunnerState final
{
    std::atomic<RunnerMode> mode = RunnerMode::Succeed;
    std::atomic<std::uint32_t> calls = 0U;
    std::atomic<bool> active = false;
};

/// @brief BuildまたはRuntimeの成功、失敗、取消待機をProcessなしで再現する
class ControlledRunner final : public cue::ChildProcessRunner
{
  public:
    /// @brief 共有制御状態を借用する
    explicit ControlledRunner(RunnerState &a_state) noexcept : m_state(&a_state)
    {
    }

    /// @brief 指定Modeに応じた所有Process結果を返す
    [[nodiscard]] cue::Result<cue::ChildProcessResult> run(
        const cue::ChildProcessRequest &, const cue::ChildProcessCancellation &a_cancellation) noexcept override
    {
        m_state->active.store(true, std::memory_order_release);
        const std::uint32_t call = m_state->calls.fetch_add(1U, std::memory_order_relaxed);
        while (m_state->mode.load(std::memory_order_acquire) == RunnerMode::BlockUntilCancelled &&
               !a_cancellation.is_cancel_requested())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        m_state->active.store(false, std::memory_order_release);
        if (a_cancellation.is_cancel_requested())
        {
            return cue::Result<cue::ChildProcessResult>::success(cue::ChildProcessResult::cancelled({}));
        }
        const std::uint32_t exitCode = m_state->mode.load(std::memory_order_acquire) == RunnerMode::Fail ? 2U : 0U;
        return cue::Result<cue::ChildProcessResult>::success(cue::ChildProcessResult::exited(
            exitCode, {{call, cue::ChildProcessStream::StandardOutput, "process-log"}}));
    }

  private:
    RunnerState *m_state;
};

struct PublisherState final
{
    std::atomic<bool> corruptInventory = false;
    std::atomic<std::uint32_t> calls = 0U;
};

/// @brief 失敗した検証の呼出位置を標準エラーへ出す
[[nodiscard]] bool require(bool a_condition,
                           std::source_location a_location = std::source_location::current()) noexcept
{
    if (!a_condition)
    {
        std::cerr << "Requirement failed at " << a_location.file_name() << ':' << a_location.line() << '\n';
    }
    return a_condition;
}

/// @brief Build成功ArtifactをTest Rootへ実体化するPublisher
class MaterializingPublisher final : public cue::BuildArtifactPublisher
{
  public:
    /// @brief Native Resourceを持たないTest Lease
    class Lease final : public cue::BuildWorkspaceLease
    {
      public:
        /// @brief 空Leaseを構築する
        Lease() noexcept = default;
        /// @brief 空Leaseを破棄する
        ~Lease() override = default;
    };

    /// @brief Artifact Rootと制御状態を借用する
    MaterializingPublisher(PublisherState &a_state, const cue::AssertContext &a_assertContext) noexcept
        : m_state(&a_state), m_assertContext(&a_assertContext)
    {
    }

    /// @brief 取消前なら空のExclusive Leaseを返す
    [[nodiscard]] cue::Result<std::optional<std::unique_ptr<cue::BuildWorkspaceLease>>> acquire_build_lease(
        const cue::BuildPlan &, const cue::ChildProcessCancellation &a_cancellation) noexcept override
    {
        if (a_cancellation.is_cancel_requested())
        {
            return cue::Result<std::optional<std::unique_ptr<cue::BuildWorkspaceLease>>>::success(std::nullopt);
        }
        return cue::Result<std::optional<std::unique_ptr<cue::BuildWorkspaceLease>>>::success(
            std::optional<std::unique_ptr<cue::BuildWorkspaceLease>>(std::make_unique<Lease>()));
    }

    /// @brief Operation固有Versionへ必須ArtifactとPackage対象外PDBを書きInventoryを返す
    [[nodiscard]] cue::Result<std::optional<cue::BuildArtifactInventory>> publish(
        const cue::BuildPlan &a_plan, const cue::ChildProcessCancellation &a_cancellation,
        std::unique_ptr<cue::BuildWorkspaceLease> a_buildLease) noexcept override
    {
        static_cast<void>(a_buildLease);
        if (a_cancellation.is_cancel_requested())
        {
            return cue::Result<std::optional<cue::BuildArtifactInventory>>::success(std::nullopt);
        }
        m_state->calls.fetch_add(1U, std::memory_order_relaxed);
        const std::vector<std::byte> moduleBytes = text_bytes("test-game-module");
        const std::vector<std::byte> pdbBytes = text_bytes("test-debug-symbols");
        const std::vector<std::byte> metadataBytes = text_bytes("{\"schemaVersion\":1}\n");
        auto modulePayload = cue::package::PackageFilePayload::create(
            cue::package::PackageFileRole::GameModule, "CueGameModule.dll", moduleBytes, *m_assertContext);
        auto metadataPayload = cue::package::PackageFilePayload::create(
            cue::package::PackageFileRole::GameModuleMetadata, "CueGameModule.metadata.json", metadataBytes,
            *m_assertContext);
        if (!modulePayload || !metadataPayload)
        {
            return cue::Result<std::optional<cue::BuildArtifactInventory>>::failure(
                modulePayload ? std::move(*metadataPayload.try_error()) : std::move(*modulePayload.try_error()));
        }
        const std::string artifactId(a_plan.operation_id());
        const std::filesystem::path versionDirectory =
            std::filesystem::path(a_plan.project_root()) / std::filesystem::path(a_plan.artifact_store_directory()) /
            L"Versions" / std::filesystem::path(artifactId);
        std::error_code error;
        std::filesystem::create_directories(versionDirectory, error);
        if (error || !write_file(versionDirectory / L"CueGameModule.dll", moduleBytes) ||
            !write_file(versionDirectory / L"CueGameModule.pdb", pdbBytes) ||
            !write_file(versionDirectory / L"CueGameModule.metadata.json", metadataBytes))
        {
            cue::ErrorCode code = cue::ErrorCode::create(m_assertContext->fatal_handler(), "Cue.Package.Test", 1);
            return cue::Result<std::optional<cue::BuildArtifactInventory>>::failure(
                cue::Error::create(m_assertContext->fatal_handler(), std::move(code), "Artifact materialization failed"));
        }
        std::string moduleHash(modulePayload.try_value()->entry().sha256());
        if (m_state->corruptInventory.load(std::memory_order_acquire))
        {
            moduleHash.assign(64U, 'f');
        }
        auto inventory = cue::BuildArtifactInventory::create(
            a_plan, artifactId,
            {{"CueGameModule.dll", moduleBytes.size(), std::move(moduleHash)},
             {"CueGameModule.pdb", pdbBytes.size(), std::string(64U, 'a')},
             {"CueGameModule.metadata.json", metadataBytes.size(),
              std::string(metadataPayload.try_value()->entry().sha256())}},
            *m_assertContext);
        if (!inventory)
        {
            return cue::Result<std::optional<cue::BuildArtifactInventory>>::failure(
                std::move(*inventory.try_error()));
        }
        return cue::Result<std::optional<cue::BuildArtifactInventory>>::success(
            std::optional<cue::BuildArtifactInventory>(std::move(*inventory.try_value())));
    }

  private:
    /// @brief ASCII Test入力を所有Byte列へ変換する
    [[nodiscard]] static std::vector<std::byte> text_bytes(std::string_view a_text)
    {
        const std::span<const char> characters(a_text.data(), a_text.size());
        const std::span<const std::byte> bytes = std::as_bytes(characters);
        return {bytes.begin(), bytes.end()};
    }

    /// @brief Test Artifact Byte列をBinary Fileへ書く
    [[nodiscard]] static bool write_file(const std::filesystem::path &a_path,
                                         std::span<const std::byte> a_bytes) noexcept
    {
        std::ofstream stream(a_path, std::ios::binary | std::ios::trunc);
        stream.write(reinterpret_cast<const char *>(a_bytes.data()), static_cast<std::streamsize>(a_bytes.size()));
        return stream.good();
    }

    PublisherState *m_state;
    const cue::AssertContext *m_assertContext;
};

/// @brief Processを起動しないBuild Runner設定を返す
[[nodiscard]] cue::CMakeRunnerSettings make_settings()
{
    return {"C:/Tools/cmake.exe", "C:/CueEngine", {}, std::chrono::seconds(5), std::chrono::seconds(5)};
}

/// @brief Debug Game Module用Build Requestを作る
[[nodiscard]] cue::BuildRequest make_request(std::string a_projectRoot, std::string a_operationId,
                                              const cue::AssertContext &a_assertContext)
{
    auto profile =
        cue::BuildProfile::create(cue::BuildConfiguration::Debug, cue::BuildTarget::GameModule, a_assertContext);
    return {std::move(a_projectRoot), *profile.try_value(), std::move(a_operationId), k_workspaceCompatibility};
}

/// @brief 空Sceneから決定的Runtime Data Publicationを作る
[[nodiscard]] cue::Result<cue::package::MinimalRuntimeDataPublication> make_runtime_data(
    const cue::AssertContext &a_assertContext) noexcept
{
    auto projectId = cue::ProjectId::parse(k_projectId, a_assertContext);
    auto descriptor = projectId ? cue::create_blank_project_descriptor(
                                      *projectId.try_value(), "Workflow Test",
                                      cue::EngineCompatibility{cue::EngineVersion{1U, 0U, 0U},
                                                               cue::EngineVersion{2U, 0U, 0U}},
                                      k_sceneId,
                                      a_assertContext)
                                : cue::Result<cue::ProjectDescriptor>::failure(std::move(*projectId.try_error()));
    auto sceneId = cue::scene::SceneAssetId::parse(k_sceneId, a_assertContext);
    if (!descriptor || !sceneId)
    {
        return cue::Result<cue::package::MinimalRuntimeDataPublication>::failure(
            descriptor ? std::move(*sceneId.try_error()) : std::move(*descriptor.try_error()));
    }
    cue::scene::SceneDocument scene = cue::scene::SceneDocument::create(*sceneId.try_value(), a_assertContext);
    auto snapshot = cue::scene::create_scene_snapshot(scene, a_assertContext);
    return snapshot ? cue::package::publish_minimal_runtime_data(*descriptor.try_value(), *snapshot.try_value(),
                                                                 a_assertContext)
                    : cue::Result<cue::package::MinimalRuntimeDataPublication>::failure(
                          std::move(*snapshot.try_error()));
}

/// @brief RunnerがProcess実行中になるまで上限付きで待つ
[[nodiscard]] bool wait_until_active(const RunnerState &a_state) noexcept
{
    for (std::uint32_t attempt = 0U; attempt < 1000U; ++attempt)
    {
        if (a_state.active.load(std::memory_order_acquire))
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

/// @brief Build・Package・Runの成功、失敗保全、停止をHeadlessで検証する
[[nodiscard]] bool test_workflow(const cue::AssertContext &a_assertContext)
{
    TestDirectory directory;
    const std::filesystem::path projectRoot = directory.path() / L"Project";
    const std::filesystem::path engineRoot = directory.path() / L"Engine";
    const std::filesystem::path hostPath = engineRoot / L"bin" / L"Debug" / L"CueRuntimeHost.exe";
    std::filesystem::create_directories(projectRoot);
    std::filesystem::create_directories(hostPath.parent_path());
    {
        std::ofstream host(hostPath, std::ios::binary);
        host << "test-runtime-host";
    }

    RunnerState buildRunner;
    RunnerState runRunner;
    PublisherState publisher;
    auto build = cue::GameBuildService::create(make_settings(), std::make_unique<ControlledRunner>(buildRunner),
                                                std::make_unique<MaterializingPublisher>(publisher, a_assertContext),
                                                a_assertContext);
    auto projectFilesystem = cue::create_windows_filesystem_root(projectRoot.generic_string(), a_assertContext);
    auto engineFilesystem = cue::create_windows_filesystem_root(engineRoot.generic_string(), a_assertContext);
    if (!require(build && projectFilesystem && engineFilesystem))
    {
        return false;
    }
    std::unique_ptr<cue::GameBuildService> buildService = std::move(*build.try_value());
    auto workflow = cue::package::GamePackageWorkflowService::create(
        *buildService, std::move(*projectFilesystem.try_value()), std::move(*engineFilesystem.try_value()),
        std::make_unique<ControlledRunner>(runRunner), projectRoot.generic_string(), {}, a_assertContext);
    auto runtimeData = make_runtime_data(a_assertContext);
    if (!require(workflow && runtimeData))
    {
        return false;
    }
    std::unique_ptr<cue::package::GamePackageWorkflowService> service = std::move(*workflow.try_value());
    constexpr std::string_view firstOperation = "01234567-89ab-4cde-8f01-23456789abcd";
    if (!require(service->start(make_request(projectRoot.generic_string(), std::string(firstOperation),
                                             a_assertContext),
                                cue::CMakeConfigureMode::Required, {1U, 0U, 0U}, std::string(k_projectId),
                                *runtimeData.try_value()) &&
                 service->wait_for_package()))
    {
        return false;
    }
    cue::package::PackageWorkflowSnapshot first = service->snapshot();
    if (first.state != cue::package::PackageWorkflowState::PackageReady)
    {
        std::cerr << "Initial package state=" << static_cast<int>(first.state) << " message=" << first.message << '\n';
    }
    if (!require(first.state == cue::package::PackageWorkflowState::PackageReady && first.package &&
                 first.latestSuccessfulPackage && first.package->operationId == firstOperation &&
                 std::filesystem::exists(projectRoot / std::filesystem::path(first.package->destination) /
                                         L"CuePackage.json") &&
                 !std::filesystem::exists(projectRoot / std::filesystem::path(first.package->destination) /
                                          L"Runtime" / L"CueGameModule.pdb")))
    {
        return false;
    }
    const std::string firstDestination = first.package->destination;

    buildRunner.mode.store(RunnerMode::Fail, std::memory_order_release);
    if (!require(service->start(make_request(projectRoot.generic_string(),
                                             "11234567-89ab-4cde-8f01-23456789abcd", a_assertContext),
                                cue::CMakeConfigureMode::Required, {1U, 0U, 0U}, std::string(k_projectId),
                                *runtimeData.try_value()) &&
                 service->wait_for_package()))
    {
        return false;
    }
    cue::package::PackageWorkflowSnapshot buildFailed = service->snapshot();
    if (!require(buildFailed.state == cue::package::PackageWorkflowState::Failed && !buildFailed.package &&
                 buildFailed.latestSuccessfulPackage &&
                 buildFailed.latestSuccessfulPackage->destination == firstDestination &&
                 publisher.calls.load(std::memory_order_acquire) == 1U))
    {
        return false;
    }

    buildRunner.mode.store(RunnerMode::Succeed, std::memory_order_release);
    publisher.corruptInventory.store(true, std::memory_order_release);
    if (!require(service->retry("21234567-89ab-4cde-8f01-23456789abcd") && service->wait_for_package()))
    {
        return false;
    }
    cue::package::PackageWorkflowSnapshot packageFailed = service->snapshot();
    if (!require(packageFailed.state == cue::package::PackageWorkflowState::Failed && !packageFailed.package &&
                 packageFailed.latestSuccessfulPackage &&
                 packageFailed.latestSuccessfulPackage->destination == firstDestination))
    {
        return false;
    }

    publisher.corruptInventory.store(false, std::memory_order_release);
    if (!require(service->retry("31234567-89ab-4cde-8f01-23456789abcd") && service->wait_for_package() &&
                 service->run(cue::package::PackageRunMode::SmokeTest) && service->wait_for_run_completion()))
    {
        return false;
    }
    const cue::package::PackageWorkflowSnapshot runSucceeded = service->snapshot();
    if (!require(runSucceeded.state == cue::package::PackageWorkflowState::RunSucceeded &&
                 runSucceeded.runOutput.size() == 1U))
    {
        return false;
    }

    runRunner.mode.store(RunnerMode::BlockUntilCancelled, std::memory_order_release);
    if (!require(service->run(cue::package::PackageRunMode::Interactive) && wait_until_active(runRunner) &&
                 service->stop() && service->wait_for_run_completion()))
    {
        return false;
    }
    if (!require(service->snapshot().state == cue::package::PackageWorkflowState::PackageReady &&
                 !runRunner.active.load(std::memory_order_acquire)))
    {
        return false;
    }

    if (!require(service->run(cue::package::PackageRunMode::Interactive) && wait_until_active(runRunner)))
    {
        return false;
    }
    service.reset();
    return require(!runRunner.active.load(std::memory_order_acquire));
}
} // namespace

/// @brief Build・Package・Run CoordinatorをUIなしで検証する
int main()
{
    TestFatalHandler fatalHandler;
    std::vector<std::unique_ptr<cue::LogSink>> sinks;
    cue::Logger logger(fatalHandler, std::move(sinks));
    cue::AssertContext assertContext(logger, fatalHandler);
    return test_workflow(assertContext) ? 0 : 1;
}
