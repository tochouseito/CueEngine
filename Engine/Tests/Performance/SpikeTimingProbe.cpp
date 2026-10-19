#include "SpikeTrigger.h"

#include <EditorHost/EditorHost.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// TraceLogging SDK は先に Win32 型の定義を要求する
#include <Windows.h>

#include <TraceLoggingProvider.h>
#include <imgui.h>

#include <Foundation/TimingTrace.h>

namespace
{
using Clock = std::chrono::steady_clock;
inline constexpr std::size_t k_threadCapacity = 4;
inline constexpr std::size_t k_spanCapacity = 262144;

// SDK が要求する Provider Handle を、main が所有する EtwMarkers の寿命内だけ登録する
TRACELOGGING_DEFINE_PROVIDER(g_spikeProvider, "CueEngine.SpikeTiming",
                             (0x9d765397, 0xa1ac, 0x4d0f, 0x9a, 0xe5, 0x0f, 0xa7, 0x79, 0xd4, 0x7f, 0x90));

/// @brief 手動 Probe だけ ETW へ区間と Clock 対応を出力する。全 Worker 停止後に登録解除する
class EtwMarkers final
{
  public:
    struct Sync final
    {
        LONGLONG qpcBefore = 0;
        LONGLONG qpcAfter = 0;
        LONGLONG steadyNs = 0;
    };

    /// @brief 要求時だけ登録し、収集 Session がない場合は未取得を明示する
    explicit EtwMarkers(bool a_isRequested) : m_isRequested(a_isRequested)
    {
        if (m_isRequested)
        {
            m_isRegistered = SUCCEEDED(TraceLoggingRegister(g_spikeProvider));
        }
    }

    /// @brief Sink の借用が全て終わってから Native Provider を解除する
    ~EtwMarkers()
    {
        if (m_isRegistered)
        {
            TraceLoggingUnregister(g_spikeProvider);
        }
    }

    /// @brief ETW 要求時は登録と Level / Keyword の有効化を検証する
    bool is_ready() const noexcept
    {
        return !m_isRequested || (m_isRegistered && TraceLoggingProviderEnabled(g_spikeProvider, 5, 1));
    }

    /// @brief QPC で単調 Clock の取得を挟み、対応付けの不確かさを区間として残す
    bool synchronize(const char *a_phase, Sync &a_sync) const noexcept
    {
        LARGE_INTEGER frequency{}, before{}, after{};
        if (!QueryPerformanceFrequency(&frequency) || !QueryPerformanceCounter(&before))
        {
            return false;
        }
        a_sync.steadyNs = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
        if (!QueryPerformanceCounter(&after))
        {
            return false;
        }
        a_sync.qpcBefore = before.QuadPart;
        a_sync.qpcAfter = after.QuadPart;
        TraceLoggingWrite(g_spikeProvider, "ClockSync", TraceLoggingLevel(5), TraceLoggingKeyword(1),
                          TraceLoggingString(a_phase, "Phase"), TraceLoggingInt64(before.QuadPart, "QpcBefore"),
                          TraceLoggingInt64(after.QuadPart, "QpcAfter"), TraceLoggingInt64(a_sync.steadyNs, "SteadyNs"),
                          TraceLoggingInt64(frequency.QuadPart, "Frequency"));
        return true;
    }

    /// @brief 全区間を間引かず出力する。実時刻は ETW Header ではなく開始・終了 Payload を使用する
    static void write(const cue::TimingTraceSpan &a_span, std::size_t a_sequence, DWORD a_thread) noexcept
    {
        TraceLoggingWrite(g_spikeProvider, "TimingSpan", TraceLoggingLevel(5), TraceLoggingKeyword(1),
                          TraceLoggingUInt64(a_span.frame, "Frame"), TraceLoggingString(a_span.name, "Name"),
                          TraceLoggingUInt64(a_sequence, "Sequence"), TraceLoggingUInt32(a_thread, "OriginalThreadId"),
                          TraceLoggingInt64(a_span.begin.time.count(), "BeginNs"),
                          TraceLoggingInt64(a_span.end.time.count(), "EndNs"),
                          TraceLoggingUInt64(a_span.detail, "Detail"), TraceLoggingUInt32(a_span.outcome, "Outcome"));
    }

