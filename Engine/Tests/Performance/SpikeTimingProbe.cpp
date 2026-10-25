#include "FocusExperiment.h"
#include "SpikeTraceExport.h"
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
#include <optional>
#include <string>
#include <utility>
#include <vector>

// TraceLogging SDK は先に Win32 型の定義を要求する
#include <Windows.h>

#include <TraceLoggingProvider.h>
#include <imgui.h>

#include <DX12/DX12Backend.h>
#include <DX12/DX12RenderDevice.h>
#include <Profiler/TimingTrace.h>
#include <Profiler/TraceBuffer.h>
#include <WindowsHost/WindowsHost.h>

namespace
{
using Clock = std::chrono::steady_clock;
inline constexpr std::size_t k_threadCapacity = 4;
inline constexpr std::size_t k_spanCapacity = 262144;

/// @brief 通常 ImGui Pass と同じ BackBuffer Clear / Barrier を保ち、UI の描画だけを除く
class ProbeBackBufferClearPass final : public cue::FrameGraphPass
{
  public:
    /// @brief GPU Query で ImGui の記録と取り違えない固定名を返す
    [[nodiscard]] const char *name() const noexcept override
    {
        return "BackBufferClear";
    }

    /// @brief 通常の表示 Pass と同じ Graphics Queue を使う
    [[nodiscard]] cue::QueueType type() const noexcept override
    {
        return cue::QueueType::Graphics;
    }

    /// @brief 外部 BackBuffer と設定済み Clear 色を Graph の生成時に保持する
    [[nodiscard]] cue::Result<void> setup(cue::FrameGraphBuilder &a_builder) override
    {
        auto buffer = a_builder.get_texture("BackBuffer");
        if (!buffer.has_value())
        {
            return cue::Result<void>::failure(*buffer.try_error());
        }
        auto desc = a_builder.texture_desc(*buffer.try_value());
        if (!desc.has_value())
        {
            return cue::Result<void>::failure(*desc.try_error());
        }
        m_backBuffer = buffer.take_value();
        m_clearColor = desc.try_value()->clearColor;
        return cue::Result<void>::success();
    }

    /// @brief Present 復帰を含む遷移は通常と同じ Graph に任せる
    [[nodiscard]] cue::Result<void> describe_resources(cue::FrameGraphBuilder &a_builder) override
    {
        return a_builder.use(m_backBuffer, cue::FrameGraphAccess::Write, cue::FrameGraphResourceState::RenderTarget);
    }

    /// @brief UI 版と同じ Clear と RTV 設定だけを記録する
    [[nodiscard]] cue::Result<void> execute(cue::FrameGraphContext &a_context) override
    {
        auto clear = a_context.clear_render_target(m_backBuffer, m_clearColor);
        return clear.has_value() ? a_context.set_render_target(m_backBuffer) : std::move(clear);
    }

  private:
    cue::FrameGraphResourceHandle m_backBuffer;
    std::array<float, 4> m_clearColor{};
};

/// @brief 計測手順を共有し、ImGui 無効時は Context / Backend / UI Callback を一切生成しない
class ProbeHost final
{
  public:
    /// @brief 起動時にだけ Host の所有先を決め、通常の Editor 設定は変更しない
    ProbeHost(cue::EditorHostConfig a_config, bool a_withoutImGui)
    {
        if (!a_withoutImGui)
        {
            m_editor = std::make_unique<cue::EditorHost>(std::move(a_config));
            return;
        }
        cue::WindowsHostConfig config;
        config.window = std::move(a_config.window);
        config.frame = cue::frame_config_for_buffering(a_config.frame, a_config.gpu.resourceFrameCount);
        config.presentation = a_config.presentation;
        config.graph = std::move(a_config.graph);
        config.graph.displayPassFactory = []() { return std::make_unique<ProbeBackBufferClearPass>(); };
        config.storage = std::move(a_config.storage);
        config.fileSystem = std::move(a_config.fileSystem);
        config.logging = a_config.logging;
        config.gpu = a_config.gpu;
        config.performancePolicy = a_config.performancePolicy;
        config.performanceSettingsFile = std::move(a_config.performanceSettingsFile);
        config.simulation = a_config.simulation;
        config.callbacks.simulationUpdate = std::move(a_config.simulationUpdate);
        config.callbacks.prepareFrame = std::move(a_config.prepareFrame);
        config.callbacks.createBackend = std::move(a_config.createBackend);
        m_windows = std::make_unique<cue::WindowsHost>(std::move(config));
    }

