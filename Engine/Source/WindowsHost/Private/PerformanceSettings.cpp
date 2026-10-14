#include <WindowsHost/PerformanceSettings.h>

#include <cmath>

namespace cue
{
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
    for (const float color : presentation.clearColor)
    {
        if (!std::isfinite(color))
            return Result<void>::failure({ErrorCategory::InvalidArgument, "PerformanceSettings.clear_color"});
    }
    return Result<void>::success();
}
} // namespace cue
