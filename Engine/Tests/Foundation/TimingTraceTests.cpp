#include <Profiler/TimingTrace.h>

#include <array>
#include <atomic>
#include <thread>

namespace
{
struct Records final
{
    std::array<cue::TimingTraceSpan, 8> spans{};
    std::atomic<std::size_t> count{0};

    /// @brief 決定的な Clock を使い、実時間の揺れを判定へ混ぜない
    static cue::TimingTracePoint capture(void *) noexcept
    {
        return {std::chrono::nanoseconds(1), 2, 0, true};
    }

    /// @brief 各 Thread に別の要素を割り当て、Join 後にだけ参照する
    static void record(void *a_context, const cue::TimingTraceSpan &a_span) noexcept
    {
        auto &records = *static_cast<Records *>(a_context);
        const auto index = records.count.fetch_add(1);
        if (index < records.spans.size())
        {
            records.spans[index] = a_span;
        }
    }
};
} // namespace

/// @brief Frame の復元、Thread 分離、終了の重複防止と通常 Build の無効化を確認する
int main()
{
    Records records;
    cue::TimingTraceSink sink{&records, Records::capture, Records::record};
    cue::set_timing_trace_sink(&sink);
    {
        cue::TimingTraceFrame outer(10);
        {
            cue::TimingTraceFrame inner(20);
            cue::TimingTraceScope span("inner", 7);
            span.finish(3);
            span.finish();
        }
        cue::TimingTraceScope span("outer");
    }
    std::thread worker([]() { cue::TimingTraceScope span("worker"); });
    worker.join();
    cue::set_timing_trace_sink(nullptr);
    cue::TimingTraceScope disabled("disabled");
#if defined(CUE_ENABLE_TIMING_TRACE)
    if (records.count != 3 || records.spans[0].frame != 20 || records.spans[0].detail != 7 ||
        records.spans[0].outcome != 3 || records.spans[1].frame != 10 ||
        records.spans[2].frame != cue::k_unknownTimingFrame)
    {
        return 1;
    }
#else
    if (records.count != 0)
    {
        return 1;
    }
#endif
    return 0;
}
