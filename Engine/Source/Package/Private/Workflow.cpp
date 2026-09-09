#include <Cue/Package/Workflow.h>

#include <Cue/Foundation/Assert.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <span>
#include <thread>
#include <utility>

namespace
{
enum class WorkflowError : std::int64_t
{
    MissingDependency = 1,
    InvalidInput,
    OperationAlreadyRunning,
    NoRetryableOperation,
    NoPublishedPackage,
    NoActiveOperation,
    OwnerThreadViolation,
    ArtifactMismatch,
    PackagePublicationFailed,
    RuntimeProcessFailed
};

/// @brief 回復不能なWorkflow内部例外をFatalHandlerへ通知してProcessを停止する
[[noreturn]] void terminate_workflow_exception(const cue::AssertContext &a_assertContext) noexcept
{
    a_assertContext.fatal_handler().terminate("Game package workflow failed unexpectedly");
    std::abort();
}

/// @brief Workflow固有の回復可能Errorを一貫したDomainで構築する
[[nodiscard]] cue::Error make_workflow_error(const cue::AssertContext &a_assertContext, WorkflowError a_code,
                                             std::string_view a_summary) noexcept
{
    cue::ErrorCode code = cue::ErrorCode::create(a_assertContext.fatal_handler(), "Cue.Package.Workflow",
                                                 static_cast<std::int64_t>(a_code));
    return cue::Error::create(a_assertContext.fatal_handler(), std::move(code), a_summary);
}

/// @brief Build ConfigurationをCMakeおよびPackage Pathと同じ固定名へ変換する
[[nodiscard]] std::string_view configuration_name(cue::BuildConfiguration a_configuration) noexcept
{
    switch (a_configuration)
    {
    case cue::BuildConfiguration::Debug:
        return "Debug";
    case cue::BuildConfiguration::Development:
        return "Development";
    case cue::BuildConfiguration::Release:
        return "Release";
    }
    return {};
}

/// @brief ASCII文字列を同一Byte列へ変換して所有する
[[nodiscard]] std::vector<std::byte> copy_bytes(std::string_view a_text)
{
    const std::span<const char> characters(a_text.data(), a_text.size());
    const std::span<const std::byte> raw = std::as_bytes(characters);
    return std::vector<std::byte>(raw.begin(), raw.end());
}

/// @brief 二つのRoot相対Path要素をslash一個で連結する
[[nodiscard]] std::string join_relative(std::string_view a_left, std::string_view a_right)
{
    std::string path(a_left);
    if (!path.empty() && path.back() != '/')
    {
        path.push_back('/');
    }
    path.append(a_right);
    return path;
}

/// @brief UTF-8 Absolute Rootと正規化済みRelative Pathを表示・Process用Locatorへ連結する
[[nodiscard]] std::string join_absolute(std::string_view a_root, std::string_view a_relative)
{
    std::string path(a_root);
    if (!path.empty() && path.back() != '/' && path.back() != '\\')
    {
        path.push_back('/');
    }
    path.append(a_relative);
    return path;
}

/// @brief 同じProject Rootから導出されたAbsolute LocatorをRoot相対Locatorへ変換する
[[nodiscard]] std::optional<std::string> make_project_relative(std::string_view a_projectRoot,
                                                               std::string_view a_absoluteLocator)
{
    std::string root(a_projectRoot);
    std::string locator(a_absoluteLocator);
    std::replace(root.begin(), root.end(), '\\', '/');
    std::replace(locator.begin(), locator.end(), '\\', '/');
    while (root.size() > 3U && root.back() == '/')
    {
        root.pop_back();
    }
    root.push_back('/');
    if (!locator.starts_with(root) || locator.size() == root.size())
    {
        return std::nullopt;
    }
    return locator.substr(root.size());
}

/// @brief Error Domain、Code、SummaryをUI向け一行Messageへ平坦化する
[[nodiscard]] std::string error_message(const cue::Error &a_error)
{
    std::string message(a_error.root_code().domain());
    message.push_back('/');
    message.append(std::to_string(a_error.root_code().value()));
    message.push_back(' ');
    message.append(a_error.summary());
    return message;
}

/// @brief Build失敗状態をPackage Workflow終端状態へ変換する
[[nodiscard]] cue::package::PackageWorkflowState build_terminal_state(cue::GameBuildOperationState a_state) noexcept
{
    return a_state == cue::GameBuildOperationState::Cancelled ? cue::package::PackageWorkflowState::Cancelled
                                                               : cue::package::PackageWorkflowState::Failed;
}
} // namespace

