#pragma once

#include <algorithm>
#include <cstdint>

#include <WindowsHost/PerformanceSettings.h>

namespace cue
{
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