  private:
    bool m_isRequested = false;
    bool m_isRegistered = false;
};

/// @brief 公式の Allocator Hook で ImGui の確保だけを観測し、既存 Allocator へ委譲する
class AllocationProbe final
{
  public:
    /// @brief ImGui Context 起動前に登録し、全 Context 破棄後まで所有する
    AllocationProbe()
    {
        ImGui::GetAllocatorFunctions(&m_allocate, &m_free, &m_context);
        ImGui::SetAllocatorFunctions(allocate, release, this);
    }

    /// @brief 元の Allocator を復元し、Process 外の設定は変更しない
    ~AllocationProbe()
    {
        ImGui::SetAllocatorFunctions(m_allocate, m_free, m_context);
    }

  private:
    /// @brief 確保 Size と期間を保持する。記録先は事前確保済みで再帰的な ImGui 確保をしない
    static void *allocate(std::size_t a_size, void *a_context)
    {
        cue::TimingTraceScope trace("ImGui.Allocate", a_size);
        auto &owner = *static_cast<AllocationProbe *>(a_context);
        return owner.m_allocate(a_size, owner.m_context);
    }

    /// @brief 解放は既存の所有規約を保って元の Callback へ渡す
    static void release(void *a_memory, void *a_context)
    {
        auto &owner = *static_cast<AllocationProbe *>(a_context);
        owner.m_free(a_memory, owner.m_context);
    }

    ImGuiMemAllocFunc m_allocate = nullptr;
    ImGuiMemFreeFunc m_free = nullptr;
    void *m_context = nullptr;
};

/// @brief Thread ごとの事前確保配列だけへ書き、計測中の排他と File 出力を避ける
class Recorder final
{
  public:
    struct Thread final
    {
        std::vector<cue::TimingTraceSpan> spans;
        DWORD id = 0;
        std::size_t count = 0;
        std::size_t dropped = 0;
    };
    std::array<Thread, k_threadCapacity> threads;
    std::atomic<std::size_t> nextThread{0};
    std::atomic<std::size_t> droppedThreads{0};
    std::atomic<bool> isRecording{false};
    std::atomic<bool> isGpuRecording{false};
    std::chrono::nanoseconds started{};
    bool emitsEtw = false;
    bool isCircular = false;
    cue::test::SpikeTrigger trigger;
    std::uint64_t qpcFrequency = 0;

    struct GpuRecord final
    {
        cue::TimingTraceGpuRecord record;
        std::array<char, 128> name{};
        std::atomic_flag writing = ATOMIC_FLAG_INIT;
        std::size_t sequence = 0;
    };
    std::vector<GpuRecord> gpuRecords{262144};
    std::atomic<std::size_t> gpuCount{0};
    std::atomic<std::size_t> gpuDropped{0};

    /// @brief 長い手動計測も測定開始前に容量を確保し、測定中の再確保と区間の間引きを避ける
    explicit Recorder(std::size_t a_spanCapacity)
    {
        for (auto &thread : threads)
        {
            thread.spans.resize(a_spanCapacity);
        }
    }

    /// @brief Cycle は CPU 時間に換算せず、実行した仕事量の補助指標として保持する
    static cue::TimingTracePoint capture(void *a_context) noexcept
    {
        if (!static_cast<Recorder *>(a_context)->isRecording.load(std::memory_order_acquire))
        {
            return {};
        }
        cue::TimingTracePoint point;
        ULONG64 cycles = 0;
        point.hasThreadCycles = QueryThreadCycleTime(GetCurrentThread(), &cycles) != 0;
        point.threadCycles = cycles;
        point.processor = GetCurrentProcessorNumber();
        point.time = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch());
        return point;
    }

