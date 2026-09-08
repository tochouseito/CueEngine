#include <Cue/Editor/ImGui/BuildPresenter.h>

#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Fatal.h>
#include <Cue/Foundation/Log.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <source_location>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <imgui.h>

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

/// @brief Test前提違反をSource Line由来のExit Codeで即時報告する
void require(bool a_condition, std::source_location a_location = std::source_location::current()) noexcept
{
    if (!a_condition)
    {
        std::_Exit(static_cast<int>((a_location.line() % 200U) + 20U));
    }
}

enum class RunnerMode : std::uint8_t
{
    BlockUntilCancelled,
    Succeed,
    Fail
};

struct RunnerState final
{
    std::atomic<RunnerMode> mode = RunnerMode::BlockUntilCancelled;
    std::atomic<std::uint64_t> sequence = 0U;
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
        while (m_state->mode.load(std::memory_order_acquire) == RunnerMode::BlockUntilCancelled &&
               !a_cancellation.is_cancel_requested())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const std::uint64_t sequence = m_state->sequence.fetch_add(1U, std::memory_order_relaxed);
        if (a_cancellation.is_cancel_requested())
        {
            return cue::Result<cue::ChildProcessResult>::success(cue::ChildProcessResult::cancelled(
                {{sequence, cue::ChildProcessStream::StandardError, "cancelled\n"}}));
        }
        const std::uint32_t exitCode = m_state->mode.load(std::memory_order_acquire) == RunnerMode::Fail ? 2U : 0U;
        return cue::Result<cue::ChildProcessResult>::success(cue::ChildProcessResult::exited(
            exitCode, {{sequence, cue::ChildProcessStream::StandardOutput, "build-log\n"}}));
    }

  private:
    RunnerState *m_state;
};

class TestPublisher final : public cue::BuildArtifactPublisher
{
  public:
    /// @brief Assert境界を借用して検証用Publisherを構築する
    explicit TestPublisher(const cue::AssertContext &a_assertContext) noexcept : m_assertContext(&a_assertContext)
    {
    }

    /// @brief Cancel前だけ必須Fileを含む決定的な検証用Artifact Inventoryを返す
    [[nodiscard]] cue::Result<std::optional<cue::BuildArtifactInventory>> publish(
        const cue::BuildPlan &a_plan, const cue::ChildProcessCancellation &a_cancellation) noexcept override
    {
        if (a_cancellation.is_cancel_requested())
        {
            return cue::Result<std::optional<cue::BuildArtifactInventory>>::success(std::nullopt);
        }
        auto inventory =
            cue::BuildArtifactInventory::create(a_plan, "71234567-89ab-4cde-8f01-23456789abcd",
                                                {{"CueGameModule.dll", 256U, std::string(64U, 'a')},
                                                 {"CueGameModule.metadata.json", 64U, std::string(64U, 'b')}},
                                                *m_assertContext);
        if (!inventory)
        {
            return cue::Result<std::optional<cue::BuildArtifactInventory>>::failure(std::move(*inventory.try_error()));
        }
        return cue::Result<std::optional<cue::BuildArtifactInventory>>::success(
            std::optional<cue::BuildArtifactInventory>(std::move(*inventory.try_value())));
    }

  private:
    const cue::AssertContext *m_assertContext;
};

class TestOperationIdSource final : public cue::editor::BuildOperationIdSource
{
  public:
    /// @brief 決定的なOperation ID列の所有権を取得してSourceを構築する
    explicit TestOperationIdSource(std::vector<std::string> a_ids) noexcept : m_ids(std::move(a_ids))
    {
    }

    /// @brief 次の検証用Operation IDを一度だけ返す
    [[nodiscard]] cue::Result<std::string> next_operation_id() noexcept override
    {
        require(m_index < m_ids.size());
        std::string operationId = m_ids[m_index++];
        return cue::Result<std::string>::success(std::move(operationId));
    }

  private:
    std::vector<std::string> m_ids;
    std::size_t m_index = 0U;
};

/// @brief Process起動を行わないTest用のAbsolute Runner設定を返す
[[nodiscard]] cue::CMakeRunnerSettings make_settings()
{
    return {"C:/Tools/cmake.exe", "C:/CueEngine", {}, std::chrono::seconds(5), std::chrono::seconds(5)};
}

/// @brief Presenterの一Frame分のShortcut処理と描画を実行する
void draw_frame(cue::editor::BuildPresenter &a_presenter) noexcept
{
    ImGui::NewFrame();
    a_presenter.process_shortcuts();
    a_presenter.draw();
    ImGui::Render();
}

/// @brief F6のPressとReleaseをFrameへ送りBuild Shortcutを再現する
void press_build_shortcut(cue::editor::BuildPresenter &a_presenter) noexcept
{
    ImGuiIO &input = ImGui::GetIO();
    ImGui::NewFrame();
    ImGui::SetNextWindowFocus();
    a_presenter.draw();
    ImGui::Render();
    input.AddKeyEvent(ImGuiKey_F6, true);
    draw_frame(a_presenter);
    input.AddKeyEvent(ImGuiKey_F6, false);
    draw_frame(a_presenter);
}

