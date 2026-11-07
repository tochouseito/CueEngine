#include "SpikeTraceExport.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <Profiler/TimingTrace.h>
#include <Profiler/TraceBuffer.h>

namespace
{
/// @brief 自分で新規作成した Directory と、登録した出力 File だけを回収する
class ExportFiles final
{
  public:
    /// @brief 衝突した Directory を再利用せず、一意な Test 保存先を確保する
    ExportFiles()
    {
        std::error_code error;
        const auto temporary = std::filesystem::temp_directory_path(error);
        if (error)
        {
            return;
        }
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        for (int attempt = 0; attempt < 16; ++attempt)
        {
            auto candidate =
                temporary / ("CueSpikeTraceExport-" + std::to_string(stamp) + "-" + std::to_string(attempt));
            if (std::filesystem::create_directory(candidate, error))
            {
                m_root = std::move(candidate);
                return;
            }
            if (error)
            {
                return;
            }
        }
    }

    /// @brief 他の File を列挙・再帰削除せず、既知の自分の出力だけを削除する
    ~ExportFiles()
    {
        if (m_root.empty())
        {
            return;
        }
        std::error_code error;
        for (const auto &file : m_outputs)
        {
            if (file.parent_path() == m_root)
            {
                std::filesystem::remove(file, error);
            }
        }
        std::filesystem::remove(m_root, error);
    }

    /// @brief 保存先を作成できたか返す
    [[nodiscard]] bool is_ready() const noexcept
    {
        return !m_root.empty();
    }

    /// @brief 作成し得る二つの File を回収対象へ登録して Prefix を返す
    [[nodiscard]] std::string prefix(const char *a_name)
    {
        const auto base = m_root / a_name;
        m_outputs.push_back(base.string() + "-spans.csv");
        m_outputs.push_back(base.string() + "-gpu.csv");
        return base.string();
    }

  private:
    std::filesystem::path m_root;
    std::vector<std::filesystem::path> m_outputs;
};

/// @brief Exporter が参照する停止状態と記録だけを用意し、Engine / Window は起動しない
struct RecorderFixture final
{
    struct Thread final
    {
        cue::TraceBuffer<cue::TimingTraceSpan> spans;
        unsigned long id = 73;
    };
    struct GpuRecord final
    {
        cue::TimingTraceGpuRecord record;
    };

    std::array<Thread, 1> threads;
    std::atomic<bool> isRecording = false;
    std::atomic<bool> isGpuRecording = false;
    mutable std::atomic<std::size_t> earlyFileWriteAttempts = 0;
    std::atomic<std::size_t> droppedThreads = 0;
    std::atomic<std::size_t> gpuCount = 0;
    std::atomic<std::size_t> gpuDropped = 0;
    cue::TraceBuffer<GpuRecord> gpuRecords{4};
    std::chrono::nanoseconds started{1000};
    bool isCircular = false;

