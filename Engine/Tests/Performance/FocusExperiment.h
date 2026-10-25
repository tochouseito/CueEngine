#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

#include <Windows.h>

#include <Profiler/TimingTrace.h>

namespace cue::test
{
inline constexpr UINT k_focusRequest = WM_APP + 37;

struct FocusWindows final
{
    HWND parent;
    HWND external;
    DWORD externalPid;
};

/// @brief 計測 Window と指定した比較 Window の間だけで、OS が許可する前面切替を受け付ける
inline LRESULT CALLBACK focus_window_proc(HWND a_window, UINT a_message, WPARAM a_wParam, LPARAM a_lParam)
{
    if (a_message == WM_NCCREATE)
    {
        const auto *creation = reinterpret_cast<const CREATESTRUCTW *>(a_lParam);
        SetWindowLongPtrW(a_window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(creation->lpCreateParams));
    }
    const auto *windows = reinterpret_cast<const FocusWindows *>(GetWindowLongPtrW(a_window, GWLP_USERDATA));
    if (a_message == k_focusRequest)
    {
        const auto foreground = GetForegroundWindow();
        const auto target = windows->external ? windows->external : a_window;
        DWORD targetPid = 0;
        if (windows->external && (!GetWindowThreadProcessId(target, &targetPid) || targetPid != windows->externalPid))
        {
            return 0;
        }
        if (foreground != windows->parent && foreground != target)
        {
            return 0;
        }
        return SetForegroundWindow(a_wParam ? windows->parent : target);
    }
    if (a_message == WM_TIMER && windows && !IsWindow(windows->parent))
    {
        DestroyWindow(a_window);
        return 0;
    }
    if (a_message == WM_DESTROY)
    {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(a_window, a_message, a_wParam, a_lParam);
}

/// @brief 計測 Process が起動する静的な補助 Window を所有し、親の消失時も終了する
inline int run_focus_companion(HWND a_parent, int a_x, int a_y, HWND a_external)
{
    FocusWindows windows{a_parent, a_external, 0};
    if (a_external && !GetWindowThreadProcessId(a_external, &windows.externalPid))
    {
        return 23;
    }
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = focus_window_proc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    windowClass.lpszClassName = L"CueSpikeFocusCompanion";
    if (!IsWindow(a_parent) || !RegisterClassW(&windowClass))
    {
        return 20;
    }
    const auto window = CreateWindowExW(WS_EX_TOOLWINDOW, windowClass.lpszClassName, L"CueEngine Focus Companion",
                                        WS_OVERLAPPED | WS_CAPTION, a_x, a_y, 120, 60, nullptr, nullptr,
                                        windowClass.hInstance, &windows);
    if (!window)
    {
        return 21;
    }
    // 外部 Window を比較する時は仲介 Window を描画条件へ加えない
    if (!a_external)
    {
        ShowWindow(window, SW_SHOWNOACTIVATE);
    }
    SetTimer(window, 1, 1000, nullptr);
    MSG message{};
    int result = 0;
    while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0)
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return result < 0 ? 22 : 0;
}

/// @brief 20 秒の F/B/F/B/F 比較を制御し、要求と実際の前面 PID を固定容量で別々に保存する
class FocusExperiment final
{
  public:
    using Clock = std::chrono::steady_clock;

    /// @brief 終了時は所有する補助 Process にだけ Close を送り、強制終了しない
    ~FocusExperiment()
    {
        stop();
    }

