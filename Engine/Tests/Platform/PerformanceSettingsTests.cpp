#include <WindowsHost/PerformanceSettings.h>

#include <limits>

/// @brief 容量、先行数、表示同期の不正な組合せを変更前に拒否する
int main()
{
    for (const bool usesWorkers : {false, true})
    {
        cue::PerformanceSettings requested;
        requested.frame.useWorkerThreads = usesWorkers;
        requested.frame.maxUpdateLeadFrames = 1;
        requested.presentation.maxQueuedFrames = 3;
        cue::FrameProgress progress;
        progress.syncFallback = cue::FrameSyncFallback::PresentationUnavailable;
        const auto inactive = cue::resolve_performance_status(requested, progress, false, std::nullopt, true);
        const auto active = cue::resolve_performance_status(requested, progress, true, 3);
        if (inactive.requested != requested || active.requested != requested ||
            inactive.effective.presentation.maxQueuedFrames || inactive.effective.presentation.isWaitableEnabled ||
            !inactive.isPending || inactive.syncFallback != progress.syncFallback ||
            inactive.effective.frame.maxUpdateLeadFrames != (usesWorkers ? 1u : 0u) ||
            active.effective.presentation.maxQueuedFrames != 3 || !active.effective.presentation.isWaitableEnabled ||
            !active.effective.presentation.isTearingAllowed || active.isPending)
        {
            return 16;
        }
    }
    cue::PerformanceSettings settings;
    if (!cue::validate_performance_settings(settings).has_value() || cue::update_lead_frames(settings.frame) != 1)
    {
        return 1;
    }
    settings.frame.maxFramesInFlight = 3;
    settings.frame.maxUpdateLeadFrames = 2;
    if (!cue::validate_performance_settings(settings).has_value() || cue::update_lead_frames(settings.frame) != 2)
    {
        return 2;
    }
    settings.frame.maxUpdateLeadFrames = 3;
    if (cue::validate_performance_settings(settings).has_value())
    {
        return 3;
    }
    settings = {};
    settings.gpu.maxFramesInFlight = 3;
    if (cue::validate_performance_settings(settings).has_value())
    {
        return 4;
    }
    settings = {};
    settings.presentation.isVSyncEnabled = true;
    settings.presentation.isTearingAllowed = true;
    if (cue::validate_performance_settings(settings).has_value())
    {
        return 5;
    }
    settings = {};
    settings.presentation.clearColor[0] = std::numeric_limits<float>::quiet_NaN();
    if (cue::validate_performance_settings(settings).has_value())
    {
        return 6;
    }
    settings = {};
    settings.frame.useWorkerThreads = false;
    if (cue::update_lead_frames(settings.frame) != 0)
    {
        return 7;
    }
    cue::PerformanceSettingsPolicy policy;
    policy.maxUpdateLeadFrames = 0;
    if (cue::validate_performance_policy({}, policy).has_value())
    {
        return 8;
    }
    settings = cue::performance_preset(cue::PerformancePreset::Throughput);
    settings.frame.useWorkerThreads = false;
    settings.frame.maxFramesInFlight = 1;
    settings.frame.maxUpdateLeadFrames = 0;
    if (cue::validate_performance_policy(settings, policy).has_value())
    {
        return 18;
    }
    return 0;
}