namespace cue::package
{
struct GamePackageWorkflowService::Impl final
{
    struct PackageInputs final
    {
        EngineVersion engineVersion;
        std::string projectId;
        MinimalRuntimeDataPublication runtimeData;
    };

    /// @brief 検証済み依存とRoot Locatorの所有権をWorkflow実装へ移す
    Impl(GameBuildService &a_buildService, std::unique_ptr<FilesystemRoot> a_projectFilesystem,
         std::unique_ptr<FilesystemRoot> a_engineBinaryFilesystem,
         std::unique_ptr<ChildProcessRunner> a_runProcessRunner, std::string a_projectRoot,
         std::vector<ChildProcessEnvironmentEntry> a_runEnvironment, const AssertContext &a_assertContext) noexcept
        : buildService(&a_buildService), projectFilesystem(std::move(a_projectFilesystem)),
          engineBinaryFilesystem(std::move(a_engineBinaryFilesystem)), runProcessRunner(std::move(a_runProcessRunner)),
          projectRoot(std::move(a_projectRoot)), runEnvironment(std::move(a_runEnvironment)),
          assertContext(&a_assertContext), ownerThread(std::this_thread::get_id())
    {
    }

    /// @brief 呼出ThreadがService生成Threadと一致するか返す
    [[nodiscard]] bool is_owner_thread() const noexcept
    {
        return std::this_thread::get_id() == ownerThread;
    }

    /// @brief 入力Rootから上限付きByte列を読みPackage Payloadへ変換する
    [[nodiscard]] Result<PackageFilePayload> read_payload(FilesystemRoot &a_filesystem,
                                                          std::string a_sourcePath,
                                                          PackageFileRole a_role,
                                                          std::string a_packagePath,
                                                          std::uint64_t a_maximumBytes) noexcept
    {
        Result<RelativePath> source = RelativePath::parse(a_sourcePath, *assertContext);
        if (!source || a_maximumBytes > std::numeric_limits<std::size_t>::max())
        {
            return Result<PackageFilePayload>::failure(
                source ? make_workflow_error(*assertContext, WorkflowError::InvalidInput,
                                             "Package input file exceeds the addressable size")
                       : std::move(*source.try_error()));
        }
        Result<std::vector<std::byte>> bytes =
            a_filesystem.read_file(*source.try_value(), static_cast<std::size_t>(a_maximumBytes));
        if (!bytes)
        {
            return Result<PackageFilePayload>::failure(std::move(*bytes.try_error()));
        }
        return PackageFilePayload::create(a_role, std::move(a_packagePath), std::move(*bytes.try_value()),
                                          *assertContext);
    }