    /// @brief 計測 Window を遮蔽しない Monitor 内の空間に補助 Window を作る。配置不能なら失敗する
    bool start(HWND a_window, HWND a_external = nullptr)
    {
        m_window = a_window;
        RECT target{};
        MONITORINFO monitor{sizeof(MONITORINFO)};
        if (!GetWindowRect(a_window, &target) ||
            !GetMonitorInfoW(MonitorFromWindow(a_window, MONITOR_DEFAULTTONEAREST), &monitor))
        {
            return false;
        }
        const auto &work = monitor.rcWork;
        std::printf("focus_placement target=%ld,%ld,%ld,%ld work=%ld,%ld,%ld,%ld\n", target.left, target.top,
                    target.right, target.bottom, work.left, work.top, work.right, work.bottom);
        if (a_external)
        {
            RECT external{}, intersection{};
            if (!IsWindowVisible(a_external) || IsIconic(a_external) || !GetWindowRect(a_external, &external) ||
                !GetWindowThreadProcessId(a_external, &m_targetPid) || m_targetPid == GetCurrentProcessId() ||
                IntersectRect(&intersection, &target, &external))
            {
                return false;
            }
            m_target = a_external;
            std::printf("focus_external pid=%lu hwnd=%llu rect=%ld,%ld,%ld,%ld\n", m_targetPid,
                        static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(a_external)), external.left,
                        external.top, external.right, external.bottom);
        }
        int x = target.right + 8, y = work.top;
        if (x + 120 > work.right)
        {
            x = work.left;
            y = target.bottom + 8;
            if (y + 60 > work.bottom)
            {
                x = target.left - 128;
                y = work.top;
                if (x < work.left)
                {
                    return false;
                }
            }
        }
        wchar_t executable[32768]{};
        if (!GetModuleFileNameW(nullptr, executable, 32768))
        {
            return false;
        }
        auto command = L"\"" + std::wstring(executable) + L"\" --focus-companion " +
                       std::to_wstring(reinterpret_cast<std::uintptr_t>(a_window)) + L" " + std::to_wstring(x) + L" " +
                       std::to_wstring(y) + L" " + std::to_wstring(reinterpret_cast<std::uintptr_t>(a_external));
        STARTUPINFOW startup{sizeof(STARTUPINFOW)};
        if (!CreateProcessW(executable, command.data(), nullptr, nullptr, false, CREATE_NO_WINDOW, nullptr, nullptr,
                            &startup, &m_process))
        {
            std::fprintf(stderr, "focus_create_process error=%lu\n", GetLastError());
            return false;
        }
        CloseHandle(m_process.hThread);
        m_process.hThread = nullptr;
        const auto deadline = Clock::now() + std::chrono::seconds(5);
        while (!m_companion && Clock::now() < deadline && WaitForSingleObject(m_process.hProcess, 0) == WAIT_TIMEOUT)
        {
            EnumWindows(find_companion, reinterpret_cast<LPARAM>(this));
            Sleep(10);
        }
        DWORD exitCode = 0;
        GetExitCodeProcess(m_process.hProcess, &exitCode);
        std::printf("focus_companion pid=%lu hwnd=%llu exit=%lu\n", m_process.dwProcessId,
                    static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(m_companion)), exitCode);
        if (!a_external)
        {
            m_target = m_companion;
            m_targetPid = m_process.dwProcessId;
        }
        return m_companion != nullptr;
    }

    /// @brief Step 境界で要求を一度送り、他アプリによる前面変更は奪い返さず異常として残す
    void sample(Clock::time_point a_now, Clock::time_point a_started)
    {
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(a_now - a_started).count();
        const int phase = (std::min)(4, static_cast<int>(ns / 4000000000));
        if (phase != m_phase)
        {
            m_phase = phase;
            DWORD_PTR result = 1;
            bool wasSent = true;
            if (phase != 0)
            {
                TimingTraceScope trace("Probe.FocusRequest", phase);
                if (GetForegroundWindow() == m_window)
                {
                    // OS の許可を現在の前面 Process から子へ渡す。拒否時は強制しない
                    AllowSetForegroundWindow(m_process.dwProcessId);
                }
                wasSent = SendMessageTimeoutW(m_companion, k_focusRequest, phase % 2 == 0, 0,
                                              SMTO_ABORTIFHUNG | SMTO_BLOCK, 100, &result) != 0;
            }
            m_failedRequests += !wasSent || result == 0;
            append(ns, "request", wasSent && result != 0);
        }
        const auto foreground = GetForegroundWindow();
        if (foreground != m_previous || m_count == 1)
        {
            m_previous = foreground;
            append(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - a_started).count(), "observed",
                   true);
        }
        const auto expected = phase % 2 == 0 ? m_window : m_target;
        ++m_checks;
        m_mismatches += foreground != expected;
    }

    /// @brief 同じ PID の補助 Window だけを閉じ、停止結果を検証可能にする
    bool stop()
    {
        if (!m_process.hProcess)
        {
            return m_stopped;
        }
        if (m_companion)
        {
            PostMessageW(m_companion, WM_CLOSE, 0, 0);
        }
        m_stopped = WaitForSingleObject(m_process.hProcess, 2000) == WAIT_OBJECT_0;
        CloseHandle(m_process.hProcess);
        m_process.hProcess = nullptr;
        return m_stopped;
    }

    /// @brief 全観測変化を終了後に保存し、位相間の切替失敗と容量不足を成功扱いしない
    bool write(const std::string &a_prefix)
    {
        std::FILE *file = nullptr;
        if (fopen_s(&file, (a_prefix + "-focus.csv").c_str(), "w") != 0)
        {
            return false;
        }
        std::fprintf(file, "time_ns,kind,phase,expected_pid,foreground_pid,foreground_tid,foreground_hwnd,success\n");
        for (std::size_t index = 0; index < m_count; ++index)
        {
            const auto &row = m_rows[index];
            std::fprintf(file, "%lld,%s,%d,%lu,%lu,%lu,%llu,%d\n", row.ns, row.kind, row.phase,
                         row.phase % 2 == 0 ? GetCurrentProcessId() : m_targetPid, row.pid, row.tid,
                         static_cast<unsigned long long>(row.window), row.succeeded);
        }
        const bool hadError = std::ferror(file) != 0;
        const bool closed = std::fclose(file) == 0;
        std::printf("focus_cycle companion_pid=%lu checks=%zu mismatches=%zu changes=%zu dropped=%zu stopped=%d "
                    "failed_requests=%zu\n",
                    m_process.dwProcessId, m_checks, m_mismatches, m_count, m_dropped, m_stopped, m_failedRequests);
        return closed && !hadError && m_dropped == 0 && m_phase == 4 && m_stopped && m_failedRequests == 0;
    }

  private:
    struct Row final
    {
        long long ns;
        const char *kind;
        int phase;
        DWORD pid;
        DWORD tid;
        std::uintptr_t window;
        bool succeeded;
    };

    /// @brief 起動した子 Process と Class の両方に一致する Window だけを借用する
    static BOOL CALLBACK find_companion(HWND a_window, LPARAM a_context)
    {
        auto &owner = *reinterpret_cast<FocusExperiment *>(a_context);
        DWORD pid = 0;
        GetWindowThreadProcessId(a_window, &pid);
        wchar_t name[64]{};
        if (pid == owner.m_process.dwProcessId && GetClassNameW(a_window, name, 64) &&
            std::wstring_view(name) == L"CueSpikeFocusCompanion")
        {
            owner.m_companion = a_window;
            return false;
        }
        return true;
    }

    /// @brief 前面が同じなら PID 問合せを繰り返さず、変化時だけ全件記録する
    void append(long long a_ns, const char *a_kind, bool a_succeeded)
    {
        const auto foreground = GetForegroundWindow();
        DWORD pid = 0;
        const auto tid = GetWindowThreadProcessId(foreground, &pid);
        if (m_count == m_rows.size())
        {
            ++m_dropped;
            return;
        }
        m_rows[m_count++] = {a_ns,       a_kind, m_phase, pid, tid, reinterpret_cast<std::uintptr_t>(foreground),
                             a_succeeded};
    }

    HWND m_window = nullptr;
    HWND m_companion = nullptr;
    // 外部比較 Window は借用し、stop() でも Close や Process 終了を要求しない
    HWND m_target = nullptr;
    DWORD m_targetPid = 0;
    HWND m_previous = nullptr;
    PROCESS_INFORMATION m_process{};
    std::array<Row, 128> m_rows{};
    std::size_t m_count = 0, m_dropped = 0, m_checks = 0, m_mismatches = 0;
    std::size_t m_failedRequests = 0;
    int m_phase = -1;
    bool m_stopped = true;
};
} // namespace cue::test
