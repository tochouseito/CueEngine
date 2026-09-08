#include <Cue/Build/Service.h>

#include <Cue/Foundation/Assert.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>

namespace
{
/// @brief 一Artifact Versionへ記録できるFile数の上限
constexpr std::size_t k_maximumArtifactFiles = 128U;
/// @brief JSON整数として情報を失わず表現できるArtifact File Size上限
constexpr std::uint64_t k_maximumArtifactByteSize = 9007199254740991ULL;

/// @brief 回復不能なService内部例外をFatalHandlerへ通知してProcessを停止する
[[noreturn]] void terminate_service_exception(const cue::AssertContext &a_assertContext) noexcept
{
    a_assertContext.fatal_handler().terminate("Game build service failed unexpectedly");
    std::abort();
}

/// @brief Service固有の回復可能Errorを一貫したDomainで構築する
[[nodiscard]] cue::Error make_service_error(const cue::AssertContext &a_assertContext,
                                            cue::GameBuildServiceError a_code, std::string_view a_summary) noexcept
{
    cue::ErrorCode code =
        cue::ErrorCode::create(a_assertContext.fatal_handler(), "Cue.Build.Service", static_cast<std::int64_t>(a_code));
    return cue::Error::create(a_assertContext.fatal_handler(), std::move(code), a_summary);
}

/// @brief Artifact IDとHashで許可するlowercase hexadecimal文字か判定する
[[nodiscard]] bool is_lower_hex(char a_value) noexcept
{
    return (a_value >= '0' && a_value <= '9') || (a_value >= 'a' && a_value <= 'f');
}

/// @brief Artifact IDがlowercase UUID v4のCanonical形式か検証する
[[nodiscard]] bool is_uuid_v4(std::string_view a_text) noexcept
{
    if (a_text.size() != 36U || a_text[8] != '-' || a_text[13] != '-' || a_text[18] != '-' || a_text[23] != '-' ||
        a_text[14] != '4' || (a_text[19] != '8' && a_text[19] != '9' && a_text[19] != 'a' && a_text[19] != 'b'))
    {
        return false;
    }
    for (std::size_t index = 0U; index < a_text.size(); ++index)
    {
        if (index != 8U && index != 13U && index != 18U && index != 23U && !is_lower_hex(a_text[index]))
        {
            return false;
        }
    }
    return true;
}

/// @brief Artifact Root外参照とWindowsで危険な要素を含まない相対Pathか検証する
[[nodiscard]] bool is_safe_relative_path(std::string_view a_path) noexcept
{
    if (a_path.empty() || a_path.front() == '/' || a_path.back() == '/' ||
        a_path.find('\\') != std::string_view::npos || a_path.find('\0') != std::string_view::npos)
    {
        return false;
    }
    std::size_t begin = 0U;
    while (begin < a_path.size())
    {
        const std::size_t end = a_path.find('/', begin);
        const std::string_view component =
            a_path.substr(begin, end == std::string_view::npos ? a_path.size() - begin : end - begin);
        if (component.empty() || component == "." || component == "..")
        {
            return false;
        }
        if (component.back() == ' ' || component.back() == '.')
        {
            return false;
        }
        for (const unsigned char value : component)
        {
            if (value < 0x20U || value == ':' || value == '*' || value == '?' || value == '"' || value == '<' ||
                value == '>' || value == '|')
            {
                return false;
            }
        }
        if (end == std::string_view::npos)
        {
            break;
        }
        begin = end + 1U;
    }
    return true;
}

/// @brief Windows上で重複するASCII大小文字違いのPathを検出する
[[nodiscard]] bool equals_path_ascii_case_insensitive(std::string_view a_left, std::string_view a_right) noexcept
{
    if (a_left.size() != a_right.size())
    {
        return false;
    }
    for (std::size_t index = 0U; index < a_left.size(); ++index)
    {
        const unsigned char left = static_cast<unsigned char>(a_left[index]);
        const unsigned char right = static_cast<unsigned char>(a_right[index]);
        const unsigned char foldedLeft =
            left >= 'A' && left <= 'Z' ? static_cast<unsigned char>(left - 'A' + 'a') : left;
        const unsigned char foldedRight =
            right >= 'A' && right <= 'Z' ? static_cast<unsigned char>(right - 'A' + 'a') : right;
        if (foldedLeft != foldedRight)
        {
            return false;
        }
    }
    return true;
}

/// @brief Content Hashがlowercase SHA-256文字列表現か検証する
[[nodiscard]] bool has_valid_hash(std::string_view a_hash) noexcept
{
    return a_hash.size() == 64U && std::all_of(a_hash.begin(), a_hash.end(), is_lower_hex);
}

/// @brief Artifact Fileを決定的な相対Path順へ整列する比較結果を返す
[[nodiscard]] bool artifact_path_less(const cue::BuildArtifactFile &a_left,
                                      const cue::BuildArtifactFile &a_right) noexcept
{
    return a_left.relativePath < a_right.relativePath;
}

/// @brief Native ErrorがあればUI再表示可能な所有Snapshotへ変換する
[[nodiscard]] std::optional<cue::BuildNativeErrorSnapshot> flatten_native_error(const cue::NativeError *a_nativeError)
{
    if (a_nativeError == nullptr)
    {
        return std::nullopt;
    }
    return cue::BuildNativeErrorSnapshot{std::string(a_nativeError->domain()), a_nativeError->value()};
}

/// @brief 所有関係を持つError ChainをUI再表示可能な診断値へ平坦化する
[[nodiscard]] std::vector<cue::BuildDiagnosticSnapshot> flatten_error(const cue::Error &a_error)
{
    std::vector<cue::BuildDiagnosticSnapshot> diagnostics;
    diagnostics.reserve(a_error.causes().size() + 1U);
    std::vector<std::string> primaryContexts;
    primaryContexts.reserve(a_error.contexts().size());
    for (const cue::ErrorContext &context : a_error.contexts())
    {
        primaryContexts.emplace_back(context.message());
    }
    diagnostics.push_back({std::string(a_error.code().domain()), a_error.code().value(), std::string(a_error.summary()),
                           std::move(primaryContexts), flatten_native_error(a_error.try_native_error())});
    for (const cue::ErrorCause &cause : a_error.causes())
    {
        std::vector<std::string> contexts;
        contexts.reserve(cause.contexts().size());
        for (const cue::ErrorContext &context : cause.contexts())
        {
            contexts.emplace_back(context.message());
        }
        diagnostics.push_back({std::string(cause.code().domain()), cause.code().value(), std::string(cause.summary()),
                               std::move(contexts), flatten_native_error(cause.try_native_error())});
    }
    return diagnostics;
}
} // namespace