    /// @brief Build ArtifactとRuntime Dataから不変Packageを一度だけ公開する
    [[nodiscard]] Result<PublishedRuntimePackageSnapshot> publish_package(
        const BuildArtifactInventory &a_artifact, const PackageInputs &a_inputs, std::string_view a_operationId,
        const PackageCancellation &a_cancellation) noexcept
    {
        try
        {
            const std::string_view configuration = configuration_name(a_artifact.configuration());
            if (configuration.empty())
            {
                return Result<PublishedRuntimePackageSnapshot>::failure(make_workflow_error(
                    *assertContext, WorkflowError::InvalidInput, "Package Build Configuration is invalid"));
            }

            std::vector<PackageFilePayload> payloads;
            payloads.reserve(a_artifact.files().size() + 3U);
            Result<PackageFilePayload> runtimeHost =
                read_payload(*engineBinaryFilesystem, join_relative("bin", join_relative(configuration, "CueRuntimeHost.exe")),
                             PackageFileRole::RuntimeHost, "CueRuntimeHost.exe", k_maximumPackagedFileBytes);
            if (!runtimeHost)
            {
                return Result<PublishedRuntimePackageSnapshot>::failure(std::move(*runtimeHost.try_error()));
            }
            payloads.push_back(std::move(*runtimeHost.try_value()));

            const std::optional<std::string> artifactDirectory =
                make_project_relative(projectRoot, a_artifact.version_directory());
            if (!artifactDirectory)
            {
                return Result<PublishedRuntimePackageSnapshot>::failure(make_workflow_error(
                    *assertContext, WorkflowError::ArtifactMismatch,
                    "Build artifact directory is outside the package project root"));
            }

            for (const BuildArtifactFile &file : a_artifact.files())
            {
                if (a_cancellation.is_cancel_requested())
                {
                    return Result<PublishedRuntimePackageSnapshot>::failure(make_workflow_error(
                        *assertContext, WorkflowError::PackagePublicationFailed,
                        "Package publication was cancelled"));
                }
                if (file.relativePath == "CueGameModule.pdb")
                {
                    continue;
                }
                PackageFileRole role = PackageFileRole::RuntimeDependency;
                std::string packagePath = join_relative("Runtime", file.relativePath);
                if (file.relativePath == "CueGameModule.dll")
                {
                    role = PackageFileRole::GameModule;
                    packagePath = "Game/CueGameModule.dll";
                }
                else if (file.relativePath == "CueGameModule.metadata.json")
                {
                    role = PackageFileRole::GameModuleMetadata;
                    packagePath = "Game/CueGameModule.metadata.json";
                }
                Result<PackageFilePayload> payload = read_payload(
                    *projectFilesystem, join_relative(*artifactDirectory, file.relativePath), role,
                    std::move(packagePath), file.byteSize);
                if (!payload)
                {
                    return Result<PublishedRuntimePackageSnapshot>::failure(std::move(*payload.try_error()));
                }
                if (payload.try_value()->entry().byte_size() != file.byteSize ||
                    payload.try_value()->entry().sha256() != file.contentHash)
                {
                    return Result<PublishedRuntimePackageSnapshot>::failure(make_workflow_error(
                        *assertContext, WorkflowError::ArtifactMismatch,
                        "Build artifact bytes differ from the published inventory"));
                }
                payloads.push_back(std::move(*payload.try_value()));
            }

            const RuntimeDataFile &projectData = a_inputs.runtimeData.project_data();
            Result<PackageFilePayload> projectPayload = PackageFilePayload::create(
                PackageFileRole::ProjectRuntimeData, std::string(projectData.relative_path()),
                copy_bytes(projectData.bytes()), *assertContext);
            const RuntimeDataFile &sceneData = a_inputs.runtimeData.startup_scene_data();
            Result<PackageFilePayload> scenePayload = PackageFilePayload::create(
                PackageFileRole::StartupSceneRuntimeData, std::string(sceneData.relative_path()),
                copy_bytes(sceneData.bytes()), *assertContext);
            if (!projectPayload || !scenePayload)
            {
                return Result<PublishedRuntimePackageSnapshot>::failure(
                    projectPayload ? std::move(*scenePayload.try_error()) : std::move(*projectPayload.try_error()));
            }
            if (projectPayload.try_value()->entry().sha256() != projectData.sha256() ||
                scenePayload.try_value()->entry().sha256() != sceneData.sha256())
            {
                return Result<PublishedRuntimePackageSnapshot>::failure(make_workflow_error(
                    *assertContext, WorkflowError::ArtifactMismatch,
                    "Runtime data bytes differ from the publication snapshot"));
            }
            payloads.push_back(std::move(*projectPayload.try_value()));
            payloads.push_back(std::move(*scenePayload.try_value()));

            std::vector<PackageFileEntry> entries;
            entries.reserve(payloads.size());
            for (const PackageFilePayload &payload : payloads)
            {
                entries.push_back(payload.entry());
            }
            Result<PackageManifest> manifest = PackageManifest::create(
                a_inputs.projectId, a_inputs.engineVersion, a_artifact.configuration(),
                std::string(a_inputs.runtimeData.startup_scene_asset_id()),
                std::string(a_inputs.runtimeData.startup_scene_data().relative_path()), std::move(entries),
                *assertContext);
            if (!manifest)
            {
                return Result<PublishedRuntimePackageSnapshot>::failure(std::move(*manifest.try_error()));
            }

            const std::string packageParent = join_relative("Generated/Packages", configuration);
            Result<RelativePath> parent = RelativePath::parse(packageParent, *assertContext);
            Result<RelativePath> destination =
                RelativePath::parse(join_relative(packageParent, a_operationId), *assertContext);
            if (!parent || !destination)
            {
                return Result<PublishedRuntimePackageSnapshot>::failure(
                    parent ? std::move(*destination.try_error()) : std::move(*parent.try_error()));
            }
            Result<void> directory = projectFilesystem->create_directories(*parent.try_value());
            if (!directory)
            {
                return Result<PublishedRuntimePackageSnapshot>::failure(std::move(*directory.try_error()));
            }
            PackagePublishReport report = publish_runtime_package(*projectFilesystem, *destination.try_value(),
                                                                  *manifest.try_value(), payloads, a_cancellation,
                                                                  *assertContext);
            if (!report.succeeded())
            {
                return Result<PublishedRuntimePackageSnapshot>::failure(
                    report.error ? std::move(*report.error)
                                 : make_workflow_error(*assertContext, WorkflowError::PackagePublicationFailed,
                                                       "Package publication failed without a diagnostic"));
            }
            const std::string packageRoot = join_absolute(projectRoot, destination.try_value()->text());
            return Result<PublishedRuntimePackageSnapshot>::success(
                {std::string(a_operationId), std::string(a_artifact.artifact_id()),
                 std::string(destination.try_value()->text()), join_absolute(packageRoot, "CueRuntimeHost.exe"),
                 std::move(report.manifest)});
        }
        catch (...)
        {
            terminate_workflow_exception(*assertContext);
        }
    }