    /// @brief 選択した Host の初期化と失敗回収の契約を維持する
    [[nodiscard]] cue::Result<void> initialize()
    {
        return m_editor ? m_editor->initialize() : m_windows->initialize();
    }

    /// @brief 両条件とも同じ Main / Update / Render の進行方式を使う
    [[nodiscard]] cue::Result<bool> step()
    {
        return m_editor ? m_editor->step() : m_windows->step();
    }

    /// @brief 完了 Frame 数は UI の有無にかかわらず Runtime の値を採用する
    [[nodiscard]] cue::Result<cue::FrameProgress> frame_progress() const
    {
        return m_editor ? m_editor->frame_progress() : m_windows->frame_progress();
    }

    /// @brief UI がない条件は Snapshot 自体が存在しないことを零件で示す
    [[nodiscard]] cue::Result<cue::ImGuiTransferInfo> ui_transfer_info() const
    {
        return m_editor ? m_editor->ui_transfer_info() : cue::Result<cue::ImGuiTransferInfo>::success({});
    }

    /// @brief 共通 Graph と GPU Query の集計はどちらの条件も保持する
    [[nodiscard]] cue::Result<cue::MainFrameGraphPerformance> graph_performance() const
    {
        return m_editor ? m_editor->graph_performance() : m_windows->graph_performance();
    }

    /// @brief Worker と GPU を排出してから結果の File 出力を許可する
    [[nodiscard]] cue::Result<void> shutdown()
    {
        return m_editor ? m_editor->shutdown() : m_windows->shutdown();
    }

