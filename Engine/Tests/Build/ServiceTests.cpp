#include <Cue/Build/Service.h>

#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Fatal.h>
#include <Cue/Foundation/Log.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace
{
constexpr cue::BuildWorkspaceCompatibility k_workspaceCompatibility{
    cue::BuildGenerator::VisualStudio2026, cue::BuildArchitecture::X64, {19U, 51U, 0U, 0U}, 1U};

class TestFatalHandler final : public cue::FatalHandler
{
  public:
    /// @brief 想定外の引数なしFatal終了を固有Exit Codeで検出する
    [[noreturn]] void terminate() noexcept override
    {
        std::_Exit(90);
    }
    /// @brief 想定外のMessage付きFatal終了を固有Exit Codeで検出する
    [[noreturn]] void terminate(std::string_view) noexcept override
    {
        std::_Exit(91);
    }
};

enum class RunnerMode : std::uint8_t
{
    BlockUntilCancelled,
    Succeed,
    Fail
};

struct RunnerState final
{
    std::atomic<RunnerMode> mode = RunnerMode::BlockUntilCancelled;
    std::atomic<std::uint32_t> calls = 0U;
    std::atomic<bool> processActive = false;
};

class ControlledRunner final : public cue::ChildProcessRunner
{
  public:
    /// @brief Testが共有するProcess状態を借用して制御可能Runnerを構築する
    explicit ControlledRunner(RunnerState &a_state) noexcept : m_state(&a_state)
    {
    }

    /// @brief 指定Modeに従う成功、失敗、Cancel結果と順序付きLogを返す
    [[nodiscard]] cue::Result<cue::ChildProcessResult> run(
        const cue::ChildProcessRequest &, const cue::ChildProcessCancellation &a_cancellation) noexcept override
    {
        m_state->processActive.store(true, std::memory_order_release);
        const std::uint32_t call = m_state->calls.fetch_add(1U, std::memory_order_relaxed);
        while (m_state->mode.load(std::memory_order_acquire) == RunnerMode::BlockUntilCancelled &&
               !a_cancellation.is_cancel_requested())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        m_state->processActive.store(false, std::memory_order_release);
        if (a_cancellation.is_cancel_requested())
        {
            return cue::Result<cue::ChildProcessResult>::success(cue::ChildProcessResult::cancelled({}));
        }
        const std::uint32_t exitCode = m_state->mode.load(std::memory_order_acquire) == RunnerMode::Fail ? 2U : 0U;
        return cue::Result<cue::ChildProcessResult>::success(cue::ChildProcessResult::exited(
            exitCode, {{call, cue::ChildProcessStream::StandardOutput, "log-" + std::to_string(call)}}));
    }

  private:
    RunnerState *m_state;
};

struct PublisherState final
{
    std::atomic<std::uint32_t> calls = 0U;
};

class TestPublisher final : public cue::BuildArtifactPublisher
{
  public:
    /// @brief Publish回数とAssert境界を借用して検証用Publisherを構築する
    TestPublisher(PublisherState &a_state, const cue::AssertContext &a_assertContext) noexcept
        : m_state(&a_state), m_assertContext(&a_assertContext)
    {
    }

    /// @brief 必須Fileを含む決定的な検証用Artifact Inventoryを返す
    [[nodiscard]] cue::Result<cue::BuildArtifactInventory> publish(const cue::BuildPlan &a_plan) noexcept override
    {
        m_state->calls.fetch_add(1U, std::memory_order_relaxed);
        return cue::BuildArtifactInventory::create(a_plan, "11234567-89ab-4cde-8f01-23456789abcd",
                                                   {{"CueGameModule.dll", 256U, std::string(64U, 'a')},
                                                    {"CueGameModule.metadata.json", 64U, std::string(64U, 'b')}},
                                                   *m_assertContext);
    }

  private:
    PublisherState *m_state;
    const cue::AssertContext *m_assertContext;
};

/// @brief Process起動を行わないTest用のAbsolute Runner設定を返す
[[nodiscard]] cue::CMakeRunnerSettings make_settings()
{
    return {"C:/Tools/cmake.exe", "C:/CueEngine", {}, std::chrono::seconds(5), std::chrono::seconds(5)};
}

/// @brief Current DirectoryをProject Rootとする検証済みBuild Request入力を返す
[[nodiscard]] cue::BuildRequest make_request(std::string a_operationId, const cue::AssertContext &a_assertContext)
{
    auto profile =
        cue::BuildProfile::create(cue::BuildConfiguration::Debug, cue::BuildTarget::GameModule, a_assertContext);
    return {std::filesystem::current_path().generic_string(), *profile.try_value(), std::move(a_operationId),
            k_workspaceCompatibility};
}

/// @brief 非同期WorkerがRunningへ遷移するまで上限付きで待機する
[[nodiscard]] bool wait_until_running(cue::GameBuildService &a_service)
{
    for (std::uint32_t attempt = 0U; attempt < 1000U; ++attempt)
    {
        if (a_service.snapshot().state == cue::GameBuildOperationState::Running)
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

/// @brief Artifact Modelが必須File、Hash、Pathと決定順を検証するか確認する
[[nodiscard]] bool test_artifact_model(const cue::AssertContext &a_assertContext)
{
    auto profile =
        cue::BuildProfile::create(cue::BuildConfiguration::Release, cue::BuildTarget::GameModule, a_assertContext);
    cue::BuildRequest request{std::filesystem::current_path().generic_string(), *profile.try_value(),
                              "01234567-89ab-4cde-8f01-23456789abcd", k_workspaceCompatibility};
    auto plan = cue::create_build_plan(request, a_assertContext);
    auto valid = cue::BuildArtifactInventory::create(*plan.try_value(), "11234567-89ab-4cde-8f01-23456789abcd",
                                                     {{"CueGameModule.metadata.json", 10U, std::string(64U, 'b')},
                                                      {"CueGameModule.dll", 20U, std::string(64U, 'a')}},
                                                     a_assertContext);
    auto traversal = cue::BuildArtifactInventory::create(*plan.try_value(), "21234567-89ab-4cde-8f01-23456789abcd",
                                                         {{"../CueGameModule.dll", 20U, std::string(64U, 'a')},
                                                          {"CueGameModule.metadata.json", 10U, std::string(64U, 'b')}},
                                                         a_assertContext);
    auto missing =
        cue::BuildArtifactInventory::create(*plan.try_value(), "31234567-89ab-4cde-8f01-23456789abcd",
                                            {{"CueGameModule.dll", 20U, std::string(64U, 'a')}}, a_assertContext);
    auto emptyMetadata = cue::BuildArtifactInventory::create(
        *plan.try_value(), "41234567-89ab-4cde-8f01-23456789abcd",
        {{"CueGameModule.dll", 20U, std::string(64U, 'a')}, {"CueGameModule.metadata.json", 0U, std::string(64U, 'b')}},
        a_assertContext);
    return valid && valid.try_value()->configuration() == cue::BuildConfiguration::Release &&
           valid.try_value()->files()[0].relativePath == "CueGameModule.dll" &&
           valid.try_value()->version_directory().ends_with(valid.try_value()->artifact_id()) && !traversal &&
           !missing && !emptyMetadata;
}

/// @brief 単一Active、Cancel、Retry、Latest成功Artifact保全をHeadless検証する
[[nodiscard]] bool test_service_lifecycle(const cue::AssertContext &a_assertContext)
{
    RunnerState runnerState;
    PublisherState publisherState;
    auto created = cue::GameBuildService::create(make_settings(), std::make_unique<ControlledRunner>(runnerState),
                                                 std::make_unique<TestPublisher>(publisherState, a_assertContext),
                                                 a_assertContext);
    if (!created)
    {
        return false;
    }
    std::unique_ptr<cue::GameBuildService> service = std::move(*created.try_value());
    auto started = service->start(make_request("01234567-89ab-4cde-8f01-23456789abcd", a_assertContext),
                                  cue::CMakeConfigureMode::Required);
    if (!started || !wait_until_running(*service) ||
        service->start(make_request("11234567-89ab-4cde-8f01-23456789abcd", a_assertContext),
                       cue::CMakeConfigureMode::Required))
    {
        return false;
    }
    if (!service->request_cancel() || !service->wait_for_completion())
    {
        return false;
    }
    cue::BuildOperationSnapshot cancelled = service->snapshot();
    if (cancelled.state != cue::GameBuildOperationState::Cancelled || cancelled.artifact ||
        cancelled.operationId != "01234567-89ab-4cde-8f01-23456789abcd")
    {
        return false;
    }

    runnerState.mode.store(RunnerMode::Succeed, std::memory_order_release);
    if (!service->retry("21234567-89ab-4cde-8f01-23456789abcd") || !service->wait_for_completion())
    {
        return false;
    }
    cue::BuildOperationSnapshot succeeded = service->snapshot();
    if (succeeded.state != cue::GameBuildOperationState::Succeeded || !succeeded.artifact ||
        !succeeded.latestSuccessfulArtifact || succeeded.logs.size() != 2U || publisherState.calls != 1U)
    {
        return false;
    }
    for (const cue::BuildLogSnapshot &log : succeeded.logs)
    {
        if (log.operationId != "21234567-89ab-4cde-8f01-23456789abcd")
        {
            return false;
        }
    }

    runnerState.mode.store(RunnerMode::Fail, std::memory_order_release);
    if (!service->start(make_request("31234567-89ab-4cde-8f01-23456789abcd", a_assertContext),
                        cue::CMakeConfigureMode::Required) ||
        !service->wait_for_completion())
    {
        return false;
    }
    cue::BuildOperationSnapshot failed = service->snapshot();
    return failed.state == cue::GameBuildOperationState::Failed && !failed.artifact &&
           failed.latestSuccessfulArtifact &&
           failed.latestSuccessfulArtifact->artifact_id() == succeeded.artifact->artifact_id() &&
           publisherState.calls == 1U;
}

/// @brief Service破棄が進行中ProcessへCancelを通知して完了を待つか検証する
[[nodiscard]] bool test_shutdown(const cue::AssertContext &a_assertContext)
{
    RunnerState runnerState;
    PublisherState publisherState;
    auto created = cue::GameBuildService::create(make_settings(), std::make_unique<ControlledRunner>(runnerState),
                                                 std::make_unique<TestPublisher>(publisherState, a_assertContext),
                                                 a_assertContext);
    std::unique_ptr<cue::GameBuildService> service = std::move(*created.try_value());
    if (!service->start(make_request("41234567-89ab-4cde-8f01-23456789abcd", a_assertContext),
                        cue::CMakeConfigureMode::Required) ||
        !wait_until_running(*service))
    {
        return false;
    }
    service.reset();
    return !runnerState.processActive.load(std::memory_order_acquire);
}
} // namespace

/// @brief Game Build Service、Artifact、Cancel／Retry、Shutdown契約をHeadless検証する
int main()
{
    TestFatalHandler fatalHandler;
    std::vector<std::unique_ptr<cue::LogSink>> sinks;
    cue::Logger logger(fatalHandler, std::move(sinks));
    cue::AssertContext assertContext(logger, fatalHandler);
    return test_artifact_model(assertContext) && test_service_lifecycle(assertContext) && test_shutdown(assertContext)
               ? 0
               : 1;
}
