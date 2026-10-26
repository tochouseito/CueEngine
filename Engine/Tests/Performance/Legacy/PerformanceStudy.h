#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <span>
#include <thread>

// Issue #101 の調査 Branch 専用。通常 Build では記録と時計取得を除去する
// configure は Worker 起動前、samples は全 Worker の Join 後だけ呼ぶ
// 実行中は予約済み領域へ書くだけとし、File IO と統計集計を測定区間に入れない
namespace cue::study
{
enum class Metric
{
    Render,
    Main,
    Update,
    FpsWait,
    Present,
    GraphPrepare,
    GraphRecord,
    CommandAcquire,
    CommandClose,
    CommandSubmit,
    GraphMark,
    TimestampFrequency,
    ImGuiEnterWait,
    ImGuiResumeWait,
    ImGuiRecordWait,
    ImGuiRecord,
    ImGuiFinish,
    UiBuild,
    SnapshotCopy,
    Readout,
    GpuReadback,
    PoolAcquireLockWait,
    PoolScan,
    AllocatorReset,
    ListReset,
    TokenCreate,
    QueueValidation,
    QueueLockWait,
    ExecuteLists,
    Signal,
    Preformat,
    MainGateWait,
    PresentNative,
    CycleReadOverhead,
    NativePreparation,
    NativePreparationCached,
    PresentationWait,
    ImGuiDescriptorValidate,
    ImGuiDescriptorMark,
    ImGuiSnapshotRecord,
    MainPump,
    PresentGpuWait
};

struct Options final
{
    unsigned readoutStride = 1;
    bool gpuTimestamps = true;
    bool pixMarkers = true;
    // 比較条件は起動前に固定し、通常 Build ではすべて既存経路を選ぶ
    bool statisticsOutsideLock = false;
    bool cacheTimestampFrequency = false;
    bool useNthStatistics = false;
    unsigned gpuStatisticsStride = 1;
    bool preformatUiText = false;
    bool waitBeforeUi = false;
    bool rotateCommandSlots = false;
    bool nativeCycles = false;
    bool cacheGraphPreparation = false;
    bool separateImGuiRecording = false;
    bool waitableSwapChain = false;
    unsigned maximumPresentationLatency = 2;
    bool noPresentationScaling = false;
    bool borderlessWindow = false;
    bool descriptorLookup = false;
    bool presentDispatch = false;
    bool presentGpuWait = false;
};

struct Sample final
{
    std::uint64_t frame;
    Metric metric;
    std::int64_t nanoseconds;
    std::size_t thread;
};

struct TimelineSample final
{
    std::uint64_t frame;
    Metric metric;
    std::int64_t start;
    std::int64_t end;
    std::size_t thread;
};

struct GpuReadinessSample final
{
    std::uint64_t frame;
    std::uint64_t fence;
    bool wasReady;
    bool isReady;
};

struct AdapterInfo final
{
    std::uint32_t vendorId = 0;
    std::uint32_t deviceId = 0;
    std::uint64_t dedicatedVideoMemory = 0;
};

/// @brief 生成後の実 Window 条件を所有値で保存する
struct WindowInfo final
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t refreshHz = 0;
    bool isBorderless = false;
    bool isPrimaryMonitor = false;
    std::int32_t outerX = 0;
    std::int32_t outerY = 0;
    std::uint32_t outerWidth = 0;
    std::uint32_t outerHeight = 0;
    std::uint32_t monitorWidth = 0;
    std::uint32_t monitorHeight = 0;
};

struct CycleSample final
{
    std::uint64_t frame;
    Metric metric;
    std::uint64_t cycles;
    std::size_t thread;
};

/// @brief 起動時に DXGI から照会した実 SwapChain 条件を保存する
struct PresentationInfo final
{
    unsigned buffers = 0;
    unsigned maximumLatency = 0;
    unsigned scaling = 0;
    unsigned flags = 0;
    unsigned width = 0;
    unsigned height = 0;
};