    /// @brief Worker 終了まで配列を読まない。Trigger 待受では古い履歴だけ循環上書きし、通常記録の不足は数える
    static void record(void *a_context, const cue::TimingTraceSpan &a_span) noexcept
    {
        auto &recorder = *static_cast<Recorder *>(a_context);
        if (!recorder.isRecording.load(std::memory_order_acquire) || a_span.begin.time < recorder.started)
        {
            return;
        }
        thread_local Recorder *owner = nullptr;
        thread_local std::size_t index = k_threadCapacity;
        if (owner != &recorder)
        {
            owner = &recorder;
            index = recorder.nextThread.fetch_add(1, std::memory_order_relaxed);
            if (index < k_threadCapacity)
            {
                recorder.threads[index].id = GetCurrentThreadId();
            }
        }
        if (index >= k_threadCapacity)
        {
            recorder.droppedThreads.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        auto &thread = recorder.threads[index];
        if (!recorder.isCircular && thread.count == thread.spans.size())
        {
            ++thread.dropped;
            return;
        }
        thread.spans[thread.count % thread.spans.size()] = a_span;
        ++thread.count;
        if (recorder.isCircular && std::strcmp(a_span.name, "Frame.Render") == 0)
        {
            recorder.trigger.consider(1, a_span.frame, (a_span.end.time - a_span.begin.time).count(),
                                      a_span.end.time.count());
        }
        if (recorder.emitsEtw)
        {
            EtwMarkers::write(a_span, thread.count - 1, thread.id);
        }
    }

    /// @brief 完了回収は CPU 計測停止後も Shutdown まで保持する。名前は借用期間内に複写する
    static void record_gpu(void *a_context, const cue::TimingTraceGpuRecord &a_record) noexcept
    {
        auto &owner = *static_cast<Recorder *>(a_context);
        if (!owner.isGpuRecording.load(std::memory_order_acquire))
        {
            return;
        }
        const auto index = owner.gpuCount.fetch_add(1, std::memory_order_relaxed);
        if ((!owner.isCircular && index >= owner.gpuRecords.size()) || std::strlen(a_record.name) >= 128)
        {
            owner.gpuDropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        auto &target = owner.gpuRecords[index % owner.gpuRecords.size()];
        if (target.writing.test_and_set(std::memory_order_acquire))
        {
            owner.gpuDropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (target.sequence > index)
        {
            target.writing.clear(std::memory_order_release);
            owner.gpuDropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        target.sequence = index;
        target.record = a_record;
        std::memcpy(target.name.data(), a_record.name, std::strlen(a_record.name) + 1);
        target.record.name = target.name.data();
        target.writing.clear(std::memory_order_release);
        if (owner.isCircular && owner.isRecording.load(std::memory_order_acquire))
        {
            // この Thread が提出・回収した Frame だけを照合する。共有 Ring を並行して読み返さない
            struct Frame final
            {
                cue::TimingTraceGpuRecord cal;
                cue::TimingTraceGpuRecord submit;
            };
            thread_local std::array<Frame, 64> frames;
            auto &frame = frames[a_record.frame % frames.size()];
            if (a_record.kind == 0 && std::strcmp(a_record.name, "BeforeSubmit") == 0)
            {
                frame.cal = a_record;
            }
            else if (a_record.kind == 1)
            {
                frame.submit = a_record;
            }
            else if (a_record.kind == 2 && a_record.detail == 0)
            {
                std::int64_t lead = 0;
                if (cue::test::gpu_submit_lead(frame.cal, frame.submit, a_record, owner.qpcFrequency, lead))
                {
                    const auto now =
                        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch());
                    owner.trigger.consider(2, a_record.frame, lead, now.count());
                }
            }
        }
    }

    /// @brief 生 Tick を変換せず保存する。異なる Queue の Tick を直接比較しない
    bool write_gpu(const std::string &a_prefix) const
    {
        std::FILE *file = nullptr;
        if (fopen_s(&file, (a_prefix + "-gpu.csv").c_str(), "w") != 0)
        {
            return false;
        }
        std::fprintf(file, "kind,frame,queue,queue_type,detail,name,begin_tick,end_tick,frequency,cpu_tick,"
                           "qpc_before,qpc_after,status\n");
        const auto count = (std::min)(gpuCount.load(), gpuRecords.size());
        for (std::size_t index = 0; index < count; ++index)
        {
            const auto first = isCircular ? gpuCount.load() - count : 0;
            const auto &r = gpuRecords[(first + index) % gpuRecords.size()].record;
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
                         static_cast<unsigned long long>(r.cpuTick), r.qpcBefore, r.qpcAfter, r.status);
        }
        const bool hadError = std::ferror(file) != 0;
        const bool succeeded = std::fclose(file) == 0 && !hadError;
        std::printf("gpu_records=%zu dropped=%zu overwritten=%zu\n", count, gpuDropped.load(),
                    isCircular ? gpuCount.load() - count : 0);
        return succeeded && gpuDropped == 0;
    }

    /// @brief 全 Worker 停止後にだけ File を作り、欠落件数を明記する
    bool write(const std::string &a_prefix) const
    {
        std::FILE *file = nullptr;
        if (fopen_s(&file, (a_prefix + "-spans.csv").c_str(), "w") != 0)
        {
            return false;
        }
        std::fprintf(file, "thread,frame,name,detail,outcome,begin_ns,end_ns,duration_ns,cycles,cycles_valid,cpu_begin,"
                           "cpu_end,source_sequence\n");
        std::size_t dropped = droppedThreads.load();
        std::size_t count = 0;
        for (const auto &thread : threads)
        {
            dropped += thread.dropped;
            const auto retained = (std::min)(thread.count, thread.spans.size());
            count += retained;
            const auto first = isCircular ? thread.count - retained : 0;
            if (retained)
            {
                std::printf("retention thread=%lu total=%zu retained=%zu first_sequence=%zu begin_ns=%lld\n", thread.id,
                            thread.count, retained, first,
                            (thread.spans[first % thread.spans.size()].begin.time - started).count());
            }
            for (std::size_t index = 0; index < retained; ++index)
            {
                const auto &span = thread.spans[(first + index) % thread.spans.size()];
                const bool hasCycles = span.begin.hasThreadCycles && span.end.hasThreadCycles &&
                                       span.end.threadCycles >= span.begin.threadCycles;
                std::fprintf(
                    file, "%lu,%llu,%s,%llu,%u,%lld,%lld,%lld,%llu,%d,%u,%u,%zu\n", thread.id,
                    static_cast<unsigned long long>(span.frame), span.name,
                    static_cast<unsigned long long>(span.detail), span.outcome, (span.begin.time - started).count(),
                    (span.end.time - started).count(), (span.end.time - span.begin.time).count(),
                    static_cast<unsigned long long>(hasCycles ? span.end.threadCycles - span.begin.threadCycles : 0),
                    hasCycles, span.begin.processor, span.end.processor, first + index);
            }
        }
        const bool hadError = std::ferror(file) != 0;
        const bool succeeded = std::fclose(file) == 0 && !hadError;
        std::printf("trace_spans=%zu dropped=%zu\n", count, dropped);
        return succeeded && dropped == 0 && write_gpu(a_prefix);
    }
};

/// @brief 計測区間の Process CPU 使用時間を秒で返す
double cpu_seconds()
{
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user))
    {
        return -1.0;
    }
    ULARGE_INTEGER k{}, u{};
    k.LowPart = kernel.dwLowDateTime;
    k.HighPart = kernel.dwHighDateTime;
    u.LowPart = user.dwLowDateTime;
    u.HighPart = user.dwHighDateTime;
    return static_cast<double>(k.QuadPart + u.QuadPart) * 1e-7;
}

