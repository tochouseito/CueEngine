#pragma once

#include <Foundation/Result.h>
#include <Platform/FileSystem.h>
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

/// @brief ゲームがユーザーへ公開する設定範囲。設定 File からは変更しない
struct PerformanceSettingsPolicy final
{
    bool usersMayEdit = true;
    std::uint32_t maxFps = 1000;
    std::uint32_t maxCpuFrames = 8;
    std::uint32_t maxGpuFrames = 8;
    std::uint32_t maxUpdateLeadFrames = 7;
    std::uint32_t maxSwapChainBuffers = 16;
    std::uint32_t maxQueuedFrames = 16;
    bool allowsUnlimitedFps = true;
};

enum class PerformancePreset
{
    Balanced,
    LowLatency,
    Throughput,
};

/// @brief 開発者が制限した公開範囲を設定要求へ適用する
[[nodiscard]] Result<void> validate_performance_policy(const PerformanceSettings &a_settings,
                                                       const PerformanceSettingsPolicy &a_policy);
/// @brief 編集可能な設定値として Preset を返す。採用前にゲームの Policy を検証する
[[nodiscard]] PerformanceSettings performance_preset(PerformancePreset a_preset);
/// @brief Schema 付き設定を一時 File から原子的に置換する。Owner Thread 専用
[[nodiscard]] Result<void> save_performance_settings(IFileSystem &a_files, const Path &a_path,
                                                    const PerformanceSettings &a_settings);
/// @brief File 上限と Schema を検証し、完全に有効な設定だけ返す。Owner Thread 専用
[[nodiscard]] Result<PerformanceSettings> load_performance_settings(IFileSystem &a_files, const Path &a_path);

/// @brief 型の範囲、先行数と容量を変更前に検証する。Thread-safe
[[nodiscard]] Result<void> validate_performance_settings(const PerformanceSettings &a_settings);

/// @brief CPU 容量の内側で有効な Update 先行数を求める。検証済み設定だけに使う
[[nodiscard]] std::uint32_t update_lead_frames(const FrameControllerDesc &a_desc) noexcept;
} // namespace cue