using cycleReader = std::uint64_t (*)() noexcept;
using dispatchHook = void (*)() noexcept;
inline constexpr auto k_noFrame = (std::numeric_limits<std::uint64_t>::max)();
#if defined(CUE_PERFORMANCE_STUDY)
inline std::array<Sample, 65536> g_samples;
inline std::atomic<std::size_t> g_count = 0;
inline std::array<TimelineSample, 65536> g_timeline;
inline std::atomic<std::size_t> g_timelineCount = 0;
inline std::array<GpuReadinessSample, 4096> g_gpuReadiness;
inline std::atomic<std::size_t> g_gpuReadinessCount = 0;
inline Options g_options;
inline AdapterInfo g_adapter;
inline WindowInfo g_window;
inline PresentationInfo g_presentation;
inline bool g_tearingEnabled = false;
inline thread_local std::uint64_t g_frame = k_noFrame;
inline std::array<CycleSample, 65536> g_cycleSamples;
inline std::atomic<std::size_t> g_cycleCount = 0;
inline std::atomic<std::size_t> g_cycleFailures = 0;
inline cycleReader g_cycleReader = nullptr;
inline dispatchHook g_dispatchBegin = nullptr;
inline dispatchHook g_dispatchEnd = nullptr;
inline std::array<std::atomic<std::uint64_t>, 32> g_commandSlots{};
#endif

/// @brief 起動前に固定領域を初期化し、実行中不変の比較条件を設定する
inline void configure(Options a_options) noexcept
{
#if defined(CUE_PERFORMANCE_STUDY)
    g_count.store(0);
    g_timelineCount.store(0);
    g_gpuReadinessCount.store(0);
    g_options = a_options;
    g_options.readoutStride = (std::max)(1u, a_options.readoutStride);
    g_adapter = {};
    g_window = {};
    g_presentation = {};
    g_tearingEnabled = false;
    g_cycleCount.store(0);
    g_cycleFailures.store(0);
    g_cycleReader = nullptr;
    g_dispatchBegin = nullptr;
    g_dispatchEnd = nullptr;
    for (auto &count : g_commandSlots)
        count.store(0);
#endif
}

/// @brief 起動前に固定した条件を返し、通常 Build では既存動作を維持する
[[nodiscard]] inline Options options() noexcept
{
#if defined(CUE_PERFORMANCE_STUDY)
    return g_options;
#else
    return {};
#endif
}

/// @brief 実際に採用した Device の識別情報を起動 Thread から記録する
inline void set_adapter(AdapterInfo a_adapter) noexcept
{
#if defined(CUE_PERFORMANCE_STUDY)
    g_adapter = a_adapter;
#endif
}

/// @brief 起動後に確定した Adapter 情報を停止後の出力へ渡す
[[nodiscard]] inline AdapterInfo adapter() noexcept
{
#if defined(CUE_PERFORMANCE_STUDY)
    return g_adapter;
#else
    return {};
#endif
}

/// @brief Owner が初期化中に確定した実際の Window 寸法と Monitor 条件を保存する
inline void set_window(WindowInfo a_info) noexcept
{
#if defined(CUE_PERFORMANCE_STUDY)
    g_window = a_info;
#endif
}

/// @brief Worker 停止後に、起動時に確定した Window 条件を返す
[[nodiscard]] inline WindowInfo window() noexcept
{
#if defined(CUE_PERFORMANCE_STUDY)
    return g_window;
#else
    return {};
#endif
}

/// @brief SwapChain が対応判定後に採用した実際の Tearing 条件を保存する
inline void set_tearing(bool a_enabled) noexcept
{
#if defined(CUE_PERFORMANCE_STUDY)
    g_tearingEnabled = a_enabled;
#endif
}

/// @brief 設定要求値と別に、実際の Present 条件を返す
[[nodiscard]] inline bool tearing_enabled() noexcept
{
#if defined(CUE_PERFORMANCE_STUDY)
    return g_tearingEnabled;
#else
    return false;
#endif
}

