#include <EditorHost/PerformanceSettingsEditing.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>

namespace
{
/// @brief 共通の枠数から導出される実行構成を検証する
bool has_buffering(const cue::PerformanceSettings &a_settings, std::uint32_t a_count)
{
    return a_settings.gpu.resourceFrameCount == a_count && a_settings.frame.maxFramesInFlight == a_count &&
           a_settings.frame.useWorkerThreads == (a_count > 1) && a_settings.frame.maxUpdateLeadFrames == a_count - 1 &&
           cue::update_lead_frames(a_settings.frame) == a_count - 1;
}

/// @brief バッファリング切替、上限補正と不正値の無変更を確認する
bool test_buffering()
{
    auto settings = cue::performance_preset(cue::PerformancePreset::Throughput);
    const auto timing = settings.frame;
    const auto presentation = settings.presentation;
    for (const std::uint32_t count : {3u, 1u, 2u, 3u, 8u, 1u})
    {
        const auto oldLimit = settings.gpu.maxFramesInFlight;
        if (!cue::set_frame_buffering(settings, count).has_value() || !has_buffering(settings, count) ||
            settings.gpu.maxFramesInFlight != (std::min)(oldLimit, count) || settings.frame.maxFps != timing.maxFps ||
            settings.frame.waitMode != timing.waitMode || settings.frame.spinDuration != timing.spinDuration ||
            settings.frame.syncMode != timing.syncMode || settings.frame.predictionSlack != timing.predictionSlack ||
            settings.presentation != presentation || !cue::validate_performance_settings(settings).has_value())
        {
            return false;
        }
    }
    const auto original = settings;
    return !cue::set_frame_buffering(settings, 0).has_value() && !cue::set_frame_buffering(settings, 9).has_value() &&
           settings == original;
}

/// @brief 枠数を変えても FPS / 表示同期の条件付き Control と実効値が正しく決まる
bool test_conditional_controls()
{
    using frameWait = cue::FrameWaitMode;
    using frameSync = cue::FrameSyncMode;
    struct ControlCase final
    {
        std::uint32_t count;
        std::uint32_t fps;
        frameWait wait;
        frameSync sync;
        bool isWaitable;
        bool isVSync;
        cue::PerformanceSettingsControls expected;
        std::chrono::nanoseconds spin;
    };
    const std::array cases{ControlCase{1,
                                       60,
                                       frameWait::Hybrid,
                                       frameSync::FixedPeriod,
                                       true,
                                       false,
                                       {true, true, true, false},
                                       std::chrono::nanoseconds(2083333)},
                           ControlCase{3,
                                       0,
                                       frameWait::Hybrid,
                                       frameSync::FixedPeriod,
                                       true,
                                       false,
                                       {false, false, true, false},
                                       std::chrono::nanoseconds(0)},
                           ControlCase{2,
                                       60,
                                       frameWait::Sleep,
                                       frameSync::FixedPeriod,
                                       false,
                                       false,
                                       {true, false, false, false},
                                       std::chrono::nanoseconds(0)},
                           ControlCase{3,
                                       60,
                                       frameWait::Hybrid,
                                       frameSync::DisplayPrediction,
                                       true,
                                       true,
                                       {true, true, true, true},
                                       std::chrono::nanoseconds(2083333)},
                           ControlCase{1,
                                       0,
                                       frameWait::Hybrid,
                                       frameSync::DisplayPrediction,
                                       true,
                                       true,
                                       {false, false, true, true},
                                       std::chrono::nanoseconds(0)},
                           ControlCase{2,
                                       60,
                                       frameWait::Hybrid,
                                       frameSync::DisplayPrediction,
                                       true,
                                       false,
                                       {true, true, true, false},
                                       std::chrono::nanoseconds(2083333)},
                           ControlCase{2,
                                       60,
                                       frameWait::Hybrid,
                                       frameSync::DisplayPrediction,
                                       false,
                                       true,
                                       {true, true, false, false},
                                       std::chrono::nanoseconds(2083333)},
                           ControlCase{3,
                                       60,
                                       frameWait::Hybrid,
                                       frameSync::PresentationReady,
                                       true,
                                       true,
                                       {true, true, true, false},
                                       std::chrono::nanoseconds(2083333)},
                           ControlCase{2,
                                       1000,
                                       frameWait::Hybrid,
                                       frameSync::FixedPeriod,
                                       true,
                                       false,
                                       {true, true, true, false},
                                       std::chrono::nanoseconds(125000)}};
    for (const auto &test : cases)
    {
        auto settings = cue::performance_preset(cue::PerformancePreset::Throughput);
        if (!cue::set_frame_buffering(settings, test.count).has_value())
        {
            return false;
        }
        settings.frame.maxFps = test.fps;
        settings.frame.waitMode = test.wait;
        settings.frame.syncMode = test.sync;
        settings.frame.spinDuration = std::chrono::milliseconds(5);
        settings.presentation.isWaitableEnabled = test.isWaitable;
        settings.presentation.isVSyncEnabled = test.isVSync;
        const auto original = settings;
        const auto controls = cue::performance_settings_controls(settings);
        const auto &expected = test.expected;
        cue::FrameProgress progress;
        if (test.sync == frameSync::DisplayPrediction && test.isWaitable && test.isVSync)
        {
            progress.effectiveSyncMode = frameSync::DisplayPrediction;
        }
        const auto status = cue::resolve_performance_status(
            settings, progress, false,
            test.isWaitable ? std::optional(settings.presentation.maxQueuedFrames) : std::nullopt);
        if (controls.canEditWaitMode != expected.canEditWaitMode || controls.canEditSpin != expected.canEditSpin ||
            controls.canEditQueuedPresents != expected.canEditQueuedPresents ||
            controls.canEditPredictionSlack != expected.canEditPredictionSlack || settings != original ||
            status.requested != original || status.effective.frame.spinDuration != test.spin ||
            status.effective.frame.predictionSlack !=
                (expected.canEditPredictionSlack ? settings.frame.predictionSlack : std::chrono::nanoseconds::zero()))
        {
            return false;
        }
    }
    return true;
}
} // namespace

/// @brief バッファリングの一括編集と条件付き設定を GPU なしで確認する
int main()
{
    if (!test_buffering())
    {
        return 1;
    }
    return test_conditional_controls() ? 0 : 2;
}
