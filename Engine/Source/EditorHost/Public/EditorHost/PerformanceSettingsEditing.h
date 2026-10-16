#pragma once

#include <algorithm>
#include <cstdint>

#include <WindowsHost/PerformanceSettings.h>

namespace cue
{
/// @brief 編集中の要求値から判定する条件付き Control の利用可否
struct PerformanceSettingsControls final
{
    bool canEditCpuCapacity = false;
    bool canEditUpdateLead = false;
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
    return {frame.useWorkerThreads,
            frame.useWorkerThreads,
            frame.maxFps != 0,
            frame.maxFps != 0 && frame.waitMode == FrameWaitMode::Hybrid,
            present.isWaitableEnabled,
            frame.syncMode == FrameSyncMode::DisplayPrediction && present.isWaitableEnabled && present.isVSyncEnabled};
}

/// @brief GPU 容量の縮小時に未完了上限も収める。失敗時は変更しない
[[nodiscard]] inline Result<void> set_gpu_frame_capacity(PerformanceSettings &a_settings, std::uint32_t a_capacity)
{
    if (a_capacity == 0 || a_capacity > 8)
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "PerformanceSettingsEditing.gpu_capacity"});
    }
    a_settings.gpu.resourceFrameCount = a_capacity;
    a_settings.gpu.maxFramesInFlight = (std::min)(a_settings.gpu.maxFramesInFlight, a_capacity);
    return Result<void>::success();
}

/// @brief Worker の実効値へ変換せず、編集・保存する先行数を求める。純粋関数
[[nodiscard]] inline std::uint32_t requested_update_lead_frames(const FrameControllerDesc &a_frame) noexcept
{
    return a_frame.maxUpdateLeadFrames.value_or(a_frame.maxFramesInFlight > 0 ? a_frame.maxFramesInFlight - 1 : 0);
}

/// @brief CPU 容量を編集し、明示的な先行数だけを範囲内へ収める。失敗時は変更しない
[[nodiscard]] inline Result<void> set_cpu_frame_capacity(PerformanceSettings &a_settings, std::uint32_t a_capacity)
{
    if (a_capacity == 0 || a_capacity > 8)
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "PerformanceSettingsEditing.cpu_capacity"});
    }
    a_settings.frame.maxFramesInFlight = a_capacity;
    if (a_settings.frame.maxUpdateLeadFrames)
    {
        a_settings.frame.maxUpdateLeadFrames = (std::min)(*a_settings.frame.maxUpdateLeadFrames, a_capacity - 1);
    }
    return Result<void>::success();
}
} // namespace cue