/// @brief Worker 起動前に DXGI から取得した生成条件を記録する
inline void set_presentation(PresentationInfo a_info) noexcept
{
#if defined(CUE_PERFORMANCE_STUDY)
    g_presentation = a_info;
#endif
}

/// @brief 停止後の保存処理へ起動時の実 SwapChain 条件を返す
[[nodiscard]] inline PresentationInfo presentation() noexcept
{
#if defined(CUE_PERFORMANCE_STUDY)
    return g_presentation;
#else
    return {};
#endif
}

/// @brief 調査 Build だけ単調時計を取得する
[[nodiscard]] inline std::chrono::steady_clock::time_point now() noexcept
{
#if defined(CUE_PERFORMANCE_STUDY)
    return std::chrono::steady_clock::now();
#else
    return {};
#endif
}

/// @brief 測定済み時間を重複しない領域へ保存し、実行中は保存済み領域を読まない
template <class Rep, class Period>
inline void record(Metric a_metric, std::chrono::duration<Rep, Period> a_duration,
                   std::uint64_t a_frame = k_noFrame) noexcept
{
#if defined(CUE_PERFORMANCE_STUDY)
    const auto frame = a_frame == k_noFrame ? g_frame : a_frame;
    if (g_options.presentDispatch)
    {
        // 追加の時計取得は調査時だけ行う。記録位置を終了時刻とする近似区間
        const auto end = std::chrono::duration_cast<std::chrono::nanoseconds>(now().time_since_epoch()).count();
        const auto index = g_timelineCount.fetch_add(1, std::memory_order_relaxed);
        if (index < g_timeline.size())
        {
            static thread_local const auto thread = std::hash<std::thread::id>{}(std::this_thread::get_id());
            g_timeline[index] = {frame, a_metric,
                                 end - std::chrono::duration_cast<std::chrono::nanoseconds>(a_duration).count(),
                                 end, thread};
        }
    }
    if (frame == k_noFrame)
    {
        return;
    }
    const auto index = g_count.fetch_add(1, std::memory_order_relaxed);
    if (index < g_samples.size())
    {
        // Thread ID の Hash は各 Thread の初回だけ計算する
        static thread_local const auto thread = std::hash<std::thread::id>{}(std::this_thread::get_id());
        g_samples[index] = {frame, a_metric, std::chrono::duration_cast<std::chrono::nanoseconds>(a_duration).count(),
                            thread};
    }
#endif
}

/// @brief 全 Worker 停止後だけ借用可能な記録を返す
[[nodiscard]] inline std::span<const Sample> samples() noexcept
{
#if defined(CUE_PERFORMANCE_STUDY)
    return {g_samples.data(), (std::min)(g_count.load(), g_samples.size())};
#else
    return {};
#endif
}

/// @brief 固定容量を超えて破棄した記録数を停止後に返す
[[nodiscard]] inline std::size_t dropped_samples() noexcept
{
#if defined(CUE_PERFORMANCE_STUDY)
    const auto count = g_count.load();
    return count > g_samples.size() ? count - g_samples.size() : 0;
#else
    return 0;
#endif
}

/// @brief Worker Join 後だけ同一時計の区間を借用し、Frame 外の Message Pump も返す
[[nodiscard]] inline std::span<const TimelineSample> timeline_samples() noexcept
{
#if defined(CUE_PERFORMANCE_STUDY)
    return {g_timeline.data(), (std::min)(g_timelineCount.load(), g_timeline.size())};
#else
    return {};
#endif
}

/// @brief Timeline の容量超過を通常の計測失敗と区別して停止後に返す
[[nodiscard]] inline std::size_t dropped_timeline_samples() noexcept
{
#if defined(CUE_PERFORMANCE_STUDY)
    const auto count = g_timelineCount.load();
    return count > g_timeline.size() ? count - g_timeline.size() : 0;
#else
    return 0;
#endif
}