namespace cue
{
BuildArtifactInventory::BuildArtifactInventory(std::string a_artifactId, BuildConfiguration a_configuration,
                                               std::string a_versionDirectory,
                                               std::vector<BuildArtifactFile> a_files) noexcept
    : m_artifactId(std::move(a_artifactId)), m_configuration(a_configuration),
      m_versionDirectory(std::move(a_versionDirectory)), m_files(std::move(a_files))
{
}

Result<BuildArtifactInventory> BuildArtifactInventory::create(const BuildPlan &a_plan, std::string a_artifactId,
                                                              std::vector<BuildArtifactFile> a_files,
                                                              const AssertContext &a_assertContext) noexcept
{
    try
    {
        if (!is_uuid_v4(a_artifactId) || a_files.empty() || a_files.size() > k_maximumArtifactFiles)
        {
            return Result<BuildArtifactInventory>::failure(
                make_service_error(a_assertContext, GameBuildServiceError::InvalidArtifact,
                                   "Build artifact identity or file count is invalid"));
        }
        std::sort(a_files.begin(), a_files.end(), artifact_path_less);
        bool hasModule = false;
        bool hasMetadata = false;
        for (std::size_t index = 0U; index < a_files.size(); ++index)
        {
            const BuildArtifactFile &file = a_files[index];
            bool duplicatePath = false;
            for (std::size_t previous = 0U; previous < index; ++previous)
            {
                duplicatePath = duplicatePath ||
                                equals_path_ascii_case_insensitive(a_files[previous].relativePath, file.relativePath);
            }
            if (!is_safe_relative_path(file.relativePath) || !has_valid_hash(file.contentHash) || duplicatePath ||
                file.byteSize > k_maximumArtifactByteSize)
            {
                return Result<BuildArtifactInventory>::failure(
                    make_service_error(a_assertContext, GameBuildServiceError::InvalidArtifact,
                                       "Build artifact file inventory is invalid"));
            }
            if (file.relativePath == "CueGameModule.dll")
            {
                hasModule = file.byteSize > 0U;
            }
            else if (file.relativePath == "CueGameModule.metadata.json")
            {
                hasMetadata = file.byteSize > 0U;
            }
        }
        if (!hasModule || !hasMetadata)
        {
            return Result<BuildArtifactInventory>::failure(make_service_error(
                a_assertContext, GameBuildServiceError::InvalidArtifact, "Build artifact is missing required files"));
        }
        std::string versionDirectory(a_plan.artifact_store_directory());
        versionDirectory.append("/Versions/");
        versionDirectory.append(a_artifactId);
        return Result<BuildArtifactInventory>::success(
            BuildArtifactInventory(std::move(a_artifactId), a_plan.profile().configuration(),
                                   std::move(versionDirectory), std::move(a_files)));
    }
    catch (...)
    {
        terminate_service_exception(a_assertContext);
    }
}

std::string_view BuildArtifactInventory::artifact_id() const noexcept
{
    return m_artifactId;
}

BuildConfiguration BuildArtifactInventory::configuration() const noexcept
{
    return m_configuration;
}

std::string_view BuildArtifactInventory::version_directory() const noexcept
{
    return m_versionDirectory;
}

std::span<const BuildArtifactFile> BuildArtifactInventory::files() const noexcept
{
    return m_files;
}

struct GameBuildService::Impl final
{
    class Observer final : public CMakeStageObserver
    {
      public:
        /// @brief Stage通知をOperation ID付きで所有Serviceへ転送するObserverを構築する
        Observer(Impl &a_owner, std::string_view a_operationId) noexcept
            : m_owner(&a_owner), m_operationId(a_operationId)
        {
        }

