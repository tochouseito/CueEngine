#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <thread>

#include <Windows.h>

#include <Foundation/PerformanceStudy.h>

namespace cue::dispatch_probe
{
enum class Kind
{
    Present,
    Empty,
    Sleep,
    Busy
};

struct Sample final
{
    Kind kind;
    std::uint64_t frame;
    std::int64_t start;
    std::int64_t end;
    std::int64_t hookNanoseconds;
    std::uint64_t cycles;
    std::uint64_t profileCycles;
    std::uint64_t waitReasons;
    DWORD switches;
    DWORD status;
    DWORD thread;
};

inline std::array<Sample, 4096> g_samples;
inline std::atomic<std::size_t> g_count = 0;
inline std::atomic<std::size_t> g_disableFailures = 0;

/// @brief 現在 Thread だけを観測し、同じ Thread の終了前に Profiling Handle を解放する
class ThreadProfile final
{
  public:
    /// @brief 管理者用 Hardware Counter を要求せず Dispatch 情報だけを有効化する
    ThreadProfile() noexcept
    {
        m_status = EnableThreadProfiling(GetCurrentThread(), THREAD_PROFILING_FLAG_DISPATCH, 0, &m_handle);
    }

    /// @brief Thread の寿命内に解除し、解除失敗は停止後の検証へ残す
    ~ThreadProfile()
    {
        release();
    }

    ThreadProfile(const ThreadProfile &) = delete;
    ThreadProfile &operator=(const ThreadProfile &) = delete;

    /// @brief Main 実行条件でも保存前に解除失敗を確認できるよう、二重解放を防いで解除する
    void release() noexcept
    {
        if (m_handle)
        {
            if (DisableThreadProfiling(m_handle) != ERROR_SUCCESS)
                g_disableFailures.fetch_add(1, std::memory_order_relaxed);
            m_handle = nullptr;
        }
    }

    /// @brief 直前までの WaitReason を消費し、同一 Thread の開始値を保持する
    void begin(Kind a_kind) noexcept
    {
        m_hookStart = study::now();
        m_kind = a_kind;
        m_beginStatus = read(m_before);
        m_cycles = 0;
        if (!QueryThreadCycleTime(GetCurrentThread(), &m_cycles) && m_beginStatus == ERROR_SUCCESS)
            m_beginStatus = GetLastError();
        m_start = study::now();
    }

    /// @brief Clock 区間の外で終了 Counter を読み、IO や集計なしで固定領域へ保存する
    void end() noexcept
    {
        const auto end = study::now();
        ULONG64 cycles = 0;
        DWORD status = m_beginStatus;
        if (!QueryThreadCycleTime(GetCurrentThread(), &cycles) && status == ERROR_SUCCESS)
            status = GetLastError();
        PERFORMANCE_DATA after{};
        const auto readStatus = read(after);
        if (status == ERROR_SUCCESS)
            status = readStatus;
        const auto index = g_count.fetch_add(1, std::memory_order_relaxed);
        if (index < g_samples.size())
        {
            g_samples[index] = {
                m_kind,
                study::g_frame,
                std::chrono::duration_cast<std::chrono::nanoseconds>(m_start.time_since_epoch()).count(),
                std::chrono::duration_cast<std::chrono::nanoseconds>(end.time_since_epoch()).count(),
                std::chrono::duration_cast<std::chrono::nanoseconds>(study::now() - m_hookStart).count(),
                cycles >= m_cycles ? cycles - m_cycles : 0,
                after.CycleTime >= m_before.CycleTime ? after.CycleTime - m_before.CycleTime : 0,
                after.WaitReasonBitMap,
                after.ContextSwitchCount - m_before.ContextSwitchCount,
                status,
                GetCurrentThreadId()};
        }
    }

  private:
    /// @brief Size と Version を毎回初期化し、失敗を有効な Zero 計測として扱わない
    [[nodiscard]] DWORD read(PERFORMANCE_DATA &a_data) noexcept
    {
        a_data = {};
        a_data.Size = sizeof(a_data);
        a_data.Version = PERFORMANCE_DATA_VERSION;
        return m_status == ERROR_SUCCESS
                   ? ReadThreadProfilingData(m_handle, READ_THREAD_PROFILING_FLAG_DISPATCHING, &a_data)
                   : m_status;
    }

    HANDLE m_handle = nullptr;
    DWORD m_status = ERROR_SUCCESS;
    DWORD m_beginStatus = ERROR_SUCCESS;
    Kind m_kind = Kind::Present;
    PERFORMANCE_DATA m_before{};
    ULONG64 m_cycles = 0;
    std::chrono::steady_clock::time_point m_start;
    std::chrono::steady_clock::time_point m_hookStart;
};

/// @brief 最初の Warmup Present で作成し、Render Worker の終了時に解除する
[[nodiscard]] inline ThreadProfile &current_profile() noexcept
{
    static thread_local ThreadProfile profile;
    return profile;
}

/// @brief Native Present の開始 Hook。Profiling の初期化は Warmup 内で完了する
inline void begin_present() noexcept
{
    current_profile().begin(Kind::Present);
}

/// @brief Native Present の終了 Hook。同じ Thread の開始 Hook と一対で使用する
inline void end_present() noexcept
{
    current_profile().end();
}

/// @brief Empty / Sleep / CPU Busy の既知区間で Counter の有効性と読取負荷を確認する
inline void calibrate() noexcept
{
    g_count.store(0);
    g_disableFailures.store(0);
    ThreadProfile profile;
    for (unsigned index = 0; index < 60; ++index)
    {
        profile.begin(Kind::Empty);
        profile.end();
        profile.begin(Kind::Sleep);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        profile.end();
        profile.begin(Kind::Busy);
        const auto until = study::now() + std::chrono::milliseconds(2);
        while (study::now() < until)
            std::atomic_signal_fence(std::memory_order_seq_cst);
        profile.end();
    }
}

/// @brief Worker Join 後にのみ Counter と Calibration を出力し、Warmup Present は除外する
[[nodiscard]] inline bool save(const std::filesystem::path &a_output, unsigned a_warmup, unsigned a_frames)
{
    auto path = a_output;
    path += ".dispatch.csv";
    std::ofstream csv(path, std::ios::binary);
    csv << "kind,frame,start_ns,end_ns,hook_ns,cycles,profile_cycles,context_switches,wait_reasons,status,thread\n";
    constexpr std::array names{"Present", "Empty", "Sleep", "Busy"};
    const auto count = (std::min)(g_count.load(), g_samples.size());
    for (std::size_t index = 0; index < count; ++index)
    {
        const auto &sample = g_samples[index];
        if (sample.kind == Kind::Present && (sample.frame < a_warmup || sample.frame >= a_warmup + a_frames))
            continue;
        csv << names[static_cast<std::size_t>(sample.kind)] << ',' << sample.frame << ',' << sample.start << ','
            << sample.end << ',' << sample.hookNanoseconds << ',' << sample.cycles << ',' << sample.profileCycles << ','
            << sample.switches << ',' << sample.waitReasons << ',' << sample.status << ',' << sample.thread << '\n';
    }
    csv.flush();
    return static_cast<bool>(csv) && g_count.load() <= g_samples.size() && g_disableFailures.load() == 0;
}
} // namespace cue::dispatch_probe