/// @brief Present 前の Graphics Fence の状態だけを保持する。固定領域は停止後に読む
inline void record_gpu_readiness(std::uint64_t a_fence, bool a_wasReady, bool a_isReady) noexcept
{
#if defined(CUE_PERFORMANCE_STUDY)
    const auto index = g_gpuReadinessCount.fetch_add(1, std::memory_order_relaxed);
    if (index < g_gpuReadiness.size())
        g_gpuReadiness[index] = {g_frame, a_fence, a_wasReady, a_isReady};
#endif
}

/// @brief Worker Join 後だけ Graphics Fence の観測値を借用する
[[nodiscard]] inline std::span<const GpuReadinessSample> gpu_readiness_samples() noexcept
{
#if defined(CUE_PERFORMANCE_STUDY)
    return {g_gpuReadiness.data(), (std::min)(g_gpuReadinessCount.load(), g_gpuReadiness.size())};
#else
    return {};
#endif
}

/// @brief Platform 固有の Reader を Worker 起動前に固定する。終了まで関数の寿命を保つ
inline void set_cycle_reader(cycleReader a_reader) noexcept
{
#if defined(CUE_PERFORMANCE_STUDY)
    g_cycleReader = a_reader;
#endif
}

/// @brief 調査用 Platform Hook を Worker 起動前に登録し、停止まで寿命を保持する
inline void set_dispatch_hooks(dispatchHook a_begin, dispatchHook a_end) noexcept
{
#if defined(CUE_PERFORMANCE_STUDY)
    g_dispatchBegin = a_begin;
    g_dispatchEnd = a_end;
#endif
}

/// @brief Native Present の外側だけで Thread 計測を開始する
inline void begin_present_dispatch() noexcept
{
#if defined(CUE_PERFORMANCE_STUDY)
    if (g_options.presentDispatch && g_dispatchBegin)
        g_dispatchBegin();
#endif
}

/// @brief Native Present の時間計測を終えてから Thread 計測を保存する
inline void end_present_dispatch() noexcept
{
#if defined(CUE_PERFORMANCE_STUDY)
    if (g_options.presentDispatch && g_dispatchEnd)
        g_dispatchEnd();
#endif
}

/// @brief 別 Phase だけ現在 Thread の CPU Cycle を読む。失敗値は記録を破棄する
[[nodiscard]] inline std::uint64_t cycle_count() noexcept
{
#if defined(CUE_PERFORMANCE_STUDY)
    if (g_options.nativeCycles && g_cycleReader && g_frame != k_noFrame)
    {
        const auto cycles = g_cycleReader();
        if (!cycles)
            g_cycleFailures.fetch_add(1, std::memory_order_relaxed);
        return cycles;
    }
#endif
    return 0;
}