        /// @brief 現在OperationのActive Stageを安全に更新する
        void on_stage_started(BuildStage a_stage) noexcept override
        {
            m_owner->stage_started(m_operationId, a_stage);
        }

        /// @brief 完了Stageの出力を現在OperationのLogへ関連付ける
        void on_stage_completed(const CMakeStageRecord &a_record) noexcept override
        {
            m_owner->stage_completed(m_operationId, a_record);
        }

      private:
        Impl *m_owner;
        std::string m_operationId;
    };

    /// @brief 注入依存とOwner Threadを記録してIdle Service状態を準備する
    Impl(CMakeRunnerSettings a_settings, std::unique_ptr<ChildProcessRunner> a_processRunner,
         std::unique_ptr<BuildArtifactPublisher> a_artifactPublisher, const AssertContext &a_assertContext) noexcept
        : settings(std::move(a_settings)), processRunner(std::move(a_processRunner)),
          artifactPublisher(std::move(a_artifactPublisher)), assertContext(&a_assertContext),
          ownerThread(std::this_thread::get_id())
    {
    }

    /// @brief Owner Thread限定操作を呼出Threadから検証する
    [[nodiscard]] bool is_owner_thread() const noexcept
    {
        return std::this_thread::get_id() == ownerThread;
    }

    /// @brief 一致する実行中OperationだけActive Stageを更新する
    void stage_started(std::string_view a_operationId, BuildStage a_stage) noexcept
    {
        try
        {
            std::scoped_lock lock(mutex);
            if (current.state == GameBuildOperationState::Running && current.operationId == a_operationId)
            {
                current.activeStage = a_stage;
            }
        }
        catch (...)
        {
            terminate_service_exception(*assertContext);
        }
    }