/// @brief 自身の Window だけを前面にし、他 Process の入力を操作しない
BOOL CALLBACK foreground_window(HWND a_window, LPARAM a_context)
{
    DWORD process = 0;
    GetWindowThreadProcessId(a_window, &process);
    if (process == GetCurrentProcessId() && IsWindowVisible(a_window))
    {
        *reinterpret_cast<HWND *>(a_context) = a_window;
        SetForegroundWindow(a_window);
        return false;
    }
    return true;
}

/// @brief Host Step 境界の状態を保持する。前面化の成否を推定せず、変化だけ Trace に残す
struct WindowObservation final
{
    HWND window = nullptr;
    std::size_t checks = 0;
    std::size_t mismatches = 0;
    std::size_t changes = 0;
    std::uint64_t previous = ~std::uint64_t{0};
    std::uint64_t initial = 0;

    /// @brief 可視 Flag は遮蔽の判定ではない。前面・最小化・Client Size の異常を別 Bit に保持する
    void sample()
    {
        RECT client{};
        const bool hasClient = GetClientRect(window, &client) != 0;
        const std::uint64_t state = (GetForegroundWindow() == window ? 1u : 0u) | (IsWindowVisible(window) ? 2u : 0u) |
                                    (IsIconic(window) ? 4u : 0u) |
                                    (hasClient && client.right == 1280 && client.bottom == 720 ? 8u : 0u);
        if (checks == 0)
        {
            initial = state;
        }
        ++checks;
        mismatches += state == 11 ? 0 : 1;
        if (state != previous)
        {
            cue::TimingTraceScope trace("Probe.WindowState", state);
            changes += previous == ~std::uint64_t{0} ? 0 : 1;
            previous = state;
        }
    }
};
} // namespace

