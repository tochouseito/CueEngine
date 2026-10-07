#include <EditorHost/EditorHost.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>
#include <utility>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <Passes/PresentToSwapChainPass.h>

namespace
{
/// @brief 後続の ImGuiPass と同じ注入経路で実 GPU 表示と解放順を検査する
class EditorDisplayPass final : public cue::FrameGraphPass
{
public:
    /// @brief Host の停止まで生存する検査値を借用する
    EditorDisplayPass(int& a_recorded, int& a_destroyed, bool a_failsSetup = false) noexcept
        : m_recorded(&a_recorded), m_destroyed(&a_destroyed), m_failsSetup(a_failsSetup)
    {
    }

    /// @brief Host の Graph が抽象型から Pass を解放した回数を記録する
    ~EditorDisplayPass() override
    {
        ++*m_destroyed;
    }
    /// @brief 固定名に依存しない注入を検査する診断名を返す
    [[nodiscard]] const char* name() const noexcept override
    {
        return "EditorDisplay";
    }
    /// @brief BackBuffer を扱う Graphics Queue を指定する
    [[nodiscard]] cue::QueueType type() const noexcept override
    {
        return cue::QueueType::Graphics;
    }

    /// @brief 注入経路の検証に既存の全画面表示 Pipeline を利用する
    [[nodiscard]] cue::Result<void> setup(cue::FrameGraphBuilder& a_builder) override
    {
        if (m_failsSetup)
        {
            return cue::Result<void>::failure({cue::ErrorCategory::InvalidState, "EditorDisplayPass.setup"});
        }
        return m_present.setup(a_builder);
    }

    /// @brief 実 BackBuffer と FinalColor の State 宣言を標準表示へ委譲する
    [[nodiscard]] cue::Result<void> describe_resources(cue::FrameGraphBuilder& a_builder) override
    {
        return m_present.describe_resources(a_builder);
    }