  private:
    std::unique_ptr<cue::EditorHost> m_editor;
    std::unique_ptr<cue::WindowsHost> m_windows;
};

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
        cue::TraceBuffer<cue::TimingTraceSpan> spans;
        DWORD id = 0;
    };
    std::array<Thread, k_threadCapacity> threads;
    std::atomic<std::size_t> nextThread{0};
    std::atomic<std::size_t> droppedThreads{0};
    std::atomic<bool> isRecording{false};
    std::atomic<bool> isGpuRecording{false};
    mutable std::atomic<std::size_t> earlyFileWriteAttempts{0};
    std::chrono::nanoseconds started{};
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
    cue::TraceBuffer<GpuRecord> gpuRecords{524288};
    std::atomic<std::size_t> gpuCount{0};
    std::atomic<std::size_t> gpuDropped{0};

    /// @brief 長い手動計測も測定開始前に容量を確保し、測定中の再確保と区間の間引きを避ける
    explicit Recorder(std::size_t a_spanCapacity, bool a_isCircular = false) : isCircular(a_isCircular)
    {
        for (auto &thread : threads)
        {
            thread.spans.resize(a_spanCapacity);
            thread.spans.set_circular(a_isCircular);
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

    /// @brief 出力 Adapter へ成功した記録の識別子だけを返し、保存先や送出方法を記録層へ持ち込まない
    struct AppendResult final
    {
        bool isStored = false;
        std::size_t sequence = 0;
        DWORD thread = 0;
    };

    /// @brief Worker 終了まで配列を読まない。Trigger 待受では古い履歴だけ循環上書きし、通常記録の不足は数える
    [[nodiscard]] static AppendResult record(void *a_context, const cue::TimingTraceSpan &a_span) noexcept
    {
        auto &recorder = *static_cast<Recorder *>(a_context);
        if (!recorder.isRecording.load(std::memory_order_acquire) || a_span.begin.time < recorder.started)
        {
            return {};
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
            return {};
        }
        auto &thread = recorder.threads[index];
        if (!thread.spans.append(a_span))
        {
            return {};
        }
        if (recorder.isCircular && std::strcmp(a_span.name, "Frame.Render") == 0)
        {
            recorder.trigger.consider(1, a_span.frame, (a_span.end.time - a_span.begin.time).count(),
                                      a_span.end.time.count());
        }
        return {true, thread.spans.accepted_count() - 1, thread.id};
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
};

/// @brief 明示的な ETW 出力設定を RAM Recorder と分離し、成功した記録だけを送出する
///
/// main が Recorder と Provider より先に破棄しない。Callback は Worker から呼ばれ、例外を送出しない
class TraceOutputAdapter final
{
  public:
    Recorder &recorder;
    bool emitsEtw = false;

    /// @brief Windows の時計取得は手動 Tool 内に閉じ、共通 Profiler の時刻契約へ渡す
    static cue::TimingTracePoint capture(void *a_context) noexcept
    {
        return Recorder::capture(&static_cast<TraceOutputAdapter *>(a_context)->recorder);
    }

    /// @brief 通常は RAM 保存だけを行い、即時 ETW モードを指定した場合だけ外部へ送る
    static void record(void *a_context, const cue::TimingTraceSpan &a_span) noexcept
    {
        auto &adapter = *static_cast<TraceOutputAdapter *>(a_context);
        const auto stored = Recorder::record(&adapter.recorder, a_span);
        if (stored.isStored && adapter.emitsEtw)
        {
            EtwMarkers::write(a_span, stored.sequence, stored.thread);
        }
    }

    /// @brief GPU 記録も同じ RAM Owner へ転送し、終了時の CSV 保存と混ぜない
    static void record_gpu(void *a_context, const cue::TimingTraceGpuRecord &a_record) noexcept
    {
        Recorder::record_gpu(&static_cast<TraceOutputAdapter *>(a_context)->recorder, a_record);
    }

    /// @brief 停止・Join 後の全区間を ETW へ出し、記録中の遅延送出を拒否する
    [[nodiscard]] bool write_deferred() const noexcept
    {
        if (recorder.isRecording.load() || recorder.isGpuRecording.load())
        {
            return false;
        }
        for (const auto &thread : recorder.threads)
        {
            const auto retained = thread.spans.retained_count();
            const auto first = thread.spans.first_sequence();
            for (std::size_t index = 0; index < retained; ++index)
            {
                EtwMarkers::write(thread.spans[(first + index) % thread.spans.size()], index, thread.id);
            }
        }
        return true;
    }
};

/// @brief 早期終了でも Host の Worker 回収後に Sink を解除し、借用 Adapter の失効を防ぐ
class TraceRegistration final
{
  public:
    /// @brief Host より前に構築し、Host の破棄が終わるまで登録先を保持する
    explicit TraceRegistration(cue::TimingTraceSink *a_sink) noexcept
    {
        cue::set_timing_trace_sink(a_sink);
    }

    /// @brief 通常停止済みでも解除を繰り返せる。全 Worker が止まってから呼ばれる
    ~TraceRegistration()
    {
        cue::set_timing_trace_sink(nullptr);
    }

    TraceRegistration(const TraceRegistration &) = delete;
    TraceRegistration &operator=(const TraceRegistration &) = delete;
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
    LONG expectedWidth = 1280;
    LONG expectedHeight = 720;
    HWND window = nullptr;
    std::size_t checks = 0;
    std::size_t mismatches = 0;
    std::size_t changes = 0;
    std::uint64_t previous = ~std::uint64_t{0};
    std::uint64_t initial = 0;
    std::optional<POINT> fixedPointer;
    RECT fixedRect{};
    std::size_t pointerChanges = 0;
    std::size_t windowMoves = 0;

    /// @brief 可視 Flag は遮蔽の判定ではない。前面・最小化・Client Size の異常を別 Bit に保持する
    void sample()
    {
        if (fixedPointer)
        {
            POINT point{};
            RECT rect{};
            pointerChanges += !GetCursorPos(&point) || point.x != fixedPointer->x || point.y != fixedPointer->y;
            windowMoves += !GetWindowRect(window, &rect) || rect.left != fixedRect.left || rect.top != fixedRect.top ||
                           rect.right != fixedRect.right || rect.bottom != fixedRect.bottom;
        }
        RECT client{};
        const bool hasClient = GetClientRect(window, &client) != 0;
        const std::uint64_t state =
            (GetForegroundWindow() == window ? 1u : 0u) | (IsWindowVisible(window) ? 2u : 0u) |
            (IsIconic(window) ? 4u : 0u) |
            (hasClient && client.right == expectedWidth && client.bottom == expectedHeight ? 8u : 0u);
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
    if (a_argc == 6 && std::strcmp(a_argv[1], "--focus-companion") == 0)
    {
        return cue::test::run_focus_companion(
            reinterpret_cast<HWND>(static_cast<std::uintptr_t>(std::strtoull(a_argv[2], nullptr, 10))),
            std::atoi(a_argv[3]), std::atoi(a_argv[4]),
            reinterpret_cast<HWND>(static_cast<std::uintptr_t>(std::strtoull(a_argv[5], nullptr, 10))));
    }
#if !defined(CUE_ENABLE_TIMING_TRACE)
    std::fprintf(stderr, "Configure with -DCUE_ENABLE_TIMING_TRACE=ON\n");
    return 1;
#endif
    if (a_argc < 8 || a_argc > 21 || a_argc == 20)
    {
        std::fprintf(stderr, "buffers waitable queued ui seconds prefix traceEnabled "
                             "[emitEtw [requireForeground [deferEtw [triggered [renderMs [gpuMs "
                             "[focusCycle [focusWindow [focusWidth [adapter [fixedPlacement "
                             "[clientWidth clientHeight]]]]]]]]]]]]\n");
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
    const bool cyclesFocus = a_argc >= 15 && std::atoi(a_argv[14]) != 0;
    const auto focusWindow =
        a_argc >= 16 ? reinterpret_cast<HWND>(static_cast<std::uintptr_t>(std::strtoull(a_argv[15], nullptr, 10)))
                     : nullptr;
    const int focusWidth = a_argc >= 17 ? std::atoi(a_argv[16]) : 1280;
    const std::string adapter = a_argc >= 18 ? a_argv[17] : "auto";
    const bool fixedPlacement = a_argc >= 19 && std::atoi(a_argv[18]) != 0;
    // 既存引数の意味を維持し、描画領域だけを解像度比較用に上書きする
    const int clientWidth = a_argc >= 21 ? std::atoi(a_argv[19]) : focusWidth;
    const int clientHeight = a_argc >= 21 ? std::atoi(a_argv[20]) : 720;
    if (buffers < 1 || buffers > 3 || queued < 1 || queued > 2 || seconds <= 0.0 || ui < 0 || ui > 3 ||
        seconds > (isTriggered ? 180.0 : 30.0) || renderMs <= 0 || gpuMs <= 0 ||
        (isTriggered && (!emitsEtw || !defersEtw || seconds < 8.0)) || (emitsEtw && !isTracing) ||
        (cyclesFocus && (isTriggered || seconds != 20.0 || !requiresForeground)) || (cyclesFocus && fixedPlacement) ||
        clientWidth < 1 || clientWidth > 16384 || clientHeight < 1 || clientHeight > 16384 ||
        (cyclesFocus && (clientWidth != focusWidth || clientHeight != 720)) ||
        (!cyclesFocus && (focusWindow || focusWidth != 1280)) || (focusWidth != 960 && focusWidth != 1280) ||
        (adapter != "auto" && adapter != "intel" && adapter != "nvidia"))
    {
        return 1;
    }
    // 高 FPS の Adapter 比較でも全 Sample を保持し、容量不足で統計を間引かない
    auto recorder =
        std::make_unique<Recorder>(isTriggered ? 524288 : (seconds > 5.0 ? 2097152 : k_spanCapacity), isTriggered);
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
    TraceOutputAdapter outputAdapter{*recorder, emitsEtw && !defersEtw};
    cue::TimingTraceSink sink{&outputAdapter, TraceOutputAdapter::capture, TraceOutputAdapter::record,
                              TraceOutputAdapter::record_gpu};
    TraceRegistration registration(isTracing ? &sink : nullptr);
    std::unique_ptr<AllocationProbe> allocationsProbe;
    if (ui != 3)
    {
        allocationsProbe = std::make_unique<AllocationProbe>();
    }
    cue::EditorHostConfig config;
    config.window.title = "CueEngine Spike Investigation";
    config.window.clientSize = {static_cast<std::uint32_t>(clientWidth), static_cast<std::uint32_t>(clientHeight)};
    config.imgui.settingsFile.clear();
    config.performanceSettingsFile.clear();
    // 測定区間は Trace 配列だけを更新し、Host の File Log と Debug 出力を発生させない
    config.logging.isFileEnabled = false;
    config.logging.debugOutput = cue::DebugLogOutput::Disabled;
    config.frame.maxFps = 0;
    config.presentation.isVSyncEnabled = false;
    config.presentation.isTearingAllowed = true;
    config.presentation.isWaitableEnabled = waitable;
    config.presentation.maxQueuedFrames = queued;
    config.gpu = {buffers, buffers};
    if (adapter != "auto")
    {
        // 優先順だけでは選択結果を保証しないため、実 Device の Vendor と Software Flag を検査する
        config.createBackend = [adapter](cue::IFileSystem *a_files) -> cue::Result<std::unique_ptr<cue::IBackend>>
        {
            const auto selection = adapter == "intel" ? cue::dx12::AdapterSelection::HardwareMinimumPower
                                                      : cue::dx12::AdapterSelection::HardwarePreferred;
            auto result = cue::dx12::DX12Backend::create(selection, {}, a_files);
            if (!result.has_value())
            {
                return cue::Result<std::unique_ptr<cue::IBackend>>::failure(*result.try_error());
            }
            auto backend = result.take_value();
            auto *device = static_cast<cue::dx12::DX12RenderDevice *>(backend->get_render_device());
            DXGI_ADAPTER_DESC1 desc{};
            const HRESULT described = device->adapter()->GetDesc1(&desc);
            if (FAILED(described))
            {
                return cue::Result<std::unique_ptr<cue::IBackend>>::failure(
                    {cue::ErrorCategory::PlatformFailure, "Probe.adapter_description", described});
            }
            const UINT expectedVendor = adapter == "intel" ? 0x8086 : 0x10de;
            if (desc.VendorId != expectedVendor || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0)
            {
                return cue::Result<std::unique_ptr<cue::IBackend>>::failure(
                    {cue::ErrorCategory::InvalidState, "Probe.unexpected_adapter"});
            }
            std::printf("adapter requested=%s vendor=0x%04x device=0x%04x luid=0x%08lx%08lx software=%d\n",
                        adapter.c_str(), desc.VendorId, desc.DeviceId,
                        static_cast<unsigned long>(desc.AdapterLuid.HighPart), desc.AdapterLuid.LowPart,
                        device->is_software_adapter() ? 1 : 0);
            std::fflush(stdout);
            return cue::Result<std::unique_ptr<cue::IBackend>>::success(std::move(backend));
        };
    }
    auto storage = cue::Path::create(prefix + "-storage");
    if (!storage.has_value())
    {
        return 2;
    }
    config.storage.dataRootOverride = storage.take_value();
    if (ui != 0 && ui != 3)
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
    ProbeHost host(std::move(config), ui == 3);
    if (!host.initialize().has_value())
    {
        return 3;
    }
    // Context のない比較を、空の UI を構築した比較と混同しない
    const bool hasImGuiContext = ImGui::GetCurrentContext() != nullptr;
    if (ui == 3 && hasImGuiContext)
    {
        return 15;
    }
    std::printf("ui_mode=%s imgui_initialized=%d current_context=%d host_file_logging=0\n",
                ui == 3 ? "no_imgui" : "imgui", ui != 3, hasImGuiContext);
    WindowObservation window;
    window.expectedWidth = clientWidth;
    window.expectedHeight = clientHeight;
    std::printf("window_expected_size width=%d height=%d\n", clientWidth, clientHeight);
    EnumWindows(foreground_window, reinterpret_cast<LPARAM>(&window.window));
    if (!window.window)
    {
        return 10;
    }
    if (a_argc >= 21)
    {
        // FHD Monitor 上でも装飾枠による寸法制限を避ける。解像度比較の両条件を同じ枠なし形式にする
        RECT requested{0, 0, clientWidth, clientHeight};
        const auto previousStyle = static_cast<DWORD>(GetWindowLongPtrW(window.window, GWL_STYLE));
        const auto style = (previousStyle & ~WS_OVERLAPPEDWINDOW) | WS_POPUP;
        const auto extendedStyle = static_cast<DWORD>(GetWindowLongPtrW(window.window, GWL_EXSTYLE));
        SetLastError(ERROR_SUCCESS);
        const auto replacedStyle = SetWindowLongPtrW(window.window, GWL_STYLE, style);
        if ((replacedStyle == 0 && GetLastError() != ERROR_SUCCESS) ||
            !AdjustWindowRectEx(&requested, style, FALSE, extendedStyle) ||
            !SetWindowPos(window.window, nullptr, 0, 0, requested.right - requested.left,
                          requested.bottom - requested.top,
                          SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED))
        {
            return 16;
        }
        std::printf("window_style=borderless fullscreen_exclusive=0\n");
    }
    RECT actualClient{};
    if (!GetClientRect(window.window, &actualClient))
    {
        return 16;
    }
    std::printf("window_actual_size width=%ld height=%ld\n", actualClient.right, actualClient.bottom);
    if (actualClient.right != clientWidth || actualClient.bottom != clientHeight)
    {
        return 16;
    }
    if (fixedPlacement)
    {
        // 両条件を同じ Monitor の Pointer 下へ配置し、外部 Window の Hit Test を比較へ混ぜない
        POINT point{};
        RECT rect{};
        MONITORINFOEXA monitor{};
        monitor.cbSize = sizeof(monitor);
        if (!GetCursorPos(&point) || !GetWindowRect(window.window, &rect) ||
            !GetMonitorInfoA(MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST), &monitor))
        {
            return 13;
        }
        const LONG width = rect.right - rect.left;
        const LONG height = rect.bottom - rect.top;
        const auto left = std::clamp(point.x - width / 2, monitor.rcWork.left,
                                     (std::max)(monitor.rcWork.left, monitor.rcWork.right - width));
        const auto top = std::clamp(point.y - height / 2, monitor.rcWork.top,
                                    (std::max)(monitor.rcWork.top, monitor.rcWork.bottom - height));
        if (!SetWindowPos(window.window, nullptr, left, top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE) ||
            !GetWindowRect(window.window, &window.fixedRect))
        {
            return 13;
        }
        window.fixedPointer = point;
        std::printf("placement_fixed=1 monitor=%s pointer_x=%ld pointer_y=%ld left=%ld top=%ld\n",
                    monitor.szDevice, point.x, point.y, left, top);
    }
    cue::test::FocusExperiment focus;
    if (cyclesFocus && focusWidth == 960)
    {
        MONITORINFO monitor{sizeof(MONITORINFO)};
        if (!GetMonitorInfoW(MonitorFromWindow(window.window, MONITOR_DEFAULTTONEAREST), &monitor) ||
            !SetWindowPos(window.window, nullptr, monitor.rcWork.left, monitor.rcWork.top, 0, 0,
                          SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE))
        {
            return 13;
        }
    }
    if (cyclesFocus && !focus.start(window.window, focusWindow))
    {
        std::fprintf(stderr, "Focus companion could not be placed without overlap or started\n");
        return 13;
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
    IO_COUNTERS ioBefore{}, ioAfter{};
    if (!GetProcessIoCounters(GetCurrentProcess(), &ioBefore))
    {
        return 15;
    }
    recorder->started = std::chrono::duration_cast<std::chrono::nanoseconds>(start.time_since_epoch());
    recorder->trigger.armedAt = recorder->started.count() + 6000000000;
    recorder->isGpuRecording.store(true, std::memory_order_release);
    recorder->isRecording.store(true, std::memory_order_release);
    window.sample();
    if (cyclesFocus)
    {
        focus.sample(start, start);
    }
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
        if (cyclesFocus)
        {
            focus.sample(Clock::now(), start);
        }
    }
    const auto stopped = Clock::now();
    if (!GetProcessIoCounters(GetCurrentProcess(), &ioAfter))
    {
        return 15;
    }
    recorder->isRecording.store(false, std::memory_order_release);
    if (cyclesFocus && !focus.stop())
    {
        return 14;
    }
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
    const bool shutdownSucceeded = host.shutdown().has_value();
    recorder->isGpuRecording.store(false, std::memory_order_release);
    cue::set_timing_trace_sink(nullptr);
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
    if (emitsEtw && defersEtw)
    {
        // Worker 停止後に全区間を出す。Header の Thread と出力時刻を元区間の識別に使わない
        if (!outputAdapter.write_deferred())
        {
            return 9;
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
    const auto writeOperations = ioAfter.WriteOperationCount - ioBefore.WriteOperationCount;
    const auto writeBytes = ioAfter.WriteTransferCount - ioBefore.WriteTransferCount;
    std::printf("measurement_io write_operations=%llu write_bytes=%llu\n",
                static_cast<unsigned long long>(writeOperations), static_cast<unsigned long long>(writeBytes));
    std::printf("placement_checks pointer_changes=%zu window_moves=%zu\n", window.pointerChanges, window.windowMoves);
    const bool focusSaved = !cyclesFocus || focus.write(prefix);
    const bool traceSaved = !isTracing || cue::test::SpikeTraceExporter<Recorder>::write(*recorder, prefix);
    std::printf("recording_output guarded=1 early_file_write_attempts=%zu\n", recorder->earlyFileWriteAttempts.load());
    return shutdownSucceeded && focusSaved && traceSaved && recorder->earlyFileWriteAttempts.load() == 0 ? 0 : 8;
}
