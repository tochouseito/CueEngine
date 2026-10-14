#include <WindowsHost/PerformanceSettings.h>

#include <limits>

/// @brief 容量、先行数、表示同期の不正な組合せを変更前に拒否する
int main()
{
    cue::PerformanceSettings settings;
    if (!cue::validate_performance_settings(settings).has_value() || cue::update_lead_frames(settings.frame) != 1)
        return 1;
    settings.frame.maxFramesInFlight = 3;
    settings.frame.maxUpdateLeadFrames = 2;
    if (!cue::validate_performance_settings(settings).has_value() || cue::update_lead_frames(settings.frame) != 2)
        return 2;
    settings.frame.maxUpdateLeadFrames = 3;
    if (cue::validate_performance_settings(settings).has_value())
        return 3;
    settings = {};
    settings.gpu.maxFramesInFlight = 3;
    if (cue::validate_performance_settings(settings).has_value())
        return 4;
    settings = {};
    settings.presentation.isVSyncEnabled = true;
    settings.presentation.isTearingAllowed = true;
    if (cue::validate_performance_settings(settings).has_value())
        return 5;
    settings = {};
    settings.presentation.clearColor[0] = std::numeric_limits<float>::quiet_NaN();
    if (cue::validate_performance_settings(settings).has_value())
        return 6;
    settings = {};
    settings.frame.useWorkerThreads = false;
    if (cue::update_lead_frames(settings.frame) != 0)
        return 7;
    return 0;
}