    /// @brief 一致する実行中Operationだけ完了StageのProcess出力を記録する
    void stage_completed(std::string_view a_operationId, const CMakeStageRecord &a_record) noexcept
    {
        try
        {
            std::scoped_lock lock(mutex);
            if (current.state != GameBuildOperationState::Running || current.operationId != a_operationId)
            {
                return;
            }
            current.stages.push_back({a_record.result.stage(), a_record.result.outcome(), a_record.result.exit_code()});
            for (const ChildProcessOutputChunk &chunk : a_record.output)
            {
                current.logs.push_back(
                    {std::string(a_operationId), a_record.result.stage(), chunk.sequence, chunk.stream, chunk.bytes});
            }
        }
        catch (...)
        {
            terminate_service_exception(*assertContext);
        }
    }

    /// @brief Error Chainを診断へ変換して一致するOperationを失敗状態へ確定する
    void finish_error(std::string_view a_operationId, const Error &a_error) noexcept
    {
        try
        {
            std::vector<BuildDiagnosticSnapshot> diagnostics = flatten_error(a_error);
            std::scoped_lock lock(mutex);
            if (current.state == GameBuildOperationState::Running && current.operationId == a_operationId)
            {
                current.activeStage.reset();
                current.diagnostics = std::move(diagnostics);
                current.state = GameBuildOperationState::Failed;
            }
        }
        catch (...)
        {
            terminate_service_exception(*assertContext);
        }
    }

    /// @brief 一致するOperationをArtifact更新なしの取消状態へ確定する
    void finish_cancelled(std::string_view a_operationId) noexcept
    {
        try
        {
            std::scoped_lock lock(mutex);
            if (current.state == GameBuildOperationState::Running && current.operationId == a_operationId)
            {
                current.activeStage.reset();
                current.state = GameBuildOperationState::Cancelled;
            }
        }
        catch (...)
        {
            terminate_service_exception(*assertContext);
        }
    }

    /// @brief Runner結果と公開Artifactから一致するOperationの終端状態を確定する
    void finish_result(std::string_view a_operationId, const CMakeBuildResult &a_result,
                       std::optional<BuildArtifactInventory> a_artifact) noexcept
    {
        try
        {
            GameBuildOperationState state = GameBuildOperationState::Failed;
            if (a_result.succeeded() && a_artifact)
            {
                state = GameBuildOperationState::Succeeded;
            }
            else if (!a_result.stages().empty())
            {
                switch (a_result.stages().back().result.outcome())
                {
                case BuildStageOutcome::Succeeded:
                case BuildStageOutcome::Failed:
                    state = GameBuildOperationState::Failed;
                    break;
                case BuildStageOutcome::Cancelled:
                    state = GameBuildOperationState::Cancelled;
                    break;
                case BuildStageOutcome::TimedOut:
                    state = GameBuildOperationState::TimedOut;
                    break;
                }
            }
            std::scoped_lock lock(mutex);
            if (current.state != GameBuildOperationState::Running || current.operationId != a_operationId)
            {
                return;
            }
            current.activeStage.reset();
            current.state = state;
            if (a_artifact)
            {
                latestSuccessful = *a_artifact;
                current.artifact = std::move(*a_artifact);
                current.latestSuccessfulArtifact = latestSuccessful;
            }
        }
        catch (...)
        {
            terminate_service_exception(*assertContext);
        }
    }