/// @brief Configuration、Cancel、Retry、履歴再表示、Keyboard、終了確認をHeadless検証する
void test_build_workflow(const cue::AssertContext &a_assertContext)
{
    RunnerState runnerState;
    auto serviceResult =
        cue::GameBuildService::create(make_settings(), std::make_unique<ControlledRunner>(runnerState),
                                      std::make_unique<TestPublisher>(a_assertContext), a_assertContext);
    require(serviceResult.has_value());
    std::unique_ptr<cue::GameBuildService> service = std::move(*serviceResult.try_value());
    auto operationIds = std::make_unique<TestOperationIdSource>(
        std::vector<std::string>{"01234567-89ab-4cde-8f01-23456789abcd", "11234567-89ab-4cde-8f01-23456789abcd",
                                 "21234567-89ab-4cde-8f01-23456789abcd", "31234567-89ab-4cde-8f01-23456789abcd"});
    std::unique_ptr<cue::editor::BuildPresenter> presenter =
        cue::editor::BuildPresenter::create(*service, std::filesystem::current_path().generic_string(),
                                            k_workspaceCompatibility, std::move(operationIds), a_assertContext);

    require(presenter->can_start());
    require(presenter->set_configuration(cue::BuildConfiguration::Development));
    require(presenter->submit(cue::editor::EditorBuildCommand::Start));
    require(presenter->current_snapshot().state == cue::GameBuildOperationState::Running);
    require(!presenter->can_start());
    require(!presenter->set_configuration(cue::BuildConfiguration::Release));
    require(!presenter->begin_editor_shutdown());
    require(presenter->is_shutdown_confirmation_pending());
    require(!presenter->respond_to_editor_shutdown(cue::editor::EditorBuildShutdownDecision::KeepEditorOpen));
    require(!presenter->is_shutdown_confirmation_pending());
    require(presenter->submit(cue::editor::EditorBuildCommand::Cancel));
    require(service->wait_for_completion().has_value());
    presenter->refresh();
    require(presenter->current_snapshot().state == cue::GameBuildOperationState::Cancelled);
    require(presenter->message().find("キャンセル") != std::string_view::npos);

    runnerState.mode.store(RunnerMode::Succeed, std::memory_order_release);
    require(presenter->submit(cue::editor::EditorBuildCommand::Retry));
    require(service->wait_for_completion().has_value());
    presenter->refresh();
    require(presenter->current_snapshot().state == cue::GameBuildOperationState::Succeeded);
    require(presenter->current_snapshot().profile->configuration() == cue::BuildConfiguration::Development);
    require(presenter->current_snapshot().artifact.has_value());
    require(presenter->displayed_log_count() == 2U);

    const std::string successfulOperationId = presenter->current_snapshot().operationId;
    runnerState.mode.store(RunnerMode::Fail, std::memory_order_release);
    require(presenter->submit(cue::editor::EditorBuildCommand::Start));
    require(service->wait_for_completion().has_value());
    presenter->refresh();
    require(presenter->current_snapshot().state == cue::GameBuildOperationState::Failed);
    require(presenter->current_snapshot().latestSuccessfulArtifact.has_value());
    require(presenter->saved_operations().size() == 2U);
    require(presenter->select_operation(successfulOperationId));
    require(presenter->displayed_snapshot().state == cue::GameBuildOperationState::Succeeded);
    require(presenter->displayed_log_count() == 2U);
    for (const cue::BuildLogSnapshot &log : presenter->displayed_snapshot().logs)
    {
        require(log.operationId == successfulOperationId);
    }

    runnerState.mode.store(RunnerMode::BlockUntilCancelled, std::memory_order_release);
    press_build_shortcut(*presenter);
    require(presenter->current_snapshot().state == cue::GameBuildOperationState::Running);
    require(!presenter->begin_editor_shutdown());
    const bool immediatelyReady =
        presenter->respond_to_editor_shutdown(cue::editor::EditorBuildShutdownDecision::CancelBuildAndClose);
    if (!immediatelyReady)
    {
        require(service->wait_for_completion().has_value());
        presenter->refresh();
    }
    require(presenter->take_shutdown_ready());
    require(!presenter->take_shutdown_ready());
    presenter.reset();
}
} // namespace

/// @brief Game Build ImGuiの操作、履歴、Artifact、終了時Cancel契約を検証する
int main()
{
    TestFatalHandler fatalHandler;
    std::vector<std::unique_ptr<cue::LogSink>> sinks;
    cue::Logger logger(fatalHandler, std::move(sinks));
    cue::AssertContext assertContext(logger, fatalHandler);

    require(ImGui::CreateContext() != nullptr);
    ImGuiIO &input = ImGui::GetIO();
    input.IniFilename = nullptr;
    input.DisplaySize = ImVec2(1280.0F, 720.0F);
    input.DeltaTime = 1.0F / 60.0F;
    static_cast<void>(input.Fonts->Build());

    test_build_workflow(assertContext);
    ImGui::DestroyContext();
    return 0;
}
