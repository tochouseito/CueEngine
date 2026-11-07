#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <string>

namespace cue::test
{
/// @brief 停止済みの RAM 記録だけを CSV へ保存する手動調査 Tool の出力層
///
/// a_recorder は全 Worker の Join 後に参照し、保存完了まで所有者が保持する
/// 記録中の誤呼出しは File を開く前に拒否し、従来の検出件数へ加える
/// Windows の QPC を出す既存 CSV Schema は維持し、Profiler の中立的な CPU Tick を対応付ける
template <class RecorderType> class SpikeTraceExporter final
{
  public:
    /// @brief 生 Tick を変換せず保存する。異なる Queue の Tick を直接比較しない
    static bool write_gpu(const RecorderType &a_recorder, const std::string &a_prefix)
    {
        if (a_recorder.isRecording.load() || a_recorder.isGpuRecording.load())
        {
            a_recorder.earlyFileWriteAttempts.fetch_add(1);
            return false;
        }
        std::FILE *file = nullptr;
        if (fopen_s(&file, (a_prefix + "-gpu.csv").c_str(), "w") != 0)
        {
            return false;
        }
        std::fprintf(file, "kind,frame,queue,queue_type,detail,name,begin_tick,end_tick,frequency,cpu_tick,"
                           "qpc_before,qpc_after,status\n");
        const auto count = (std::min)(a_recorder.gpuCount.load(), a_recorder.gpuRecords.size());
        for (std::size_t index = 0; index < count; ++index)
        {
            const auto first = a_recorder.isCircular ? a_recorder.gpuCount.load() - count : 0;
            const auto &r = a_recorder.gpuRecords[(first + index) % a_recorder.gpuRecords.size()].record;
            std::fprintf(file, "%u,%llu,%llu,%u,%llu,\"", r.kind, static_cast<unsigned long long>(r.frame),
                         static_cast<unsigned long long>(r.queue), r.queueType,
                         static_cast<unsigned long long>(r.detail));
            for (const char *c = r.name; *c; ++c)
            {
                if (*c == '"')
                {
                    std::fputc('"', file);
                }
                std::fputc(*c, file);
            }
            std::fprintf(file, "\",%llu,%llu,%llu,%llu,%lld,%lld,%lld\n", static_cast<unsigned long long>(r.beginTick),
                         static_cast<unsigned long long>(r.endTick), static_cast<unsigned long long>(r.frequency),
                         static_cast<unsigned long long>(r.cpuTick), r.cpuBefore, r.cpuAfter, r.status);
        }
        const bool hadError = std::ferror(file) != 0;
        const bool succeeded = std::fclose(file) == 0 && !hadError;
        std::printf("gpu_records=%zu dropped=%zu overwritten=%zu\n", count, a_recorder.gpuDropped.load(),
                    a_recorder.isCircular ? a_recorder.gpuCount.load() - count : 0);
        return succeeded && a_recorder.gpuDropped == 0;
    }

    /// @brief 全 Worker 停止後にだけ File を作り、欠落件数を明記する
    static bool write(const RecorderType &a_recorder, const std::string &a_prefix)
    {
        // 終了前に誤って呼ばれても File を開かず、計測中の結果保存を拒否する
        if (a_recorder.isRecording.load() || a_recorder.isGpuRecording.load())
        {
            a_recorder.earlyFileWriteAttempts.fetch_add(1);
            return false;
        }
        std::FILE *file = nullptr;
        if (fopen_s(&file, (a_prefix + "-spans.csv").c_str(), "w") != 0)
        {
            return false;
        }
        std::fprintf(file, "thread,frame,name,detail,outcome,begin_ns,end_ns,duration_ns,cycles,cycles_valid,cpu_begin,"
                           "cpu_end,source_sequence\n");
        std::size_t dropped = a_recorder.droppedThreads.load();
        std::size_t count = 0;
        for (const auto &thread : a_recorder.threads)
        {
            dropped += thread.spans.dropped_count();
            const auto retained = thread.spans.retained_count();
            count += retained;
            const auto first = thread.spans.first_sequence();
            if (retained)
            {
                std::printf("retention thread=%lu total=%zu retained=%zu first_sequence=%zu begin_ns=%lld\n", thread.id,
                            thread.spans.accepted_count(), retained, first,
                            (thread.spans[first % thread.spans.size()].begin.time - a_recorder.started).count());
            }
            for (std::size_t index = 0; index < retained; ++index)
            {
                const auto &span = thread.spans[(first + index) % thread.spans.size()];
                const bool hasCycles = span.begin.hasThreadCycles && span.end.hasThreadCycles &&
                                       span.end.threadCycles >= span.begin.threadCycles;
                std::fprintf(
                    file, "%lu,%llu,%s,%llu,%u,%lld,%lld,%lld,%llu,%d,%u,%u,%zu\n", thread.id,
                    static_cast<unsigned long long>(span.frame), span.name,
                    static_cast<unsigned long long>(span.detail), span.outcome,
                    (span.begin.time - a_recorder.started).count(), (span.end.time - a_recorder.started).count(),
                    (span.end.time - span.begin.time).count(),
                    static_cast<unsigned long long>(hasCycles ? span.end.threadCycles - span.begin.threadCycles : 0),
                    hasCycles, span.begin.processor, span.end.processor, first + index);
            }
        }
        const bool hadError = std::ferror(file) != 0;
        const bool succeeded = std::fclose(file) == 0 && !hadError;
        std::printf("trace_spans=%zu dropped=%zu\n", count, dropped);
        return succeeded && dropped == 0 && write_gpu(a_recorder, a_prefix);
    }
};
} // namespace cue::test
