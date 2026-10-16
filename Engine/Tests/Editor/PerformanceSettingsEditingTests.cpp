#include <EditorHost/PerformanceSettingsEditing.h>

#include <array>

#include "../Support/TemporaryFiles.h"

namespace
{
/// @brief Thread、FPS 待機、表示同期の代表的な切替を実効表示と一緒に確認する
bool test_conditional_controls()
{
    using frameWait = cue::FrameWaitMode;
    using frameSync = cue::FrameSyncMode;
    struct ControlCase final
    {
        bool usesWorkers;
        std::uint32_t fps;
        frameWait wait;
        frameSync sync;
        bool isWaitable;
        bool isVSync;
        cue::PerformanceSettingsControls expected;
        std::chrono::nanoseconds effectiveSpin;
    };
    const std::array cases{ControlCase{true,
                                       60,
                                       frameWait::Hybrid,
                                       frameSync::FixedPeriod,
                                       true,
                                       false,
                                       {true, true, true, true, true, false},
                                       std::chrono::nanoseconds(2083333)},
                           ControlCase{false,
                                       60,
                                       frameWait::Hybrid,
                                       frameSync::FixedPeriod,
                                       true,
                                       false,
                                       {false, false, true, true, true, false},
                                       std::chrono::nanoseconds(2083333)},
                           ControlCase{false,
                                       0,
                                       frameWait::Hybrid,
                                       frameSync::FixedPeriod,
                                       true,
                                       false,
                                       {false, false, false, false, true, false},
                                       std::chrono::nanoseconds(0)},
                           ControlCase{true,
                                       60,
                                       frameWait::Sleep,
                                       frameSync::FixedPeriod,
                                       false,
                                       false,
                                       {true, true, true, false, false, false},
                                       std::chrono::nanoseconds(0)},
                           ControlCase{true,
                                       60,
                                       frameWait::Hybrid,
                                       frameSync::DisplayPrediction,
                                       true,
                                       true,
                                       {true, true, true, true, true, true},
                                       std::chrono::nanoseconds(2083333)},
                           ControlCase{false,
                                       0,
                                       frameWait::Hybrid,
                                       frameSync::DisplayPrediction,
                                       true,
                                       true,
                                       {false, false, false, false, true, true},
                                       std::chrono::nanoseconds(0)},
                           ControlCase{true,
                                       60,
                                       frameWait::Hybrid,
                                       frameSync::DisplayPrediction,
                                       true,
                                       false,
                                       {true, true, true, true, true, false},
                                       std::chrono::nanoseconds(2083333)},
                           ControlCase{true,
                                       60,
                                       frameWait::Hybrid,
                                       frameSync::DisplayPrediction,
                                       false,
                                       true,
                                       {true, true, true, true, false, false},
                                       std::chrono::nanoseconds(2083333)},
                           ControlCase{true,
                                       60,
                                       frameWait::Hybrid,
                                       frameSync::PresentationReady,
                                       true,
                                       true,
                                       {true, true, true, true, true, false},
                                       std::chrono::nanoseconds(2083333)},
                           ControlCase{true,
                                       1000,
                                       frameWait::Hybrid,
                                       frameSync::FixedPeriod,
                                       true,
                                       false,
                                       {true, true, true, true, true, false},
                                       std::chrono::nanoseconds(125000)}};
    for (const auto &test : cases)
    {
        auto settings = cue::performance_preset(cue::PerformancePreset::Throughput);
        settings.frame.useWorkerThreads = test.usesWorkers;
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
        if (controls.canEditCpuCapacity != expected.canEditCpuCapacity ||
            controls.canEditUpdateLead != expected.canEditUpdateLead ||
            controls.canEditWaitMode != expected.canEditWaitMode || controls.canEditSpin != expected.canEditSpin ||
            controls.canEditQueuedPresents != expected.canEditQueuedPresents ||
            controls.canEditPredictionSlack != expected.canEditPredictionSlack || settings != original ||
            status.requested != original || status.effective.frame.spinDuration != test.effectiveSpin ||
            status.effective.frame.predictionSlack !=
                (expected.canEditPredictionSlack ? settings.frame.predictionSlack : std::chrono::nanoseconds::zero()))
        {
            return false;
        }
    }
    auto settings = cue::performance_preset(cue::PerformancePreset::Throughput);
    const auto original = settings;
    return !cue::set_gpu_frame_capacity(settings, 0).has_value() &&
           !cue::set_gpu_frame_capacity(settings, 9).has_value() && settings == original &&
           cue::set_gpu_frame_capacity(settings, 1).has_value() && settings.gpu.maxFramesInFlight == 1 &&
           cue::set_gpu_frame_capacity(settings, 3).has_value() && settings.gpu.maxFramesInFlight == 1 &&
           settings.frame == original.frame && settings.presentation == original.presentation &&
           cue::validate_performance_settings(settings).has_value();
}
} // namespace

/// @brief Worker 切替で要求値を保持し、容量縮小時だけ依存値を整える
int main()
{
    if (!test_conditional_controls())
    {
        return 7;
    }
    for (const bool hasExplicitLead : {false, true})
    {
        auto settings = cue::performance_preset(cue::PerformancePreset::Throughput);
        if (!hasExplicitLead)
        {
            settings.frame.maxUpdateLeadFrames.reset();
        }
        const auto savedLead = settings.frame.maxUpdateLeadFrames;
        for (const bool usesWorkers : {true, false, true})
        {
            settings.frame.useWorkerThreads = usesWorkers;
            if (cue::requested_update_lead_frames(settings.frame) != 2 ||
                cue::update_lead_frames(settings.frame) != (usesWorkers ? 2u : 0u) ||
                settings.frame.maxUpdateLeadFrames != savedLead)
            {
                return 1;
            }
        }
        settings.frame.useWorkerThreads = false;
        cue::tests::TemporaryFiles temporary;
        if (!temporary.isCreated)
        {
            return 2;
        }
        auto path = temporary.root.join("Performance.cfg");
        if (!path.has_value() ||
            !cue::save_performance_settings(*temporary.files, *path.try_value(), settings).has_value())
        {
            return 3;
        }
        auto loaded = cue::load_performance_settings(*temporary.files, *path.try_value());
        if (!loaded.has_value() || *loaded.try_value() != settings)
        {
            return 4;
        }
        auto original = settings;
        if (cue::set_cpu_frame_capacity(settings, 0).has_value() ||
            cue::set_cpu_frame_capacity(settings, 9).has_value() || settings != original ||
            !cue::set_cpu_frame_capacity(settings, 2).has_value() ||
            cue::requested_update_lead_frames(settings.frame) != 1 ||
            settings.frame.maxUpdateLeadFrames.has_value() != hasExplicitLead ||
            !cue::validate_performance_settings(settings).has_value())
        {
            return 5;
        }
        if (!cue::set_cpu_frame_capacity(settings, 3).has_value() ||
            cue::requested_update_lead_frames(settings.frame) != (hasExplicitLead ? 1u : 2u) ||
            !cue::set_cpu_frame_capacity(settings, 1).has_value() ||
            cue::requested_update_lead_frames(settings.frame) != 0 ||
            !cue::validate_performance_settings(settings).has_value())
        {
            return 6;
        }
    }
    return 0;
}