    GameBuildService *buildService;
    std::unique_ptr<FilesystemRoot> projectFilesystem;
    std::unique_ptr<FilesystemRoot> engineBinaryFilesystem;
    std::unique_ptr<ChildProcessRunner> runProcessRunner;
    std::string projectRoot;
    std::vector<ChildProcessEnvironmentEntry> runEnvironment;
    const AssertContext *assertContext;
    std::thread::id ownerThread;
    mutable std::mutex mutex;
    std::thread worker;
    std::shared_ptr<PackageCancellation> packageCancellation;
    std::shared_ptr<ChildProcessCancellation> processCancellation;
    std::optional<PackageInputs> pendingInputs;
    std::optional<PackageInputs> retryInputs;
    PackageWorkflowSnapshot current;
};

GamePackageWorkflowService::GamePackageWorkflowService(std::unique_ptr<Impl> a_impl) noexcept
    : m_impl(std::move(a_impl))
{
}

GamePackageWorkflowService::~GamePackageWorkflowService()
{
    PackageWorkflowState state = PackageWorkflowState::Idle;
    {
        std::scoped_lock lock(m_impl->mutex);
        state = m_impl->current.state;
        if (m_impl->packageCancellation)
        {
            m_impl->packageCancellation->request_cancel();
        }
        if (m_impl->processCancellation)
        {
            m_impl->processCancellation->request_cancel();
        }
    }
    if (state == PackageWorkflowState::Building)
    {
        static_cast<void>(m_impl->buildService->request_cancel());
        static_cast<void>(m_impl->buildService->wait_for_completion());
    }
    if (m_impl->worker.joinable())
    {
        m_impl->worker.join();
    }
}

Result<std::unique_ptr<GamePackageWorkflowService>> GamePackageWorkflowService::create(
    GameBuildService &a_buildService, std::unique_ptr<FilesystemRoot> a_projectFilesystem,
    std::unique_ptr<FilesystemRoot> a_engineBinaryFilesystem, std::unique_ptr<ChildProcessRunner> a_runProcessRunner,
    std::string a_projectRoot, std::vector<ChildProcessEnvironmentEntry> a_runEnvironment,
    const AssertContext &a_assertContext) noexcept
{
    try
    {
        if (!a_projectFilesystem || !a_engineBinaryFilesystem || !a_runProcessRunner || a_projectRoot.empty())
        {
            return Result<std::unique_ptr<GamePackageWorkflowService>>::failure(make_workflow_error(
                a_assertContext, WorkflowError::MissingDependency, "Package workflow dependency is missing"));
        }
        auto impl = std::make_unique<Impl>(a_buildService, std::move(a_projectFilesystem),
                                           std::move(a_engineBinaryFilesystem), std::move(a_runProcessRunner),
                                           std::move(a_projectRoot), std::move(a_runEnvironment), a_assertContext);
        return Result<std::unique_ptr<GamePackageWorkflowService>>::success(
            std::unique_ptr<GamePackageWorkflowService>(new GamePackageWorkflowService(std::move(impl))));
    }
    catch (...)
    {
        terminate_workflow_exception(a_assertContext);
    }
}

Result<void> GamePackageWorkflowService::start(BuildRequest a_buildRequest, CMakeConfigureMode a_configureMode,
                                               EngineVersion a_engineVersion, std::string a_projectId,
                                               MinimalRuntimeDataPublication a_runtimeData) noexcept
{
    try
    {
        if (!m_impl->is_owner_thread())
        {
            return Result<void>::failure(make_workflow_error(*m_impl->assertContext,
                                                              WorkflowError::OwnerThreadViolation,
                                                              "Package workflow start requires owner thread"));
        }
        advance();
        {
            std::scoped_lock lock(m_impl->mutex);
            if (m_impl->current.state == PackageWorkflowState::Building ||
                m_impl->current.state == PackageWorkflowState::Packaging ||
                m_impl->current.state == PackageWorkflowState::Running)
            {
                return Result<void>::failure(make_workflow_error(*m_impl->assertContext,
                                                                  WorkflowError::OperationAlreadyRunning,
                                                                  "Package workflow operation is already running"));
            }
        }
        Impl::PackageInputs inputs{a_engineVersion, std::move(a_projectId), std::move(a_runtimeData)};
        Result<void> started = m_impl->buildService->start(std::move(a_buildRequest), a_configureMode);
        if (!started)
        {
            return started;
        }
        {
            std::scoped_lock lock(m_impl->mutex);
            m_impl->pendingInputs = inputs;
            m_impl->retryInputs = std::move(inputs);
            m_impl->current.state = PackageWorkflowState::Building;
            m_impl->current.activeStage = PackageWorkflowStage::Build;
            m_impl->current.build = m_impl->buildService->snapshot();
            m_impl->current.package.reset();
            m_impl->current.runOutput.clear();
            m_impl->current.message = "Game Module Buildを開始しました。";
        }
        return Result<void>::success();
    }
    catch (...)
    {
        terminate_workflow_exception(*m_impl->assertContext);
    }
}

Result<void> GamePackageWorkflowService::retry(std::string a_operationId) noexcept
{
    try
    {
        if (!m_impl->is_owner_thread())
        {
            return Result<void>::failure(make_workflow_error(*m_impl->assertContext,
                                                              WorkflowError::OwnerThreadViolation,
                                                              "Package workflow retry requires owner thread"));
        }
        advance();
        std::optional<Impl::PackageInputs> inputs;
        {
            std::scoped_lock lock(m_impl->mutex);
            if (!m_impl->retryInputs)
            {
                return Result<void>::failure(make_workflow_error(*m_impl->assertContext,
                                                                  WorkflowError::NoRetryableOperation,
                                                                  "No package workflow is available for retry"));
            }
            if (m_impl->current.state == PackageWorkflowState::Building ||
                m_impl->current.state == PackageWorkflowState::Packaging ||
                m_impl->current.state == PackageWorkflowState::Running)
            {
                return Result<void>::failure(make_workflow_error(*m_impl->assertContext,
                                                                  WorkflowError::OperationAlreadyRunning,
                                                                  "Package workflow operation is already running"));
            }
            inputs = *m_impl->retryInputs;
        }
        Result<void> restarted = m_impl->buildService->retry(std::move(a_operationId));
        if (!restarted)
        {
            return restarted;
        }
        {
            std::scoped_lock lock(m_impl->mutex);
            m_impl->pendingInputs = std::move(inputs);
            m_impl->current.state = PackageWorkflowState::Building;
            m_impl->current.activeStage = PackageWorkflowStage::Build;
            m_impl->current.build = m_impl->buildService->snapshot();
            m_impl->current.package.reset();
            m_impl->current.runOutput.clear();
            m_impl->current.message = "BuildからPackageまでを再実行しました。";
        }
        return Result<void>::success();
    }
    catch (...)
    {
        terminate_workflow_exception(*m_impl->assertContext);
    }
}

void GamePackageWorkflowService::advance() noexcept
{
    try
    {
        if (!m_impl->is_owner_thread())
        {
            return;
        }
        PackageWorkflowState state;
        {
            std::scoped_lock lock(m_impl->mutex);
            state = m_impl->current.state;
        }
        if (state == PackageWorkflowState::Building)
        {
            BuildOperationSnapshot build = m_impl->buildService->snapshot();
            {
                std::scoped_lock lock(m_impl->mutex);
                m_impl->current.build = build;
            }
            if (build.state != GameBuildOperationState::Running)
            {
                static_cast<void>(m_impl->buildService->wait_for_completion());
                build = m_impl->buildService->snapshot();
                std::optional<Impl::PackageInputs> inputs;
                if (build.state == GameBuildOperationState::Succeeded && build.artifact)
                {
                    std::shared_ptr<PackageCancellation> cancellation = std::make_shared<PackageCancellation>();
                    {
                        std::scoped_lock lock(m_impl->mutex);
                        inputs = std::move(m_impl->pendingInputs);
                        m_impl->packageCancellation = cancellation;
                        m_impl->current.build = build;
                        m_impl->current.state = PackageWorkflowState::Packaging;
                        m_impl->current.activeStage = PackageWorkflowStage::Package;
                        m_impl->current.message = "Runtime DataとStandalone Packageを公開しています。";
                    }
                    if (!inputs)
                    {
                        std::scoped_lock lock(m_impl->mutex);
                        m_impl->packageCancellation.reset();
                        m_impl->current.state = PackageWorkflowState::Failed;
                        m_impl->current.activeStage = PackageWorkflowStage::None;
                        m_impl->current.message = "Package入力が失われました。";
                    }
                    else
                    {
                        const BuildArtifactInventory artifact = *build.artifact;
                        const std::string operationId = build.operationId;
                        m_impl->worker = std::thread(
                            [impl = m_impl.get(), artifact, inputs = std::move(*inputs), operationId, cancellation]()
                            {
                                Result<PublishedRuntimePackageSnapshot> published =
                                    impl->publish_package(artifact, inputs, operationId, *cancellation);
                                std::scoped_lock lock(impl->mutex);
                                impl->packageCancellation.reset();
                                impl->current.activeStage = PackageWorkflowStage::None;
                                if (!published)
                                {
                                    impl->current.state = cancellation->is_cancel_requested()
                                                              ? PackageWorkflowState::Cancelled
                                                              : PackageWorkflowState::Failed;
                                    impl->current.message = error_message(*published.try_error());
                                    return;
                                }
                                impl->current.state = PackageWorkflowState::PackageReady;
                                impl->current.package = *published.try_value();
                                impl->current.latestSuccessfulPackage = std::move(*published.try_value());
                                impl->current.message = "Standalone Packageを公開しました。";
                            });
                    }
                }
                else
                {
                    std::scoped_lock lock(m_impl->mutex);
                    m_impl->pendingInputs.reset();
                    m_impl->current.build = build;
                    m_impl->current.state = build_terminal_state(build.state);
                    m_impl->current.activeStage = PackageWorkflowStage::None;
                    m_impl->current.message = build.state == GameBuildOperationState::Cancelled
                                                  ? "BuildをキャンセルしたためPackageは開始していません。"
                                                  : "Buildに失敗したためPackageは開始していません。";
                }
            }
        }

        bool joinWorker = false;
        {
            std::scoped_lock lock(m_impl->mutex);
            joinWorker = m_impl->worker.joinable() && m_impl->current.state != PackageWorkflowState::Packaging &&
                         m_impl->current.state != PackageWorkflowState::Running;
        }
        if (joinWorker)
        {
            m_impl->worker.join();
        }
    }
    catch (...)
    {
        terminate_workflow_exception(*m_impl->assertContext);
    }
}

Result<void> GamePackageWorkflowService::request_cancel() noexcept
{
    try
    {
        PackageWorkflowState state;
        std::shared_ptr<PackageCancellation> packageCancellation;
        std::shared_ptr<ChildProcessCancellation> processCancellation;
        {
            std::scoped_lock lock(m_impl->mutex);
            state = m_impl->current.state;
            packageCancellation = m_impl->packageCancellation;
            processCancellation = m_impl->processCancellation;
        }
        if (state == PackageWorkflowState::Building)
        {
            return m_impl->buildService->request_cancel();
        }
        if (state == PackageWorkflowState::Packaging && packageCancellation)
        {
            packageCancellation->request_cancel();
            return Result<void>::success();
        }
        if (state == PackageWorkflowState::Running && processCancellation)
        {
            processCancellation->request_cancel();
            return Result<void>::success();
        }
        return Result<void>::failure(make_workflow_error(*m_impl->assertContext, WorkflowError::NoActiveOperation,
                                                          "No active package workflow can be cancelled"));
    }
    catch (...)
    {
        terminate_workflow_exception(*m_impl->assertContext);
    }
}

Result<void> GamePackageWorkflowService::run(PackageRunMode a_mode) noexcept
{
    try
    {
        if (!m_impl->is_owner_thread())
        {
            return Result<void>::failure(make_workflow_error(*m_impl->assertContext,
                                                              WorkflowError::OwnerThreadViolation,
                                                              "Package run requires owner thread"));
        }
        advance();
        std::optional<PublishedRuntimePackageSnapshot> package;
        std::shared_ptr<ChildProcessCancellation> cancellation = std::make_shared<ChildProcessCancellation>();
        {
            std::scoped_lock lock(m_impl->mutex);
            if (m_impl->current.state != PackageWorkflowState::PackageReady &&
                m_impl->current.state != PackageWorkflowState::RunSucceeded)
            {
                return Result<void>::failure(make_workflow_error(*m_impl->assertContext,
                                                                  WorkflowError::NoPublishedPackage,
                                                                  "No published Package is ready to run"));
            }
            package = m_impl->current.package;
            if (!package)
            {
                return Result<void>::failure(make_workflow_error(*m_impl->assertContext,
                                                                  WorkflowError::NoPublishedPackage,
                                                                  "Published Package snapshot is missing"));
            }
            m_impl->processCancellation = cancellation;
            m_impl->current.state = PackageWorkflowState::Running;
            m_impl->current.activeStage = PackageWorkflowStage::Run;
            m_impl->current.runOutput.clear();
            m_impl->current.message = "Standalone Runtimeを起動しました。";
        }
        const std::string workingDirectory = join_absolute(m_impl->projectRoot, package->destination);
        const std::vector<std::string> arguments{
            a_mode == PackageRunMode::SmokeTest ? "--package-smoke-test" : "--package"};
        ChildProcessRequest request(package->executable, arguments, workingDirectory, m_impl->runEnvironment,
                                    std::nullopt);
        m_impl->worker = std::thread([impl = m_impl.get(), request = std::move(request), cancellation]()
        {
            Result<ChildProcessResult> runResult = impl->runProcessRunner->run(request, *cancellation);
            std::scoped_lock lock(impl->mutex);
            impl->processCancellation.reset();
            impl->current.activeStage = PackageWorkflowStage::None;
            if (!runResult)
            {
                impl->current.state = PackageWorkflowState::Failed;
                impl->current.message = error_message(*runResult.try_error());
                return;
            }
            impl->current.runOutput = runResult.try_value()->output();
            if (runResult.try_value()->outcome() == ChildProcessOutcome::Cancelled)
            {
                impl->current.state = PackageWorkflowState::PackageReady;
                impl->current.message = "Standalone Runtimeを停止しました。";
                return;
            }
            if (runResult.try_value()->outcome() == ChildProcessOutcome::Exited &&
                runResult.try_value()->exit_code() == std::optional<std::uint32_t>(0U))
            {
                impl->current.state = PackageWorkflowState::RunSucceeded;
                impl->current.message = "Standalone Runtimeが正常終了しました。";
                return;
            }
            impl->current.state = PackageWorkflowState::Failed;
            impl->current.message = "Standalone Runtimeが異常終了しました。";
        });
        return Result<void>::success();
    }
    catch (...)
    {
        terminate_workflow_exception(*m_impl->assertContext);
    }
}

Result<void> GamePackageWorkflowService::stop() noexcept
{
    return request_cancel();
}

Result<void> GamePackageWorkflowService::wait_for_package() noexcept
{
    if (!m_impl->is_owner_thread())
    {
        return Result<void>::failure(make_workflow_error(*m_impl->assertContext, WorkflowError::OwnerThreadViolation,
                                                          "Package wait requires owner thread"));
    }
    for (;;)
    {
        advance();
        const PackageWorkflowState state = snapshot().state;
        if (state != PackageWorkflowState::Building && state != PackageWorkflowState::Packaging)
        {
            return Result<void>::success();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

Result<void> GamePackageWorkflowService::wait_for_run_completion() noexcept
{
    if (!m_impl->is_owner_thread())
    {
        return Result<void>::failure(make_workflow_error(*m_impl->assertContext, WorkflowError::OwnerThreadViolation,
                                                          "Package run wait requires owner thread"));
    }
    for (;;)
    {
        advance();
        if (snapshot().state != PackageWorkflowState::Running)
        {
            return Result<void>::success();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

PackageWorkflowSnapshot GamePackageWorkflowService::snapshot() const noexcept
{
    try
    {
        std::scoped_lock lock(m_impl->mutex);
        return m_impl->current;
    }
    catch (...)
    {
        terminate_workflow_exception(*m_impl->assertContext);
    }
}
} // namespace cue::package
