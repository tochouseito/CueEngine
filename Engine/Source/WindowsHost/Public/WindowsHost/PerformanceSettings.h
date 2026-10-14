#pragma once

#include <Foundation/Result.h>
#include <RHI/FrameExecution.h>
#include <RHI/Presentation.h>
#include <Runtime/FrameController.h>

namespace cue
{
/// @brief Host が各所有者へ適用する性能設定の所有値
struct PerformanceSettings final
{
    FrameControllerDesc frame;
    GpuFrameConfig gpu;
    PresentationConfig presentation;

    /// @brief 設定要求の同一性を比較する
    bool operator==(const PerformanceSettings &) const = default;
};

/// @brief 型の範囲、先行数と容量を変更前に検証する。Thread-safe
[[nodiscard]] Result<void> validate_performance_settings(const PerformanceSettings &a_settings);

/// @brief CPU 容量の内側で有効な Update 先行数を求める。検証済み設定だけに使う
[[nodiscard]] std::uint32_t update_lead_frames(const FrameControllerDesc &a_desc) noexcept;
} // namespace cue