    /// @brief ConfigureとBuildを順に実行し、成功時だけArtifactを公開する
    void execute(BuildPlan a_plan, std::string a_operationId, CMakeConfigureMode a_configureMode,
                 ChildProcessCancellation &a_cancellation) noexcept
    {
        Observer observer(*this, a_operationId);
        auto acquired = artifactPublisher->acquire_build_lease(a_plan, a_cancellation);
        if (!acquired)
        {
            finish_error(a_operationId, *acquired.try_error());
            return;
        }
        if (!acquired.try_value()->has_value())
        {
            finish_cancelled(a_operationId);
            return;
        }
        std::unique_ptr<BuildWorkspaceLease> buildLease = std::move(**acquired.try_value());
        auto built = run_cmake_build(a_plan, settings, a_configureMode, *processRunner, a_cancellation, observer,
                                     *assertContext);
        if (!built)
        {
            finish_error(a_operationId, *built.try_error());
            return;
        }
        if (!built.try_value()->succeeded())
        {
            finish_result(a_operationId, *built.try_value(), std::nullopt);
            return;
        }
        if (a_cancellation.is_cancel_requested())
        {
            finish_cancelled(a_operationId);
            return;
        }
        auto published = artifactPublisher->publish(a_plan, a_cancellation, std::move(buildLease));
        if (!published)
        {
            ErrorCode code =
                ErrorCode::create(assertContext->fatal_handler(), "Cue.Build.Service",
                                  static_cast<std::int64_t>(GameBuildServiceError::ArtifactPublicationFailed));
            Error classified =
                Error::reclassify(assertContext->fatal_handler(), std::move(code), "Build artifact publication failed",
                                  std::move(*published.try_error()));
            finish_error(a_operationId, classified);
            return;
        }
        if (!published.try_value()->has_value())
        {
            finish_cancelled(a_operationId);
            return;
        }
        finish_result(a_operationId, *built.try_value(), std::move(**published.try_value()));
    }

