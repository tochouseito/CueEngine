#pragma once

#include <WindowsHost/PerformanceSettings.h>

namespace cue
{
/// @brief 編集中の要求値から判定する条件付き Control の利用可否
struct PerformanceSettingsControls final
{
    bool canEditWaitMode = false;
    bool canEditSpin = false;
    bool canEditQueuedPresents = false;
    bool canEditPredictionSlack = false;
};

/// @brief 実効状態を要求値へ書き戻さず、UI の編集条件だけを返す。純粋関数
[[nodiscard]] inline PerformanceSettingsControls performance_settings_controls(
    const PerformanceSettings &a_settings) noexcept
{
    const auto &frame = a_settings.frame;
    const auto &present = a_settings.presentation;
    return {frame.maxFps != 0, frame.maxFps != 0 && frame.waitMode == FrameWaitMode::Hybrid, present.isWaitableEnabled,
            frame.syncMode == FrameSyncMode::DisplayPrediction && present.isWaitableEnabled && present.isVSyncEnabled};
}

} // namespace cue