/// @brief CPU Cycle は時間へ換算せず、同じ Thread の API 区間として別領域へ保存する
inline void record_cycles(Metric a_metric, std::uint64_t a_begin, std::uint64_t a_end) noexcept
{
#if defined(CUE_PERFORMANCE_STUDY)
    if (!a_begin)
        return;
    if (!a_end)
        return;
    if (a_end < a_begin)
    {
        g_cycleFailures.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const auto index = g_cycleCount.fetch_add(1, std::memory_order_relaxed);
    if (index < g_cycleSamples.size())
    {
        static thread_local const auto thread = std::hash<std::thread::id>{}(std::this_thread::get_id());
        g_cycleSamples[index] = {g_frame, a_metric, a_end - a_begin, thread};
    }
#endif
}

/// @brief 空の読取区間など、保存前に終了値を取得できる Scope の Cycle を保存する
inline void record_cycles(Metric a_metric, std::uint64_t a_begin) noexcept
{
    if (a_begin)
        record_cycles(a_metric, a_begin, cycle_count());
}

/// @brief Worker Join 後に CPU Cycle の記録を借用する
[[nodiscard]] inline std::span<const CycleSample> cycle_samples() noexcept
{
#if defined(CUE_PERFORMANCE_STUDY)
    return {g_cycleSamples.data(), (std::min)(g_cycleCount.load(), g_cycleSamples.size())};
#else
    return {};
#endif
}

/// @brief Cycle 取得と保存の失敗数を停止後に確認する
[[nodiscard]] inline std::size_t cycle_failures() noexcept
{
#if defined(CUE_PERFORMANCE_STUDY)
    const auto count = g_cycleCount.load();
    return g_cycleFailures.load() + (count > g_cycleSamples.size() ? count - g_cycleSamples.size() : 0);
#else
    return 0;
#endif
}

/// @brief Graphics Slot の実際の貸出回数を固定領域へ保存する
inline void record_command_slot(std::size_t a_slot) noexcept
{
#if defined(CUE_PERFORMANCE_STUDY)
    if (a_slot < g_commandSlots.size())
        g_commandSlots[a_slot].fetch_add(1, std::memory_order_relaxed);
#endif
}

/// @brief 停止後に各 Slot の貸出回数を返す。Warmup を含む
[[nodiscard]] inline std::array<std::uint64_t, 32> command_slot_counts() noexcept
{
    std::array<std::uint64_t, 32> result{};
#if defined(CUE_PERFORMANCE_STUDY)
    for (std::size_t index = 0; index < result.size(); ++index)
        result[index] = g_commandSlots[index].load();
#endif
    return result;
}

/// @brief CSV 上で区間を識別する固定名を返す
[[nodiscard]] inline const char *metric_name(Metric a_metric) noexcept
{
    constexpr std::array names{"Render",
                               "Main",
                               "Update",
                               "FpsWait",
                               "Present",
                               "GraphPrepare",
                               "GraphRecord",
                               "CommandAcquire",
                               "CommandClose",
                               "CommandSubmit",
                               "GraphMark",
                               "TimestampFrequency",
                               "ImGuiEnterWait",
                               "ImGuiResumeWait",
                               "ImGuiRecordWait",
                               "ImGuiRecord",
                               "ImGuiFinish",
                               "UiBuild",
                               "SnapshotCopy",
                               "Readout",
                               "GpuReadback",
                               "PoolAcquireLockWait",
                               "PoolScan",
                               "AllocatorReset",
                               "ListReset",
                               "TokenCreate",
                               "QueueValidation",
                               "QueueLockWait",
                               "ExecuteLists",
                               "Signal",
                               "Preformat",
                               "MainGateWait",
                               "PresentNative",
                               "CycleReadOverhead",
                               "NativePreparation",
                               "NativePreparationCached",
                               "PresentationWait",
                               "ImGuiDescriptorValidate",
                               "ImGuiDescriptorMark",
                               "ImGuiSnapshotRecord",
                               "MainPump",
                               "PresentGpuWait"};
    return names[static_cast<std::size_t>(a_metric)];
}

/// @brief 同じ Thread の入れ子区間へ絶対 Frame ID を引き継ぐ
class FrameScope final
{
  public:
    /// @brief Thread の直前 ID を保存し、この Callback の ID に切り替える
    explicit FrameScope(std::uint64_t a_frame) noexcept
    {
#if defined(CUE_PERFORMANCE_STUDY)
        m_previous = g_frame;
        g_frame = a_frame;
#endif
    }
    /// @brief 入れ子の呼出元 ID を復元する
    ~FrameScope()
    {
#if defined(CUE_PERFORMANCE_STUDY)
        g_frame = m_previous;
#endif
    }
    FrameScope(const FrameScope &) = delete;
    FrameScope &operator=(const FrameScope &) = delete;

  private:
    std::uint64_t m_previous = k_noFrame;
};

/// @brief 早期 Return を含めた関数区間を記録する。入れ子の時間は含まれる
class Scope final
{
  public:
    /// @brief Scope の開始を固定する
    explicit Scope(Metric a_metric) noexcept : m_metric(a_metric), m_start(now())
    {
    }
    /// @brief 終了時に時間だけを保存する
    ~Scope()
    {
        record(m_metric, now() - m_start);
    }
    Scope(const Scope &) = delete;
    Scope &operator=(const Scope &) = delete;

  private:
    Metric m_metric;
    std::chrono::steady_clock::time_point m_start;
};
} // namespace cue::study
