#pragma once

#include <atomic>
#include <cstdint>

#include <Foundation/TimingTrace.h>

namespace cue::test
{
/// @brief 最初の閾値超過だけを公開する。configure は Worker 起動前、結果参照は time の acquire 後に行う
struct SpikeTrigger final
{
    std::atomic<std::int64_t> time{0};
    std::atomic<bool> claimed{false};
    std::int64_t armedAt = 0;
    std::int64_t renderThreshold = 20000000;
    std::int64_t gpuThreshold = 5000000;
    std::uint32_t kind = 0;
    std::uint64_t frame = 0;
    std::int64_t duration = 0;

    /// @brief 開始前の標本と閾値未満を除外し、全 Payload を揃えてから時刻を公開する
    void consider(std::uint32_t a_kind, std::uint64_t a_frame, std::int64_t a_duration, std::int64_t a_now) noexcept
    {
        if (a_now < armedAt || a_duration < (a_kind == 1 ? renderThreshold : gpuThreshold))
        {
            return;
        }
        bool expected = false;
        if (claimed.compare_exchange_strong(expected, true, std::memory_order_relaxed))
        {
            kind = a_kind;
            frame = a_frame;
            duration = a_duration;
            time.store(a_now, std::memory_order_release);
        }
    }
};

/// @brief 同一 Frame / Queue の提出と Calibration だけを結び、巨大な絶対 GPU Tick の桁落ちを避ける
inline bool gpu_submit_lead(const TimingTraceGpuRecord &a_calibration, const TimingTraceGpuRecord &a_submit,
                            const TimingTraceGpuRecord &a_pass, std::uint64_t a_qpcFrequency,
                            std::int64_t &a_duration) noexcept
{
    if (a_calibration.kind != 0 || a_submit.kind != 1 || a_pass.kind != 2 || a_calibration.status != 0 ||
        a_submit.status != 0 || a_pass.status != 0 || a_calibration.frame != a_pass.frame ||
        a_submit.frame != a_pass.frame || a_calibration.queue != a_pass.queue || a_submit.queue != a_pass.queue ||
        a_calibration.frequency == 0 || a_calibration.frequency != a_pass.frequency || a_qpcFrequency == 0 ||
        a_pass.beginTick < a_calibration.beginTick)
    {
        return false;
    }
    const auto gpuDelta = a_pass.beginTick - a_calibration.beginTick;
    const auto cpuDelta = static_cast<std::int64_t>(a_calibration.cpuTick) - a_submit.qpcAfter;
    const double nanos = static_cast<double>(gpuDelta) * 1e9 / a_calibration.frequency +
                         static_cast<double>(cpuDelta) * 1e9 / a_qpcFrequency;
    if (nanos < -1e12 || nanos > 1e12)
    {
        return false;
    }
    a_duration = static_cast<std::int64_t>(nanos);
    return true;
}
} // namespace cue::test
