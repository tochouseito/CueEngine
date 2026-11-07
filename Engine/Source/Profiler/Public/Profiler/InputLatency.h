#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

#include <Platform/InputEventTiming.h>
#include <Profiler/Profiler.h>

namespace cue
{
/// @brief 反映した入力イベント単位の直近 120 標本と、全期間の対応付け件数を所有する
/// CPU 完了は Host の Render Callback 終了点、Present は API 呼出前後。GPU / 表示遅延を含まない
struct InputLatencyInfo final
{
    // 入力到達から Consumer の Frame 採取完了まで
    TimingStatistics toFrameCapture;
    // 入力到達から、その Frame の Render 開始まで
    TimingStatistics toRenderStart;
    // 入力到達から Present API の呼出し開始まで
    TimingStatistics toPresentStart;
    // 入力到達から Present API の復帰まで
    TimingStatistics toPresentReturn;
    // 入力到達から Host の CPU 描画 Callback の完了まで
    TimingStatistics toCpuComplete;
    // Window に到達したイベントの累計
    std::uint64_t observedEvents = 0;
    // Consumer が Frame へ割り当てたイベントの累計
    std::uint64_t assignedEvents = 0;
    // 実際に描画・Present されたイベントの累計
    std::uint64_t presentedEvents = 0;
    // 非表示・取消・描画失敗によって計測対象から外した件数
    std::uint64_t skippedEvents = 0;
    // 時刻順序や ID が不正で分布へ加えなかった件数
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
    /// @brief 単体利用の記録領域を所有する。生成時の Allocation 失敗は生成境界へ伝播する
    InputLatencySamples() : InputLatencySamples(nullptr)
    {
    }

    /// @brief 既存 Profiler へ五つの系列を登録する。nullptr は単体利用の Registry を所有する
    explicit InputLatencySamples(Profiler *a_profiler)
        : m_ownedProfiler(a_profiler ? nullptr : std::make_unique<Profiler>()),
          m_capture((a_profiler ? a_profiler : m_ownedProfiler.get())->create_metric("Input.ToFrameCapture")),
          m_render((a_profiler ? a_profiler : m_ownedProfiler.get())->create_metric("Input.ToRenderStart")),
          m_presentStart((a_profiler ? a_profiler : m_ownedProfiler.get())->create_metric("Input.ToPresentStart")),
          m_presentReturn((a_profiler ? a_profiler : m_ownedProfiler.get())->create_metric("Input.ToPresentReturn")),
          m_complete((a_profiler ? a_profiler : m_ownedProfiler.get())->create_metric("Input.ToCpuComplete"))
    {
    }

    /// @brief Window で観測した件数と、この Frame に反映した件数を独立して数える
    void assigned(std::size_t a_observed, std::size_t a_assigned) noexcept
    {
        // Window 到達数と Consumer が採用した数を独立して数える
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
            // 入力 ID と各時刻の順序が崩れた標本を除外する
            if (event.eventId == 0 || event.received > a_captured || a_captured > a_renderStart ||
                a_renderStart > a_presentStart || a_presentStart > a_presentReturn || a_presentReturn > a_cpuComplete)
            {
                ++m_info.invalidEvents;
                continue;
            }
            // すべての区間を同じ入力到達時刻から測る
            m_capture.add(a_captured - event.received);
            m_render.add(a_renderStart - event.received);
            m_presentStart.add(a_presentStart - event.received);
            m_presentReturn.add(a_presentReturn - event.received);
            m_complete.add(a_cpuComplete - event.received);
            // 標本の属する入力・Frame・Present の対応を保持する
            ++m_info.presentedEvents;
            m_info.lastEventId = event.eventId;
            m_info.lastFrame = a_frame;
            m_info.lastPresent = a_present;
        }
    }

    /// @brief 追加済み全標本から正確に維持した最新の分布を返す
    [[nodiscard]] InputLatencyInfo info() const noexcept
    {
        // 件数と各 Metric の分布を所有値へ複写する
        auto result = m_info;
        result.toFrameCapture = m_capture.statistics();
        result.toRenderStart = m_render.statistics();
        result.toPresentStart = m_presentStart.statistics();
        result.toPresentReturn = m_presentReturn.statistics();
        result.toCpuComplete = m_complete.statistics();
        return result;
    }

    /// @brief 登録系列を維持し、件数と有効標本だけを初期化する。外部排他の保持中に呼ぶ
    void reset() noexcept
    {
        // 前回の件数と分布をまとめて初期状態へ戻す
        m_info = {};
        m_capture.reset();
        m_render.reset();
        m_presentStart.reset();
        m_presentReturn.reset();
        m_complete.reset();
    }

  private:
    std::unique_ptr<Profiler> m_ownedProfiler;
    InputLatencyInfo m_info;
    ProfileMetric m_capture;
    ProfileMetric m_render;
    ProfileMetric m_presentStart;
    ProfileMetric m_presentReturn;
    ProfileMetric m_complete;
};
} // namespace cue