/// @brief 保存設定に触れず実 Window の全区間を採取する手動調査 Target
int main(int a_argc, char **a_argv)
{
#if !defined(CUE_ENABLE_TIMING_TRACE)
    std::fprintf(stderr, "Configure with -DCUE_ENABLE_TIMING_TRACE=ON\n");
    return 1;
#endif
    if (a_argc < 8 || a_argc > 14)
    {
        std::fprintf(stderr, "buffers waitable queued ui seconds prefix traceEnabled "
                             "[emitEtw [requireForeground [deferEtw [triggered [renderMs [gpuMs]]]]]]\n");
        return 1;
    }
    const auto buffers = static_cast<std::uint32_t>(std::atoi(a_argv[1]));
    const bool waitable = std::atoi(a_argv[2]) != 0;
    const auto queued = static_cast<std::uint32_t>(std::atoi(a_argv[3]));
    const int ui = std::atoi(a_argv[4]);
    const double seconds = std::atof(a_argv[5]);
    const std::string prefix = a_argv[6];
    const bool isTracing = std::atoi(a_argv[7]) != 0;
    const bool emitsEtw = a_argc >= 9 && std::atoi(a_argv[8]) != 0;
    const bool requiresForeground = a_argc >= 10 && std::atoi(a_argv[9]) != 0;
    const bool defersEtw = a_argc >= 11 && std::atoi(a_argv[10]) != 0;
    const bool isTriggered = a_argc >= 12 && std::atoi(a_argv[11]) != 0;
    const double renderMs = a_argc >= 13 ? std::atof(a_argv[12]) : 20.0;
    const double gpuMs = a_argc >= 14 ? std::atof(a_argv[13]) : 5.0;
    if (buffers < 1 || buffers > 3 || queued < 1 || queued > 2 || seconds <= 0.0 ||
        seconds > (isTriggered ? 180.0 : 30.0) || renderMs <= 0 || gpuMs <= 0 ||
        (isTriggered && (!emitsEtw || !defersEtw || seconds < 8.0)) || (emitsEtw && !isTracing))
    {
        return 1;
    }
    auto recorder = std::make_unique<Recorder>(isTriggered ? 524288 : (seconds > 5.0 ? 1048576 : k_spanCapacity));
    recorder->isCircular = isTriggered;
    recorder->trigger.renderThreshold = static_cast<std::int64_t>(renderMs * 1e6);
    recorder->trigger.gpuThreshold = static_cast<std::int64_t>(gpuMs * 1e6);
    LARGE_INTEGER qpcFrequency{};
    QueryPerformanceFrequency(&qpcFrequency);
    recorder->qpcFrequency = qpcFrequency.QuadPart;
    EtwMarkers markers(emitsEtw);
    if (!markers.is_ready())
    {
        std::fprintf(stderr, "ETW provider registration or collector is unavailable\n");
        return 9;
    }
    recorder->emitsEtw = emitsEtw && !defersEtw;
    cue::TimingTraceSink sink{recorder.get(), Recorder::capture, Recorder::record, Recorder::record_gpu};
    cue::set_timing_trace_sink(isTracing ? &sink : nullptr);
    AllocationProbe allocationsProbe;
    cue::EditorHostConfig config;
    config.window.title = "CueEngine Spike Investigation";
    config.window.clientSize = {1280, 720};
    config.imgui.settingsFile.clear();
    config.performanceSettingsFile.clear();
    config.frame.maxFps = 0;
    config.presentation.isVSyncEnabled = false;
    config.presentation.isTearingAllowed = true;
    config.presentation.isWaitableEnabled = waitable;
    config.presentation.maxQueuedFrames = queued;
    config.gpu = {buffers, buffers};
    auto storage = cue::Path::create(prefix + "-storage");
    if (!storage.has_value())
    {
        return 2;
    }
    config.storage.dataRootOverride = storage.take_value();
    if (ui != 0)
    {
        config.buildUi = [ui]()
        {
            if (ui == 1)
            {
                ImGui::SetNextWindowPos({32.0f, 32.0f}, ImGuiCond_FirstUseEver);
                ImGui::SetNextWindowSize({640.0f, 480.0f}, ImGuiCond_FirstUseEver);
                ImGui::Begin("Test");
                ImGui::TextUnformatted("TEST");
                ImGui::End();
            }
            return cue::Result<void>::success();
        };
    }
    cue::EditorHost host(std::move(config));
    if (!host.initialize().has_value())
    {
        return 3;
    }
    WindowObservation window;
    EnumWindows(foreground_window, reinterpret_cast<LPARAM>(&window.window));
    if (!window.window)
    {
        return 10;
    }
    if (requiresForeground)
    {
        // Windows が前面化を拒否した場合は User の操作を待つ。別 Window の入力を擬似送信しない
        std::printf("awaiting_foreground pid=%lu timeout_seconds=45\n", GetCurrentProcessId());
        std::fflush(stdout);
        const auto deadline = Clock::now() + std::chrono::seconds(45);
        while (GetForegroundWindow() != window.window && Clock::now() < deadline)
        {
            auto step = host.step();
            if (!step.has_value() || !*step.try_value())
            {
                return 11;
            }
            Sleep(10);
        }
        if (GetForegroundWindow() != window.window)
        {
            std::fprintf(stderr, "Foreground was not acquired\n");
            return 11;
        }
    }
    const auto warmupEnd = Clock::now() + std::chrono::seconds(2);
    while (Clock::now() < warmupEnd)
    {
        auto step = host.step();
        if (!step.has_value() || !*step.try_value())
        {
            return 4;
        }
    }
    if (requiresForeground && GetForegroundWindow() != window.window)
    {
        std::fprintf(stderr, "Foreground was lost during warmup\n");
        return 11;
    }
    auto initial = host.frame_progress();
    auto transferInitial = host.ui_transfer_info();
    if (!initial.has_value() || !transferInitial.has_value())
    {
        return 5;
    }
    const double cpuStart = cpu_seconds();
    EtwMarkers::Sync startSync{}, stopSync{};
    if (!markers.synchronize("Start", startSync))
    {
        return 9;
    }
    const auto start = Clock::now();
    recorder->started = std::chrono::duration_cast<std::chrono::nanoseconds>(start.time_since_epoch());
    recorder->trigger.armedAt = recorder->started.count() + 6000000000;
    recorder->isGpuRecording.store(true, std::memory_order_release);
    recorder->isRecording.store(true, std::memory_order_release);
    window.sample();
    const auto end = start + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(seconds));
    while (true)
    {
        const auto triggerAt = recorder->trigger.time.load(std::memory_order_acquire);
        const auto now = Clock::now();
        if (triggerAt ? std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count() >=
                            triggerAt + 2000000000
                      : now >= end)
        {
            break;
        }
        auto step = host.step();
        if (!step.has_value() || !*step.try_value())
        {
            return 6;
        }
        window.sample();
    }
    const auto stopped = Clock::now();
    recorder->isRecording.store(false, std::memory_order_release);
    if (!markers.synchronize("Stop", stopSync))
    {
        return 9;
    }
    const double cpuElapsed = cpu_seconds() - cpuStart;
    if (isTriggered)
    {
        const auto triggerAt = recorder->trigger.time.load(std::memory_order_acquire);
        std::printf("trigger kind=%u frame=%llu duration_ns=%lld at_ns=%lld render_ms=%.3f gpu_ms=%.3f\n",
                    triggerAt ? recorder->trigger.kind : 0,
                    static_cast<unsigned long long>(triggerAt ? recorder->trigger.frame : 0),
                    triggerAt ? recorder->trigger.duration : 0, triggerAt ? triggerAt - recorder->started.count() : 0,
                    renderMs, gpuMs);
        std::fflush(stdout);
        std::FILE *flag = nullptr;
        if (fopen_s(&flag, (prefix + "-freeze.flag").c_str(), "w") != 0)
        {
            return 12;
        }
        std::fclose(flag);
        const auto freezeDeadline = Clock::now() + std::chrono::seconds(45);
        while (GetFileAttributesA((prefix + "-frozen.flag").c_str()) == INVALID_FILE_ATTRIBUTES &&
               Clock::now() < freezeDeadline)
        {
            Sleep(10);
        }
        if (GetFileAttributesA((prefix + "-frozen.flag").c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            return 12;
        }
    }
    auto final = host.frame_progress();
    auto transferFinal = host.ui_transfer_info();
    if (!final.has_value() || !transferFinal.has_value())
    {
        return 7;
    }
    const auto count = final.try_value()->renderedFrames - initial.try_value()->renderedFrames;
    const auto allocations =
        transferFinal.try_value()->snapshotAllocations - transferInitial.try_value()->snapshotAllocations;
    const double elapsed = std::chrono::duration<double>(stopped - start).count();
    auto graph = host.graph_performance();
    if (graph.has_value())
    {
        // GPU は非同期に完了する直近 120 標本であり、CPU Trace 全区間の統計とは区別する
        for (const auto &pass : graph.try_value()->gpuPasses)
        {
            if (pass.isAvailable)
            {
                std::printf("gpu_last120 name=%s samples=%zu avg_ms=%.6f max_ms=%.6f\n", pass.name.c_str(),
                            pass.statistics.sampleCount,
                            std::chrono::duration<double, std::milli>(pass.statistics.averageDuration).count(),
                            std::chrono::duration<double, std::milli>(pass.statistics.maxDuration).count());
            }
        }
    }
    const bool shutdownSucceeded = host.shutdown().has_value();
    recorder->isGpuRecording.store(false, std::memory_order_release);
    cue::set_timing_trace_sink(nullptr);
    if (emitsEtw && defersEtw)
    {
        // Worker 停止後に全区間を出す。Header の Thread と出力時刻を元区間の識別に使わない
        for (const auto &thread : recorder->threads)
        {
            const auto retained = (std::min)(thread.count, thread.spans.size());
            const auto first = isTriggered ? thread.count - retained : 0;
            for (std::size_t index = 0; index < retained; ++index)
            {
                EtwMarkers::write(thread.spans[(first + index) % thread.spans.size()], index, thread.id);
            }
        }
    }
    if (isTracing)
    {
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        std::printf("etw_clock qpc_frequency=%lld measurement_start_ns=%lld start_qpc_before=%lld start_qpc_after=%lld "
                    "start_steady_ns=%lld "
                    "stop_qpc_before=%lld stop_qpc_after=%lld stop_steady_ns=%lld\n",
                    frequency.QuadPart, recorder->started.count(), startSync.qpcBefore, startSync.qpcAfter,
                    startSync.steadyNs, stopSync.qpcBefore, stopSync.qpcAfter, stopSync.steadyNs);
    }
    std::printf("pid=%lu buffers=%u waitable=%d queued=%u ui=%d trace=%d frames=%llu elapsed=%.6f fps=%.3f "
                "cpu_cores=%.4f snapshot_allocations=%llu\n",
                GetCurrentProcessId(), buffers, waitable, queued, ui, isTracing, static_cast<unsigned long long>(count),
                elapsed, count / elapsed, cpuElapsed / elapsed, static_cast<unsigned long long>(allocations));
    std::printf("window_checks=%zu mismatches=%zu changes=%zu final_state=%llu initial_state=%llu\n", window.checks,
                window.mismatches, window.changes, static_cast<unsigned long long>(window.previous),
                static_cast<unsigned long long>(window.initial));
    return shutdownSucceeded && (!isTracing || recorder->write(prefix)) ? 0 : 8;
}
