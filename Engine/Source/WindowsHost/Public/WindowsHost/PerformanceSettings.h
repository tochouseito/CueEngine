#pragma once

#include <array>
#include <cstdint>
#include <optional>

#include <Foundation/Result.h>
#include <RHI/FrameExecution.h>
#include <RHI/Presentation.h>
#include <Runtime/FrameController.h>

namespace cue
{
/// @brief Host が各所有者へ適用する性能設定の所有値
/// 利用者のバッファリング数は gpu.resourceFrameCount。CPU 容量・Worker・先行数はそこから導出する
struct PerformanceSettings final
{
    // CPU Frame の投入と待機の要求値
    FrameControllerDesc frame;
    // GPU 資源枠と未完了 Frame 上限の要求値
    GpuFrameConfig gpu;
    // SwapChain と表示同期の要求値
    PresentationConfig presentation;

    /// @brief 設定要求の同一性を比較する
    bool operator==(const PerformanceSettings &) const = default;
};

/// @brief ゲームがユーザーへ公開する設定範囲。設定 File からは変更しない
struct PerformanceSettingsPolicy final
{
    // 実行中の設定変更をゲーム側が許可するか
    bool usersMayEdit = true;
    // ユーザーが指定できる有限 FPS 上限の最大値
    std::uint32_t maxFps = 1000;
    std::uint32_t maxCpuFrames = 8;
    std::uint32_t maxGpuFrames = 8;
    std::uint32_t maxUpdateLeadFrames = 7;
    std::uint32_t maxSwapChainBuffers = 16;
    std::uint32_t maxQueuedFrames = 16;
    // maxFps = 0 の無制限指定を許可するか
    bool allowsUnlimitedFps = true;
};

/// @brief 表示設定の実効値。Waitable 無効時の Queue 上限は未適用として返す
struct EffectivePresentationSettings final
{
    std::uint32_t bufferCount = 2;
    bool isVSyncEnabled = false;
    bool isTearingAllowed = false;
    std::array<float, 4> clearColor{};
    bool isWaitableEnabled = false;
    std::optional<std::uint32_t> maxQueuedFrames;
};

/// @brief 設定要求とは独立した、現在適用される設定の観測値
struct EffectivePerformanceSettings final
{
    FrameControllerDesc frame;
    GpuFrameConfig gpu;
    EffectivePresentationSettings presentation;
};

/// @brief 要求値と Native 能力 / 実効同期による適用値を区別する
struct PerformanceSettingsStatus final
{
    // 利用側が最後に要求した設定。実効値の補正とは分離して保持する
    PerformanceSettings requested;
    EffectivePerformanceSettings effective;
    FrameSyncFallback syncFallback = FrameSyncFallback::None;
    bool isPending = false;
};

/// @brief Native の適用結果と Runtime の同期結果から状態表示を作る。要求値は変更しない
[[nodiscard]] PerformanceSettingsStatus resolve_performance_status(const PerformanceSettings &a_requested,
                                                                   const FrameProgress &a_progress,
                                                                   bool a_isTearingEnabled,
                                                                   std::optional<std::uint32_t> a_queuedFrameLimit,
                                                                   bool a_isPending = false);

enum class PerformancePreset
{
    Balanced,
    LowLatency,
    Throughput,
};

/// @brief バッファリング数から Runtime の CPU 容量・Worker・先行数を導出する。純粋関数
/// 不正な容量は保持して後続の設定検証で拒否する。FPS / 同期設定は維持する
[[nodiscard]] FrameControllerDesc frame_config_for_buffering(FrameControllerDesc a_frame,
                                                             std::uint32_t a_frameCount) noexcept;

/// @brief CPU / GPU の枠を一括編集し、GPU 未完了上限を収める。1〜8 以外は変更せず失敗する
[[nodiscard]] Result<void> set_frame_buffering(PerformanceSettings &a_settings, std::uint32_t a_frameCount);

/// @brief 開発者が制限した公開範囲を設定要求へ適用する
[[nodiscard]] Result<void> validate_performance_policy(const PerformanceSettings &a_settings,
                                                       const PerformanceSettingsPolicy &a_policy);
/// @brief 編集可能な設定値として Preset を返す。採用前にゲームの Policy を検証する
[[nodiscard]] PerformanceSettings performance_preset(PerformancePreset a_preset);
/// @brief 型の範囲、先行数と容量を変更前に検証する。Thread-safe
[[nodiscard]] Result<void> validate_performance_settings(const PerformanceSettings &a_settings);

/// @brief CPU 容量の内側で有効な Update 先行数を求める。検証済み設定だけに使う
[[nodiscard]] std::uint32_t update_lead_frames(const FrameControllerDesc &a_desc) noexcept;
} // namespace cue
