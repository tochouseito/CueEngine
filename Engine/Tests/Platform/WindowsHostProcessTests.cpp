#include <Cue/WindowsHost/WindowsHost.h>

#include <chrono>
#include <cstdint>
#include <cwchar>
#include <stop_token>
#include <string>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace
{
struct WindowSearch final
{
    DWORD processId;
    HWND handle = nullptr;
};

/// @brief Test対象Processの表示中Main Windowを探す
BOOL CALLBACK find_window(HWND a_handle, LPARAM a_context)
{
    // 子 Process に属する表示中 Window だけを Test 対象にする
    auto* search = reinterpret_cast<WindowSearch*>(a_context);
    DWORD processId = 0;
    GetWindowThreadProcessId(a_handle, &processId);
    if (processId == search->processId && IsWindowVisible(a_handle))
    {
        search->handle = a_handle;
        return FALSE;
    }
    return TRUE;
}

/// @brief Window状態の反映をProcessをブロックせずに待つ
bool wait_for_window_state(HWND a_window, HANDLE a_process, bool a_isZoomed, bool a_isIconic,
                           bool a_checkZoom = true)
{
    for (int attempt = 0; attempt < 100; ++attempt)
    {
        if (WaitForSingleObject(a_process, 0) != WAIT_TIMEOUT || !IsWindow(a_window))
        {
            return false;
        }
        if (static_cast<bool>(IsIconic(a_window)) == a_isIconic &&
            (a_isIconic || !a_checkZoom || static_cast<bool>(IsZoomed(a_window)) == a_isZoomed))
        {
            return true;
        }
        Sleep(20);
    }
    return false;
}

/// @brief 製品HostへResizeと最小化・復帰を送りProcessの継続を確認する
int exercise_window_states(HWND a_window, HANDLE a_process)
{
    // 別Processへの同期Window操作はそのMessage Threadが止まるとTest自体も止まる
    RECT originalSize{};
    if (!GetClientRect(a_window, &originalSize) ||
        !PostMessageW(a_window, WM_SYSCOMMAND, SC_MAXIMIZE, 0) ||
        !wait_for_window_state(a_window, a_process, true, false))
    {
        return 6;
    }
    RECT maximizedSize{};
    if (!GetClientRect(a_window, &maximizedSize) ||
        (originalSize.right - originalSize.left == maximizedSize.right - maximizedSize.left &&
         originalSize.bottom - originalSize.top == maximizedSize.bottom - maximizedSize.top))
    {
        return 6;
    }
    if (!PostMessageW(a_window, WM_SYSCOMMAND, SC_MINIMIZE, 0) ||
        !wait_for_window_state(a_window, a_process, false, true))
    {
        return 7;
    }
    if (!PostMessageW(a_window, WM_SYSCOMMAND, SC_RESTORE, 0) ||
        !wait_for_window_state(a_window, a_process, false, false, false))
    {
        return 8;
    }
    // 最大化状態から最小化したWindowは一度のRestoreで最大化へ戻る場合がある
    if (IsZoomed(a_window) &&
        (!PostMessageW(a_window, WM_SYSCOMMAND, SC_RESTORE, 0) ||
         !wait_for_window_state(a_window, a_process, false, false)))
    {
        return 8;
    }
    RECT restoredSize{};
    if (!GetClientRect(a_window, &restoredSize) ||
        restoredSize.right - restoredSize.left != originalSize.right - originalSize.left ||
        restoredSize.bottom - restoredSize.top != originalSize.bottom - originalSize.top ||
        WaitForSingleObject(a_process, 200) != WAIT_TIMEOUT)
    {
        return 8;
    }
    return 0;
}

/// @brief Test専用の子ProcessでFrame完了またはRender失敗を発生させる
int run_child(bool a_useWorkerThreads, bool a_failRender)
{
    // Main と同じ Host を子 Process 内で動かし、終了Codeを親から観測する
    cue::WindowsHost host({{"CueWindowsHost Process Test", {320, 240}}, {2, a_useWorkerThreads, 0}});
    auto initResult = host.initialize(
        [](std::uint64_t, std::stop_token) { return cue::Result<void>::success(); },
        [a_failRender](std::uint64_t, std::stop_token) {
            if (a_failRender)
            {
                return cue::Result<void>::failure({cue::ErrorCategory::InvalidState, "Test.render.failure"});
            }
            return cue::Result<void>::success();
        });
    if (!initResult.has_value())
    {
        [[maybe_unused]] auto stopResult = host.shutdown();
        return 2;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline)
    {
        // 失敗Modeでは Error と Shutdown の双方が同じ原因を示すことを確認する
        auto stepResult = host.step();
        if (!stepResult.has_value())
        {
            const bool isExpectedFailure = a_failRender &&
                stepResult.try_error()->operation == "Test.render.failure";
            auto stopResult = host.shutdown();
            return isExpectedFailure && !stopResult.has_value() &&
                stopResult.try_error()->operation == "Test.render.failure" ? 1 : 3;
        }
        if (!*stepResult.try_value())
        {
            [[maybe_unused]] auto stopResult = host.shutdown();
            return 4;
        }
        if (a_failRender)
        {
            continue;
        }
        // 正常Modeでは指定数の Frame が完了してから終了する
        auto progressResult = host.progress();
        if (!progressResult.has_value())
        {
            [[maybe_unused]] auto stopResult = host.shutdown();
            return 5;
        }
        if (progressResult.try_value()->renderedFrames >= 4)
        {
            const bool areFramesComplete = progressResult.try_value()->updatedFrames >= 4;
            auto stopResult = host.shutdown();
            return areFramesComplete && stopResult.has_value() ? 0 : 6;
        }
    }
    [[maybe_unused]] auto stopResult = host.shutdown();
    return 7;
}
} // namespace

