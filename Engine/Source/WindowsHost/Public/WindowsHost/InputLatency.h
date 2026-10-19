#pragma once

#include <cstdint>
#include <span>

#include <Foundation/TimingSamples.h>
#include <Platform/InputEventTiming.h>

namespace cue
{
/// @brief 反映した入力イベント単位の直近 120 標本と、全期間の対応付け件数を所有する
/// CPU 完了は Host の Render Callback 終了点、Present は API 呼出前後。GPU / 表示遅延を含まない
struct InputLatencyInfo final
{
    TimingStatistics toFrameCapture;
    TimingStatistics toRenderStart;
    TimingStatistics toPresentStart;
    TimingStatistics toPresentReturn;
    TimingStatistics toCpuComplete;
    std::uint64_t observedEvents = 0;
    std::uint64_t assignedEvents = 0;
    std::uint64_t presentedEvents = 0;
    std::uint64_t skippedEvents = 0;
    std::uint64_t invalidEvents = 0;
    std::uint64_t lastEventId = 0;
    std::uint64_t lastFrame = 0;
    std::uint64_t lastPresent = 0;
};

/// @brief 入力の到達と採用 Frame の処理点を対応付け、標本を間引かず集計する
/// Host が所有し、全操作は外部 Lock で直列化する。入力のない Frame は 0 ms として追加しない
class InputLatencySamples final
{
  public:
    /// @brief Window で観測した件数と、この Frame に反映した件数を独立して数える
    void assigned(std::size_t a_observed, std::size_t a_assigned) noexcept
    {
        m_info.observedEvents += a_observed;
        m_info.assignedEvents += a_assigned;
    }

    /// @brief Present しなかった Frame の入力は成功標本へ混ぜない
    void skipped(std::size_t a_count) noexcept
    {
        m_info.skippedEvents += a_count;
    }

    /// @brief 同じ入力を再利用せず、成功した Frame の全入力から各区間を記録する
    void presented(std::span<const InputEventTiming> a_events, std::uint64_t a_frame, std::uint64_t a_present,
                   std::chrono::steady_clock::time_point a_captured,
                   std::chrono::steady_clock::time_point a_renderStart,
                   std::chrono::steady_clock::time_point a_presentStart,
                   std::chrono::steady_clock::time_point a_presentReturn,
                   std::chrono::steady_clock::time_point a_cpuComplete) noexcept
    {
        for (const auto &event : a_events)
        {
            if (event.eventId == 0 || event.received > a_captured || a_captured > a_renderStart ||
                a_renderStart > a_presentStart || a_presentStart > a_presentReturn || a_presentReturn > a_cpuComplete)
            {
                ++m_info.invalidEvents;
                continue;
            }
            m_capture.add(a_captured - event.received);
            m_render.add(a_renderStart - event.received);
            m_presentStart.add(a_presentStart - event.received);
            m_presentReturn.add(a_presentReturn - event.received);
            m_complete.add(a_cpuComplete - event.received);
            ++m_info.presentedEvents;
            m_info.lastEventId = event.eventId;
            m_info.lastFrame = a_frame;
            m_info.lastPresent = a_present;
        }
    }

    /// @brief 追加済み全標本から正確に維持した最新の分布を返す
    [[nodiscard]] InputLatencyInfo info() const noexcept
    {
        auto result = m_info;
        result.toFrameCapture = m_capture.statistics();
        result.toRenderStart = m_render.statistics();
        result.toPresentStart = m_presentStart.statistics();
        result.toPresentReturn = m_presentReturn.statistics();
        result.toCpuComplete = m_complete.statistics();
        return result;
    }

  private:
    InputLatencyInfo m_info;
    TimingSamples m_capture;
    TimingSamples m_render;
    TimingSamples m_presentStart;
    TimingSamples m_presentReturn;
    TimingSamples m_complete;
};
} // namespace cue