    /// @brief 本物と同じ Profiler Buffer を設定し、循環時の順序を検証する
    RecorderFixture(std::size_t a_capacity, bool a_isCircular) : isCircular(a_isCircular)
    {
        threads[0].spans.resize(a_capacity);
        threads[0].spans.set_circular(a_isCircular);
    }
};

using exporter = cue::test::SpikeTraceExporter<RecorderFixture>;

/// @brief 不成立の項目を明示し、Release 構成でも検証を省略しない
bool check(bool a_condition, const char *a_message)
{
    if (!a_condition)
    {
        std::fprintf(stderr, "SpikeTraceExport: %s\n", a_message);
    }
    return a_condition;
}

/// @brief File の有無を Metadata で確認し、確認自体の失敗も不成立にする
bool is_absent(const std::string &a_file)
{
    std::error_code error;
    const bool exists = std::filesystem::exists(a_file, error);
    return !exists && !error;
}

/// @brief Text Mode で改行差を正規化し、保存された全内容を読む
std::optional<std::string> read_text(const std::string &a_file)
{
    std::ifstream file(a_file);
    if (!file)
    {
        return {};
    }
    std::string text{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    return file.bad() ? std::nullopt : std::optional<std::string>(std::move(text));
}

/// @brief CPU / GPU のどちらかが記録中なら、File 作成前に全保存経路を拒否する
bool rejects_early_export(ExportFiles &a_files)
{
    RecorderFixture recorder(2, false);
    const auto prefix = a_files.prefix("guarded");
    recorder.isRecording.store(true);
    if (!check(!exporter::write(recorder, prefix) && !exporter::write_gpu(recorder, prefix),
               "CPU recording allowed an export"))
    {
        return false;
    }
    recorder.isRecording.store(false);
    recorder.isGpuRecording.store(true);
    return check(!exporter::write(recorder, prefix) && !exporter::write_gpu(recorder, prefix),
                 "GPU recording allowed an export") &&
           check(recorder.earlyFileWriteAttempts.load() == 4, "early export attempts were not counted") &&
           check(is_absent(prefix + "-spans.csv") && is_absent(prefix + "-gpu.csv"), "rejected export created a file");
}

/// @brief 循環保存の古い記録だけを除き、元の Sequence / 時刻 / Cycle の CSV 契約を維持する
bool preserves_circular_cpu_csv(ExportFiles &a_files)
{
    RecorderFixture recorder(3, true);
    for (std::uint64_t frame = 1; frame <= 5; ++frame)
    {
        cue::TimingTraceSpan span;
        span.name = "Pass";
        span.frame = frame;
        span.detail = 7;
        span.outcome = 0;
        span.begin.time = std::chrono::nanoseconds(1000 + frame * 100);
        span.end.time = span.begin.time + std::chrono::nanoseconds(100);
        span.begin.hasThreadCycles = span.end.hasThreadCycles = true;
        span.begin.threadCycles = 100;
        span.end.threadCycles = 117;
        span.begin.processor = 1;
        span.end.processor = 2;
        if (!recorder.threads[0].spans.append(span))
        {
            return false;
        }
    }
    const auto prefix = a_files.prefix("circular");
    if (!check(exporter::write(recorder, prefix), "circular export failed"))
    {
        return false;
    }
    const auto cpu = read_text(prefix + "-spans.csv");
    const auto gpu = read_text(prefix + "-gpu.csv");
    const std::string expected =
        "thread,frame,name,detail,outcome,begin_ns,end_ns,duration_ns,cycles,cycles_valid,cpu_begin,"
        "cpu_end,source_sequence\n"
        "73,3,Pass,7,0,300,400,100,17,1,1,2,2\n"
        "73,4,Pass,7,0,400,500,100,17,1,1,2,3\n"
        "73,5,Pass,7,0,500,600,100,17,1,1,2,4\n";
    return check(cpu && *cpu == expected, "CPU schema, retention order or sequence changed") &&
           check(gpu && *gpu == "kind,frame,queue,queue_type,detail,name,begin_tick,end_tick,frequency,cpu_tick,"
                                "qpc_before,qpc_after,status\n",
                 "empty GPU export did not preserve the header");
}

/// @brief 共通 CPU Tick を既存 QPC 列へ写し、全 GPU 記録と引用符を保持する
bool preserves_gpu_csv(ExportFiles &a_files)
{
    RecorderFixture recorder(1, false);
    for (std::uint64_t index = 0; index < 3; ++index)
    {
        auto &record = recorder.gpuRecords[index].record;
        record.kind = static_cast<std::uint32_t>(index);
        record.frame = 100 + index;
        record.queue = 17;
        record.queueType = 2;
        record.detail = index;
        record.name = "Gpu\"pass";
        record.beginTick = 200;
        record.endTick = 300;
        record.frequency = 1000;
        record.cpuTick = 400;
        record.cpuBefore = 1001;
        record.cpuAfter = 1009;
        record.cpuClockId = 8;
        record.cpuFrequency = 10000000;
    }
    recorder.gpuCount.store(3);
    const auto prefix = a_files.prefix("gpu");
    if (!check(exporter::write(recorder, prefix), "GPU export failed"))
    {
        return false;
    }
    const auto gpu = read_text(prefix + "-gpu.csv");
    const std::string expected = "kind,frame,queue,queue_type,detail,name,begin_tick,end_tick,frequency,cpu_tick,"
                                 "qpc_before,qpc_after,status\n"
                                 "0,100,17,2,0,\"Gpu\"\"pass\",200,300,1000,400,1001,1009,0\n"
                                 "1,101,17,2,1,\"Gpu\"\"pass\",200,300,1000,400,1001,1009,0\n"
                                 "2,102,17,2,2,\"Gpu\"\"pass\",200,300,1000,400,1001,1009,0\n";
    if (!check(gpu && *gpu == expected, "GPU schema changed or records were lost"))
    {
        return false;
    }
    recorder.gpuDropped.store(1);
    return check(!exporter::write_gpu(recorder, prefix), "GPU drops were treated as a complete export");
}
} // namespace

/// @brief Window / GPU を起動せず、外部保存の境界と既存 CSV 互換性を検証する
int main()
{
    ExportFiles files;
    return files.is_ready() && rejects_early_export(files) && preserves_circular_cpu_csv(files) &&
                   preserves_gpu_csv(files)
               ? 0
               : 1;
}