/// @brief 製品HostのCloseとTest専用子ProcessのFrame・失敗終了を確認する
int wmain(int a_argumentCount, wchar_t* a_arguments[])
{
    // 子 Process 用の引数は、再帰的に親の検証処理へ入る前に処理する
    if (a_argumentCount == 2)
    {
        if (std::wcscmp(a_arguments[1], L"--child-auto") == 0)
        {
            return run_child(true, false);
        }
        if (std::wcscmp(a_arguments[1], L"--child-auto-single") == 0)
        {
            return run_child(false, false);
        }
        if (std::wcscmp(a_arguments[1], L"--child-fail-render") == 0)
        {
            return run_child(true, true);
        }
    }
    if (a_argumentCount != 2 && a_argumentCount != 3)
    {
        return 1;
    }
    const bool isAutoMode = a_argumentCount == 3 &&
                            (std::wcscmp(a_arguments[2], L"--auto") == 0 ||
                             std::wcscmp(a_arguments[2], L"--auto-single") == 0);
    const bool isSingleThread = isAutoMode && std::wcscmp(a_arguments[2], L"--auto-single") == 0;
    const bool isFailureMode = a_argumentCount == 3 && std::wcscmp(a_arguments[2], L"--expect-render-failure") == 0;
    const bool isResizeMode = a_argumentCount == 3 && std::wcscmp(a_arguments[2], L"--resize") == 0;
    if (a_argumentCount == 3 && !isAutoMode && !isFailureMode && !isResizeMode)
    {
        return 1;
    }

    // Close Testは製品Executable、Frameと失敗TestはTest自身を子Processとして起動する
    std::wstring commandLine;
    if (isAutoMode || isFailureMode)
    {
        commandLine = L"\"" + std::wstring(a_arguments[1]) + L"\" ";
        commandLine += isFailureMode ? L"--child-fail-render" :
                       isSingleThread ? L"--child-auto-single" : L"--child-auto";
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    // 子 Process を親側から監視し、起動できない場合は即座に失敗する
    if (!CreateProcessW(a_arguments[1], (isAutoMode || isFailureMode) ? commandLine.data() : nullptr,
                        nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process))
    {
        return 2;
    }

    int result = 0;
    if (isAutoMode || isFailureMode)
    {
        // 自動終了またはCallback失敗を子Process自身の終了Codeで確認する
        if (WaitForSingleObject(process.hProcess, 5000) != WAIT_OBJECT_0)
        {
            result = 4;
        }
        else
        {
            DWORD exitCode = 0;
            const DWORD expectedExitCode = isFailureMode ? 1 : 0;
            if (!GetExitCodeProcess(process.hProcess, &exitCode) || exitCode != expectedExitCode)
            {
                result = 5;
            }
        }
    }
    else
    {
        // Window表示まで最大5秒だけ待つ
        WindowSearch search{process.dwProcessId};
        for (int attempt = 0; attempt < 200 && !search.handle; ++attempt)
        {
            EnumWindows(&find_window, reinterpret_cast<LPARAM>(&search));
            if (!search.handle)
            {
                Sleep(25);
            }
        }

        if (!search.handle)
        {
            result = 3;
        }
        if (result == 0 && isResizeMode)
        {
            result = exercise_window_states(search.handle, process.hProcess);
        }

        // Resizeや復帰後もTitle BarのCloseと同じMessageで正常終了する
        if (result == 0 && !PostMessageW(search.handle, WM_CLOSE, 0, 0))
        {
            result = 3;
        }
        else if (result == 0 && WaitForSingleObject(process.hProcess, 5000) != WAIT_OBJECT_0)
        {
            result = 4;
        }
        else if (result == 0)
        {
            DWORD exitCode = 0;
            if (!GetExitCodeProcess(process.hProcess, &exitCode) || exitCode != 0)
            {
                result = 5;
            }
        }
    }

    // 失敗時は子Processを残さず、Native Handleも閉じる
    if (result != 0)
    {
        TerminateProcess(process.hProcess, static_cast<UINT>(result));
        WaitForSingleObject(process.hProcess, 5000);
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return result;
}
