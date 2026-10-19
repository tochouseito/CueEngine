#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>

namespace cue
{
inline constexpr auto k_unknownTimingFrame = (std::numeric_limits<std::uint64_t>::max)();

/// @brief Sink が同じ単調 Clock と単位で取得する時刻と任意の Thread Cycle を保持する
struct TimingTracePoint final
{
    std::chrono::nanoseconds time{};
    std::uint64_t threadCycles = 0;
    std::uint32_t processor = 0;
    bool hasThreadCycles = false;
};

/// @brief 集計前の区間を Frame と対応付ける。name は Process 終了まで有効な文字列を借用する
struct TimingTraceSpan final
{
    const char *name = nullptr;
    std::uint64_t frame = k_unknownTimingFrame;
    std::uint64_t detail = 0;
    std::uint32_t outcome = (std::numeric_limits<std::uint32_t>::max)();
    TimingTracePoint begin;
    TimingTracePoint end;
};

/// @brief GPU の生 Tick と CPU QPC を保持する調査記録。name は Callback 中だけ借用する
/// kind は 0: Clock 対応、1: CPU 提出、2: 完了 Pass とする。時刻の変換は解析側で行う
struct TimingTraceGpuRecord final
{
    const char *name = "";
    std::uint64_t frame = k_unknownTimingFrame;
    std::uint64_t queue = 0;
    std::uint64_t detail = 0;
    std::uint64_t beginTick = 0;
    std::uint64_t endTick = 0;
    std::uint64_t frequency = 0;
    std::uint64_t cpuTick = 0;
    std::int64_t qpcBefore = 0;
    std::int64_t qpcAfter = 0;
    std::int64_t status = 0;
    std::uint32_t kind = 0;
    std::uint32_t queueType = 0;
};

/// @brief 調査用の時刻取得と記録先を借用する。Callback は全 Thread から呼ばれ、例外を送出しない
///
/// Owner は Worker 起動前に登録し、全 Worker の終了後に解除してから Sink を破棄する
/// 記録先の容量不足を Sink 側で数える。Frame 処理中の File 出力や動的確保は行わない
struct TimingTraceSink final
{
    void *context = nullptr;
    TimingTracePoint (*capture)(void *) noexcept = nullptr;
    void (*record)(void *, const TimingTraceSpan &) noexcept = nullptr;
    void (*recordGpu)(void *, const TimingTraceGpuRecord &) noexcept = nullptr;
};

namespace timing_trace_detail
{
#if defined(CUE_ENABLE_TIMING_TRACE)
inline std::atomic<TimingTraceSink *> sink{nullptr};
inline thread_local std::uint64_t frame = k_unknownTimingFrame;
#endif
} // namespace timing_trace_detail

/// @brief 完了を回収した時点ではなく、提出した論理 Frame を保存するために使う
inline std::uint64_t timing_trace_frame() noexcept
{
#if defined(CUE_ENABLE_TIMING_TRACE)
    return timing_trace_detail::frame;
#else
    return k_unknownTimingFrame;
#endif
}

/// @brief 調査用の GPU 記録先がある場合だけ Native Clock 取得を行う
inline TimingTraceSink *timing_trace_gpu_sink() noexcept
{
#if defined(CUE_ENABLE_TIMING_TRACE)
    auto *sink = timing_trace_detail::sink.load(std::memory_order_acquire);
    return sink && sink->recordGpu ? sink : nullptr;
#else
    return nullptr;
#endif
}

/// @brief 明示的な調査 Build だけ記録先を登録する。通常 Build では何もしない
inline void set_timing_trace_sink(TimingTraceSink *a_sink) noexcept
{
#if defined(CUE_ENABLE_TIMING_TRACE)
    timing_trace_detail::sink.store(a_sink, std::memory_order_release);
#else
    (void)a_sink;
#endif
}

/// @brief 同じ Thread の下位処理へ論理 Frame ID を渡し、Scope 終了時に元へ戻す
class TimingTraceFrame final
{
  public:
    /// @brief Frame の採用後にだけ呼び、未投入の待機へ別 Frame の ID を付けない
    explicit TimingTraceFrame(std::uint64_t a_frame) noexcept
    {
#if defined(CUE_ENABLE_TIMING_TRACE)
        m_previous = timing_trace_detail::frame;
        timing_trace_detail::frame = a_frame;
#else
        (void)a_frame;
#endif
    }

    /// @brief 呼出元の Frame 対応を復元する
    ~TimingTraceFrame()
    {
#if defined(CUE_ENABLE_TIMING_TRACE)
        timing_trace_detail::frame = m_previous;
#endif
    }
    TimingTraceFrame(const TimingTraceFrame &) = delete;
    TimingTraceFrame &operator=(const TimingTraceFrame &) = delete;

  private:
#if defined(CUE_ENABLE_TIMING_TRACE)
    std::uint64_t m_previous = k_unknownTimingFrame;
#endif
};

/// @brief 通常 Build では消える区間計測。調査 Build の記録先は全 Scope より長く所有する
class TimingTraceScope final
{
  public:
    /// @brief 固定名と任意の Fence 値などを開始点に関連付ける
    explicit TimingTraceScope(const char *a_name, std::uint64_t a_detail = 0) noexcept
    {
#if defined(CUE_ENABLE_TIMING_TRACE)
        m_sink = timing_trace_detail::sink.load(std::memory_order_acquire);
        if (m_sink && m_sink->capture && m_sink->record)
        {
            m_span.name = a_name;
            m_span.frame = timing_trace_detail::frame;
            m_span.detail = a_detail;
            m_span.begin = m_sink->capture(m_sink->context);
        }
        else
        {
            m_sink = nullptr;
        }
#else
        (void)a_name;
        (void)a_detail;
#endif
    }

    /// @brief 早期 Return でも開始済み区間を一度だけ閉じる
    ~TimingTraceScope()
    {
        finish();
    }

    /// @brief Scope 全体より短い待機区間などを明示的に閉じる。複数回呼んでも重複記録しない
    void finish(std::uint32_t a_outcome = (std::numeric_limits<std::uint32_t>::max)()) noexcept
    {
#if defined(CUE_ENABLE_TIMING_TRACE)
        if (m_sink)
        {
            m_span.outcome = a_outcome;
            m_span.end = m_sink->capture(m_sink->context);
            m_sink->record(m_sink->context, m_span);
            m_sink = nullptr;
        }
#else
        (void)a_outcome;
#endif
    }
    TimingTraceScope(const TimingTraceScope &) = delete;
    TimingTraceScope &operator=(const TimingTraceScope &) = delete;

  private:
#if defined(CUE_ENABLE_TIMING_TRACE)
    TimingTraceSink *m_sink = nullptr;
    TimingTraceSpan m_span;
#endif
};
} // namespace cue
