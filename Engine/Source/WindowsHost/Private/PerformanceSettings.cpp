#include <WindowsHost/PerformanceSettings.h>

#include <algorithm>
#include <cmath>

namespace cue
{
/// @brief 利用者設定の独立した CPU / Worker / 先行数を共通の枠数から置き換える
FrameControllerDesc frame_config_for_buffering(FrameControllerDesc a_frame, std::uint32_t a_frameCount) noexcept
{
    // CPU 枠と Update の先行数を、利用者が選んだ GPU 枠数へ揃える
    a_frame.maxFramesInFlight = a_frameCount;
    a_frame.useWorkerThreads = a_frameCount > 1;
    a_frame.maxUpdateLeadFrames = a_frameCount > 0 ? a_frameCount - 1 : 0;
    return a_frame;
}

/// @brief 枠数を一度の編集で揃え、容量を再拡張しても明示的な GPU 上限は増やさない
Result<void> set_frame_buffering(PerformanceSettings &a_settings, std::uint32_t a_frameCount)
{
    if (a_frameCount == 0 || a_frameCount > 8)
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "PerformanceSettings.buffering"});
    }
    // 枠数の変更を関連する CPU / GPU 設定へまとめて反映する
    a_settings.frame = frame_config_for_buffering(a_settings.frame, a_frameCount);
    a_settings.gpu.resourceFrameCount = a_frameCount;
    // GPU 未完了数の明示上限は容量を拡張しても勝手に増やさない
    a_settings.gpu.maxFramesInFlight = (std::min)(a_settings.gpu.maxFramesInFlight, a_frameCount);
    return Result<void>::success();
}

/// @brief 未適用の表示 Queue 上限を設定値で埋めず、Native の成功結果を反映する
PerformanceSettingsStatus resolve_performance_status(const PerformanceSettings &a_requested,
                                                     const FrameProgress &a_progress, bool a_isTearingEnabled,
                                                     std::optional<std::uint32_t> a_queuedFrameLimit, bool a_isPending)
{
    PerformanceSettingsStatus status;
    // 要求値を保持し、実効値の補正で上書きしない
    status.requested = a_requested;
    status.effective.frame = a_requested.frame;
    status.effective.frame.maxUpdateLeadFrames = update_lead_frames(a_requested.frame);
    // Runtime が採用した同期方式と有効な Spin 時間を表示値へ使う
    status.effective.frame.syncMode = a_progress.effectiveSyncMode;
    status.effective.frame.spinDuration = effective_spin_duration(a_requested.frame);
    status.effective.frame.predictionSlack = a_progress.effectiveSyncMode == FrameSyncMode::DisplayPrediction
                                                 ? a_requested.frame.predictionSlack
                                                 : std::chrono::nanoseconds::zero();
    status.effective.gpu = a_requested.gpu;
    const auto &present = a_requested.presentation;
    // Waitable が無効な場合は表示 Queue 上限を未適用として返す
    status.effective.presentation = {present.bufferCount, present.isVSyncEnabled,         a_isTearingEnabled,
                                     present.clearColor,  a_queuedFrameLimit.has_value(), a_queuedFrameLimit};
    status.syncFallback = a_progress.syncFallback;
    status.isPending = a_isPending;
    return status;
}

/// @brief ゲーム固有の最大値と編集許可を共通検証へ重ねる
Result<void> validate_performance_policy(const PerformanceSettings &a_settings,
                                         const PerformanceSettingsPolicy &a_policy)
{
    auto settings = a_settings;
    // 互換 API の独立指定があっても、共通枠数から検証値を導出する
    settings.frame = frame_config_for_buffering(settings.frame, settings.gpu.resourceFrameCount);
    auto valid = validate_performance_settings(settings);
    if (!valid.has_value())
    {
        return valid;
    }
    // 型として有効な値でも、ゲーム側の公開範囲を超えた要求は拒否する
    if (!a_policy.usersMayEdit || settings.frame.maxFps > a_policy.maxFps ||
        (!a_policy.allowsUnlimitedFps && settings.frame.maxFps == 0) ||
        settings.frame.maxFramesInFlight > a_policy.maxCpuFrames ||
        settings.gpu.resourceFrameCount > a_policy.maxGpuFrames ||
        update_lead_frames(settings.frame) > a_policy.maxUpdateLeadFrames ||
        settings.presentation.bufferCount > a_policy.maxSwapChainBuffers ||
        settings.presentation.maxQueuedFrames > a_policy.maxQueuedFrames)
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "PerformanceSettings.policy"});
    }
    return Result<void>::success();
}