    CMakeRunnerSettings settings;
    std::unique_ptr<ChildProcessRunner> processRunner;
    std::unique_ptr<BuildArtifactPublisher> artifactPublisher;
    const AssertContext *assertContext;
    std::thread::id ownerThread;
    mutable std::mutex mutex;
    std::thread worker;
    std::unique_ptr<ChildProcessCancellation> cancellation;
    BuildOperationSnapshot current;
    std::optional<BuildArtifactInventory> latestSuccessful;
    std::optional<BuildRequest> lastRequest;
};

GameBuildService::GameBuildService(std::unique_ptr<Impl> a_impl) noexcept : m_impl(std::move(a_impl))
{
}

GameBuildService::~GameBuildService()
{
    try
    {
        if (!m_impl)
        {
            return;
        }
        {
            std::scoped_lock lock(m_impl->mutex);
            if (m_impl->cancellation)
            {
                m_impl->cancellation->request_cancel();
            }
        }
        if (m_impl->worker.joinable())
        {
            m_impl->worker.join();
        }
    }
    catch (...)
    {
        terminate_service_exception(*m_impl->assertContext);
    }
}

Result<std::unique_ptr<GameBuildService>> GameBuildService::create(
    CMakeRunnerSettings a_settings, std::unique_ptr<ChildProcessRunner> a_processRunner,
    std::unique_ptr<BuildArtifactPublisher> a_artifactPublisher, const AssertContext &a_assertContext) noexcept
{
    try
    {
        if (!a_processRunner || !a_artifactPublisher)
        {
            return Result<std::unique_ptr<GameBuildService>>::failure(make_service_error(
                a_assertContext, GameBuildServiceError::MissingDependency, "Build service dependencies are missing"));
        }
        auto implementation = std::make_unique<Impl>(std::move(a_settings), std::move(a_processRunner),
                                                     std::move(a_artifactPublisher), a_assertContext);
        return Result<std::unique_ptr<GameBuildService>>::success(
            std::unique_ptr<GameBuildService>(new GameBuildService(std::move(implementation))));
    }
    catch (...)
    {
        terminate_service_exception(a_assertContext);
    }
}

Result<void> GameBuildService::start(BuildRequest a_request, CMakeConfigureMode a_configureMode) noexcept
{
    try
    {
        if (!m_impl->is_owner_thread())
        {
            return Result<void>::failure(make_service_error(*m_impl->assertContext,
                                                            GameBuildServiceError::OwnerThreadViolation,
                                                            "Build start requires owner thread"));
        }
        {
            std::scoped_lock lock(m_impl->mutex);
            if (m_impl->current.state == GameBuildOperationState::Running)
            {
                return Result<void>::failure(make_service_error(*m_impl->assertContext,
                                                                GameBuildServiceError::OperationAlreadyRunning,
                                                                "A build operation is already running"));
            }
        }
        if (m_impl->worker.joinable())
        {
            m_impl->worker.join();
        }
        auto plan = create_build_plan(a_request, *m_impl->assertContext);
        if (!plan)
        {
            return Result<void>::failure(std::move(*plan.try_error()));
        }
        auto cancellation = std::make_unique<ChildProcessCancellation>();
        ChildProcessCancellation *cancellationPointer = cancellation.get();
        const std::string operationId(a_request.operationId);
        {
            std::scoped_lock lock(m_impl->mutex);
            m_impl->lastRequest = a_request;
            m_impl->cancellation = std::move(cancellation);
            m_impl->current = {};
            m_impl->current.state = GameBuildOperationState::Running;
            m_impl->current.operationId = operationId;
            m_impl->current.profile = a_request.profile;
            m_impl->current.latestSuccessfulArtifact = m_impl->latestSuccessful;
        }
        m_impl->worker = std::thread(&Impl::execute, m_impl.get(), std::move(*plan.try_value()), operationId,
                                     a_configureMode, std::ref(*cancellationPointer));
        return Result<void>::success();
    }
    catch (...)
    {
        terminate_service_exception(*m_impl->assertContext);
    }
}

Result<void> GameBuildService::retry(std::string a_operationId) noexcept
{
    try
    {
        if (!m_impl->is_owner_thread())
        {
            return Result<void>::failure(make_service_error(*m_impl->assertContext,
                                                            GameBuildServiceError::OwnerThreadViolation,
                                                            "Build retry requires owner thread"));
        }
        std::optional<BuildRequest> request;
        {
            std::scoped_lock lock(m_impl->mutex);
            if (m_impl->current.state == GameBuildOperationState::Running)
            {
                return Result<void>::failure(make_service_error(*m_impl->assertContext,
                                                                GameBuildServiceError::OperationAlreadyRunning,
                                                                "A build operation is already running"));
            }
            if (m_impl->lastRequest)
            {
                request = m_impl->lastRequest;
            }
        }
        if (!request)
        {
            return Result<void>::failure(make_service_error(*m_impl->assertContext,
                                                            GameBuildServiceError::NoRetryableOperation,
                                                            "No build operation is available for retry"));
        }
        request->operationId = std::move(a_operationId);
        return start(std::move(*request), CMakeConfigureMode::Required);
    }
    catch (...)
    {
        terminate_service_exception(*m_impl->assertContext);
    }
}

Result<void> GameBuildService::request_cancel() noexcept
{
    try
    {
        std::scoped_lock lock(m_impl->mutex);
        if (m_impl->current.state != GameBuildOperationState::Running || !m_impl->cancellation)
        {
            return Result<void>::failure(make_service_error(*m_impl->assertContext,
                                                            GameBuildServiceError::NoActiveOperation,
                                                            "No active build operation can be cancelled"));
        }
        m_impl->cancellation->request_cancel();
        return Result<void>::success();
    }
    catch (...)
    {
        terminate_service_exception(*m_impl->assertContext);
    }
}

BuildOperationSnapshot GameBuildService::snapshot() const noexcept
{
    try
    {
        std::scoped_lock lock(m_impl->mutex);
        return m_impl->current;
    }
    catch (...)
    {
        terminate_service_exception(*m_impl->assertContext);
    }
}

Result<void> GameBuildService::wait_for_completion() noexcept
{
    try
    {
        if (!m_impl->is_owner_thread())
        {
            return Result<void>::failure(make_service_error(*m_impl->assertContext,
                                                            GameBuildServiceError::OwnerThreadViolation,
                                                            "Build wait requires owner thread"));
        }
        if (m_impl->worker.joinable())
        {
            m_impl->worker.join();
        }
        return Result<void>::success();
    }
    catch (...)
    {
        terminate_service_exception(*m_impl->assertContext);
    }
}
} // namespace cue