    /// @brief 呼出回数を記録し、実 DX12 Context へ全画面描画を記録する
    [[nodiscard]] cue::Result<void> execute(cue::FrameGraphContext& a_context) override
    {
        ++*m_recorded;
        return m_present.execute(a_context);
    }

private:
    int* m_recorded = nullptr;
    int* m_destroyed = nullptr;
    bool m_failsSetup = false;
    cue::PresentToSwapChainPass m_present;
};

/// @brief Resize 前後の UI 構築と Snapshot 消費が同じ Host で進むまで Frame を送る
[[nodiscard]] bool wait_for_editor_progress(cue::EditorHost &a_host, std::uint64_t a_afterUi,
                                            std::uint64_t a_afterRendered, std::uint64_t a_afterConsumed,
                                            std::uint64_t a_afterRecorded, std::uint32_t a_frameCount,
                                            bool a_usesWorkers)
{
    const auto ownerId = std::this_thread::get_id();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline)
    {
        auto step = a_host.step();
        auto ui = a_host.ui_frame_info();
        auto progress = a_host.frame_progress();
        auto transfer = a_host.ui_transfer_info();
        if (!step.has_value() || !*step.try_value() || !ui.has_value() || !progress.has_value() ||
            !transfer.has_value())
        {
            return false;
        }
        if (ui.try_value()->frames > a_afterUi && ui.try_value()->vertexCount > 0 && ui.try_value()->indexCount > 0 &&
            progress.try_value()->renderedFrames > a_afterRendered &&
            transfer.try_value()->consumedFrames > a_afterConsumed &&
            transfer.try_value()->consumedFrames > transfer.try_value()->discardedFrames &&
            transfer.try_value()->consumedFrames - transfer.try_value()->discardedFrames > a_afterRecorded &&
            transfer.try_value()->pendingFrames <= a_frameCount &&
            (progress.try_value()->renderThreadId != ownerId) == a_usesWorkers &&
            transfer.try_value()->renderThreadId == progress.try_value()->renderThreadId)
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

/// @brief Window 側を止めず、最小化前に採用した CPU Callback の完了を待つ
[[nodiscard]] bool wait_for_editor_idle(cue::EditorHost &a_host)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline)
    {
        auto step = a_host.step();
        auto progress = a_host.frame_progress();
        if (!step.has_value() || !*step.try_value() || !progress.has_value())
        {
            return false;
        }
        if (progress.try_value()->updatedFrames == progress.try_value()->submittedFrames &&
            progress.try_value()->renderedFrames == progress.try_value()->submittedFrames)
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

/// @brief 初期化途中の失敗後も停止でき、同じ Host を再初期化しない
int test_failed_initialization()
{
    int recorded = 0;
    int destroyed = 0;
    cue::EditorHostConfig config;
    config.window.title.clear();
    config.imgui.settingsFile.clear();
    config.graph.displayPass = std::make_unique<EditorDisplayPass>(recorded, destroyed);
    cue::EditorHost host(std::move(config));
    auto result = host.initialize();
    if (result.has_value() || result.try_error()->category != cue::ErrorCategory::InvalidArgument || destroyed != 1)
    {
        return 1;
    }
    if (!host.shutdown().has_value() || !host.shutdown().has_value())
    {
        return 2;
    }
    if (host.initialize().has_value() || host.step().has_value() || host.frame_progress().has_value())
    {
        return 3;
    }
    return 0;
}

/// @brief Editor の既定構成で描画を進め、UI Owner と同じ Thread 上の Frame 実行を確認する
int test_owner_thread_frames()
{
    int recorded = 0;
    int destroyed = 0;
    int uiFrames = 0;
    const auto ownerId = std::this_thread::get_id();
    cue::EditorHostConfig config;
    config.window.clientSize = {320, 240};
    config.frame.maxFps = 0;
    config.frame.useWorkerThreads = false;
    config.imgui.settingsFile.clear();
    config.buildUi = [&]()
    {
        ++uiFrames;
        return std::this_thread::get_id() == ownerId
                   ? cue::Result<void>::success()
                   : cue::Result<void>::failure({cue::ErrorCategory::WrongThread, "Test.ui"});
    };
    config.graph.displayPass = std::make_unique<EditorDisplayPass>(recorded, destroyed);
    cue::EditorHost host(std::move(config));
    if (host.step().has_value() || host.frame_progress().has_value() || host.ui_frame_info().has_value())
    {
        return 1;
    }
    auto initResult = host.initialize();
    if (!initResult.has_value() || host.initialize().has_value())
    {
        return 2;
    }
    for (int index = 0; index < 3; ++index)
    {
        auto stepResult = host.step();
        if (!stepResult.has_value() || !*stepResult.try_value())
        {
            return 3;
        }
    }
    auto progressResult = host.frame_progress();
    if (!progressResult.has_value())
    {
        return 4;
    }
    const auto &progress = *progressResult.try_value();
    auto ui = host.ui_frame_info();
    auto frameTiming = host.frame_timing_info();
    auto uiTiming = host.ui_timing_info();
    auto graphTiming = host.graph_performance();
    if (!ui.has_value() || ui.try_value()->frames != 3 || uiFrames != 3 || recorded != 3 || destroyed != 0 ||
        progress.submittedFrames != 3 || progress.updatedFrames != 3 || progress.renderedFrames != 3 ||
        progress.updateThreadId != ownerId || progress.renderThreadId != ownerId)
    {
        return 5;
    }
    if (!frameTiming.has_value() || !uiTiming.has_value() || !graphTiming.has_value() ||
        frameTiming.try_value()->main.sampleCount != 3 || frameTiming.try_value()->update.sampleCount != 3 ||
        frameTiming.try_value()->render.sampleCount != 3 || uiTiming.try_value()->uiBuild.sampleCount != 3 ||
        uiTiming.try_value()->snapshotCopy.sampleCount != 3 || !graphTiming.try_value()->record.sampleCount)
    {
        return 9;
    }

    bool rejectedOtherThread = false;
    std::thread worker(
        [&]()
        {
            auto initialize = host.initialize();
            auto step = host.step();
            auto progress = host.frame_progress();
            auto ui = host.ui_frame_info();
            auto frameTiming = host.frame_timing_info();
            auto uiTiming = host.ui_timing_info();
            auto graphTiming = host.graph_performance();
            auto stop = host.shutdown();
            rejectedOtherThread = !initialize.has_value() && !step.has_value() && !progress.has_value() &&
                                  !stop.has_value() && !ui.has_value() && !frameTiming.has_value() &&
                                  !uiTiming.has_value() && !graphTiming.has_value() &&
                                  initialize.try_error()->category == cue::ErrorCategory::WrongThread &&
                                  step.try_error()->category == cue::ErrorCategory::WrongThread &&
                                  progress.try_error()->category == cue::ErrorCategory::WrongThread &&
                                  stop.try_error()->category == cue::ErrorCategory::WrongThread &&
                                  ui.try_error()->category == cue::ErrorCategory::WrongThread &&
                                  frameTiming.try_error()->category == cue::ErrorCategory::WrongThread &&
                                  uiTiming.try_error()->category == cue::ErrorCategory::WrongThread &&
                                  graphTiming.try_error()->category == cue::ErrorCategory::WrongThread;
        });
    worker.join();
    if (!rejectedOtherThread)
    {
        return 6;
    }
    if (!host.shutdown().has_value() || !host.shutdown().has_value() || destroyed != 1)
    {
        return 7;
    }
    if (host.step().has_value() || host.frame_progress().has_value() || host.ui_frame_info().has_value())
    {
        return 8;
    }
    return 0;
}

/// @brief Graph Build 失敗と Graph 生成前の設定失敗の両方で注入 Pass を直ちに回収する
int test_failed_display()
{
    for (bool failsConfig : {false, true})
    {
        int recorded = 0;
        int destroyed = 0;
        cue::EditorHostConfig config;
        config.window.clientSize = {320, 240};
        config.imgui.settingsFile.clear();
        config.graph.displayPass = std::make_unique<EditorDisplayPass>(recorded, destroyed, true);
        if (failsConfig)
        {
            config.frame.maxFramesInFlight = 0;
        }
        cue::EditorHost host(std::move(config));
        auto result = host.initialize();
        const auto expected = failsConfig ? cue::ErrorCategory::InvalidArgument : cue::ErrorCategory::InvalidState;
        if (result.has_value() || result.try_error()->category != expected || recorded != 0 || destroyed != 1 ||
            !host.shutdown().has_value() || host.step().has_value())
        {
            return 1;
        }
    }
    return 0;
}

/// @brief Context / GPU Backend の生成失敗でも Window とともに回収する
int test_invalid_ui_config()
{
    for (int kind = 0; kind < 3; ++kind)
    {
        cue::EditorHostConfig config;
        config.imgui.settingsFile.clear();
        config.frame.useWorkerThreads = kind == 1;
        if (kind == 1)
        {
            config.imgui.fontSize = -1.0f;
        }
        if (kind == 0)
        {
            config.imgui.fontSize = 0.0f;
        }
        if (kind == 2)
        {
            // Window / Backend / SwapChain の生成後に失敗する GPU 接続も Rollback する
            config.imgui.rendererDescriptorCapacity = 0;
        }
        cue::EditorHost host(std::move(config));
        auto result = host.initialize();
        if (result.has_value() || result.try_error()->category != cue::ErrorCategory::InvalidArgument ||
            !host.shutdown().has_value() || host.ui_frame_info().has_value() || host.step().has_value())
        {
            return 1;
        }
    }
    return 0;
}

/// @brief UI 内の Host 再入を拒否し、UI 失敗を Runtime と Host の Result に伝える
int test_failed_ui_callback()
{
    cue::EditorHost* borrowed = nullptr;
    bool rejectedReentry = false;
    cue::EditorHostConfig config;
    config.imgui.settingsFile.clear();
    config.frame.maxFps = 0;
    config.buildUi = [&]()
    {
        auto stopped = borrowed->shutdown();
        auto stepped = borrowed->step();
        rejectedReentry = !stopped.has_value() && !stepped.has_value() &&
                          stopped.try_error()->category == cue::ErrorCategory::InvalidState &&
                          stepped.try_error()->category == cue::ErrorCategory::InvalidState;
        return cue::Result<void>::failure({cue::ErrorCategory::InvalidState, "Test.ui_callback"});
    };
    cue::EditorHost host(std::move(config));
    borrowed = &host;
    if (!host.initialize().has_value())
    {
        return 1;
    }
    auto step = host.step();
    auto stopped = host.shutdown();
    if (!rejectedReentry || step.has_value() || step.try_error()->operation != "Test.ui_callback" ||
        stopped.has_value() || stopped.try_error()->operation != "Test.ui_callback" ||
        !host.shutdown().has_value() || host.ui_frame_info().has_value())
    {
        return 2;
    }
    return 0;
}
/// @brief 既定 UI と既定 ImGuiPass の組合せで CPU 構築から GPU 提出まで進む
int test_default_ui_display(bool a_usesWorkers, std::uint32_t a_frameCount)
{
    cue::EditorHostConfig config;
    config.window.clientSize = {320, 240};
    config.imgui.settingsFile.clear();
    config.frame.maxFps = 0;
    config.frame.useWorkerThreads = a_usesWorkers;
    config.frame.maxFramesInFlight = a_frameCount;
    cue::EditorHost host(std::move(config));
    if (!host.initialize().has_value())
    {
        return 1;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline)
    {
        auto current = host.frame_progress();
        if (!current.has_value())
        {
            return 2;
        }
        if (current.try_value()->renderedFrames >= 120)
        {
            break;
        }
        auto step = host.step();
        if (!step.has_value() || !*step.try_value())
        {
            return 2;
        }
    }
    auto ui = host.ui_frame_info();
    auto progress = host.frame_progress();
    auto transfer = host.ui_transfer_info();
    if (!ui.has_value() || ui.try_value()->frames < 120 || ui.try_value()->vertexCount == 0 ||
        ui.try_value()->indexCount == 0 || !progress.has_value() || progress.try_value()->renderedFrames < 120 ||
        (progress.try_value()->renderThreadId != std::this_thread::get_id()) != a_usesWorkers ||
        !transfer.has_value() || transfer.try_value()->pendingFrames > a_frameCount ||
        transfer.try_value()->consumedFrames < 120 || !host.shutdown().has_value())
    {
        return 3;
    }
    return 0;
}

/// @brief 最小化と復帰を越えて既定 Test UI、ImGui Snapshot と描画 Thread が継続する
int test_resize_ui_display(bool a_usesWorkers)
{
    cue::EditorHostConfig config;
    config.window.title = a_usesWorkers ? "CueEditorHost Resize Worker" : "CueEditorHost Resize Single";
    config.window.clientSize = {320, 240};
    config.frame.maxFps = 0;
    config.frame.useWorkerThreads = a_usesWorkers;
    config.frame.maxFramesInFlight = a_usesWorkers ? 2 : 1;
    config.imgui.settingsFile.clear();
    const auto frameCount = config.frame.maxFramesInFlight;
    cue::EditorHost host(std::move(config));
    if (!host.initialize().has_value() || !wait_for_editor_progress(host, 0, 0, 0, 0, frameCount, a_usesWorkers))
    {
        return 1;
    }
    auto initialProgress = host.frame_progress();
    auto initialTransfer = host.ui_transfer_info();
    if (!initialProgress.has_value() || !initialTransfer.has_value())
    {
        return 1;
    }
    const HWND handle =
        FindWindowW(nullptr, a_usesWorkers ? L"CueEditorHost Resize Worker" : L"CueEditorHost Resize Single");
    if (!handle || GetWindowThreadProcessId(handle, nullptr) != GetCurrentThreadId())
    {
        return 2;
    }
    RECT initialClient{};
    if (!GetClientRect(handle, &initialClient))
    {
        return 2;
    }

    // 最小化中も Host と UI Owner が有効で、GPU 提出を休止して安全に Frame を回収する
    ShowWindow(handle, SW_MINIMIZE);
    if (!IsIconic(handle) || !wait_for_editor_idle(host))
    {
        return 3;
    }
    auto pausedTransfer = host.ui_transfer_info();
    if (!pausedTransfer.has_value() ||
        pausedTransfer.try_value()->consumedFrames < pausedTransfer.try_value()->discardedFrames)
    {
        return 3;
    }
    const auto pausedRecorded =
        pausedTransfer.try_value()->consumedFrames - pausedTransfer.try_value()->discardedFrames;
    for (int index = 0; index < 12; ++index)
    {
        auto step = host.step();
        if (!step.has_value() || !*step.try_value())
        {
            return 4;
        }
    }
    auto minimizedUi = host.ui_frame_info();
    auto minimizedProgress = host.frame_progress();
    auto minimizedTransfer = host.ui_transfer_info();
    if (!minimizedUi.has_value() || !minimizedProgress.has_value() || !minimizedTransfer.has_value() ||
        minimizedTransfer.try_value()->consumedFrames < minimizedTransfer.try_value()->discardedFrames ||
        minimizedTransfer.try_value()->consumedFrames - minimizedTransfer.try_value()->discardedFrames !=
            pausedRecorded ||
        minimizedTransfer.try_value()->pendingFrames > frameCount)
    {
        return 4;
    }

    // 復帰と寸法変更の後も同じ ImGuiManager が Test UI の Snapshot を消費する
    ShowWindow(handle, SW_RESTORE);
    ShowWindow(handle, SW_SHOWNORMAL);
    if (IsIconic(handle) || !SetWindowPos(handle, nullptr, 0, 0, 640, 460, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE))
    {
        return 5;
    }
    RECT client{};
    if (!GetClientRect(handle, &client) || client.right <= 0 || client.bottom <= 0 ||
        (client.right == initialClient.right && client.bottom == initialClient.bottom) ||
        !wait_for_editor_progress(host, minimizedUi.try_value()->frames, minimizedProgress.try_value()->renderedFrames,
                                  minimizedTransfer.try_value()->consumedFrames, pausedRecorded, frameCount,
                                  a_usesWorkers))
    {
        return 6;
    }
    auto transfer = host.ui_transfer_info();
    auto progress = host.frame_progress();
    if (!transfer.has_value() || !progress.has_value() ||
        transfer.try_value()->publishedFrames < transfer.try_value()->consumedFrames ||
        transfer.try_value()->pendingFrames > frameCount ||
        (progress.try_value()->updateThreadId != std::this_thread::get_id()) != a_usesWorkers ||
        progress.try_value()->updateThreadId != initialProgress.try_value()->updateThreadId ||
        progress.try_value()->renderThreadId != initialProgress.try_value()->renderThreadId ||
        transfer.try_value()->renderThreadId != initialTransfer.try_value()->renderThreadId ||
        transfer.try_value()->consumedFrames <= initialTransfer.try_value()->consumedFrames ||
        !host.shutdown().has_value() || IsWindow(handle))
    {
        return 7;
    }
    return 0;
}
} // namespace

/// @brief Editor 用の Host 基盤の異常系と実 Window 上の Frame 進行を確認する
int main()
{
    for (const auto frames : {1u, 2u})
    {
        for (const bool usesWorkers : {false, true})
        {
            if (const int result = test_default_ui_display(usesWorkers, frames); result != 0)
            {
                return 60 + result;
            }
        }
    }
    for (const bool usesWorkers : {false, true})
    {
        if (const int result = test_resize_ui_display(usesWorkers); result != 0)
        {
            return 70 + result;
        }
    }
    if (const int result = test_failed_initialization(); result != 0)
    {
        return 10 + result;
    }
    if (const int result = test_owner_thread_frames(); result != 0)
    {
        return 20 + result;
    }
    if (const int result = test_failed_display(); result != 0)
    {
        return 30 + result;
    }
    if (const int result = test_invalid_ui_config(); result != 0)
    {
        return 40 + result;
    }
    if (const int result = test_failed_ui_callback(); result != 0)
    {
        return 50 + result;
    }
    return 0;
}