/// @brief 遅延と並列性の出発点を値として提供し、各タイトルで再調整できるようにする
PerformanceSettings performance_preset(PerformancePreset a_preset)
{
    PerformanceSettings settings;
    // 低遅延 Preset は CPU / GPU の先行を一枠へ抑える
    if (a_preset == PerformancePreset::LowLatency)
    {
        settings.gpu = {1, 1};
    }
    // 並列性重視の Preset は資源枠と表示 Buffer を三枠へ増やす
    else if (a_preset == PerformancePreset::Throughput)
    {
        settings.gpu = {3, 3};
        settings.presentation.bufferCount = 3;
        settings.presentation.maxQueuedFrames = 2;
    }
    settings.frame = frame_config_for_buffering(settings.frame, settings.gpu.resourceFrameCount);
    return settings;
}

/// @brief Worker がない場合は先行せず、未指定なら既存の CPU 容量に合わせる
std::uint32_t update_lead_frames(const FrameControllerDesc &a_desc) noexcept
{
    return a_desc.useWorkerThreads
               ? a_desc.maxUpdateLeadFrames.value_or(a_desc.maxFramesInFlight > 0 ? a_desc.maxFramesInFlight - 1 : 0)
               : 0;
}

/// @brief Native 実体を変更する前にすべての設定値を確認する
Result<void> validate_performance_settings(const PerformanceSettings &a_settings)
{
    const auto &frame = a_settings.frame;
    const auto &gpu = a_settings.gpu;
    const auto &presentation = a_settings.presentation;
    // 容量・待機時間・表示方式の組合せを Native 資源の変更前に検証する
    if (frame.maxFramesInFlight == 0 || frame.maxFramesInFlight > 8 ||
        (frame.maxUpdateLeadFrames && *frame.maxUpdateLeadFrames >= frame.maxFramesInFlight) ||
        (frame.waitMode != FrameWaitMode::Sleep && frame.waitMode != FrameWaitMode::Hybrid) ||
        (frame.syncMode != FrameSyncMode::FixedPeriod && frame.syncMode != FrameSyncMode::PresentationReady &&
         frame.syncMode != FrameSyncMode::DisplayPrediction) ||
        frame.spinDuration < std::chrono::nanoseconds::zero() || frame.spinDuration > std::chrono::milliseconds(5) ||
        frame.predictionSlack < std::chrono::nanoseconds::zero() ||
        frame.predictionSlack > std::chrono::milliseconds(100) || gpu.resourceFrameCount == 0 ||
        gpu.resourceFrameCount > 8 || gpu.maxFramesInFlight == 0 || gpu.maxFramesInFlight > gpu.resourceFrameCount ||
        presentation.bufferCount < 2 || presentation.bufferCount > 16 || presentation.maxQueuedFrames == 0 ||
        presentation.maxQueuedFrames > 16 || (presentation.isVSyncEnabled && presentation.isTearingAllowed))
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "PerformanceSettings.validate"});
    }
    // Clear 色に NaN や無限大が含まれる設定を拒否する
    for (const float color : presentation.clearColor)
    {
        if (!std::isfinite(color))
            return Result<void>::failure({ErrorCategory::InvalidArgument, "PerformanceSettings.clear_color"});
    }
    return Result<void>::success();
}
} // namespace cue
