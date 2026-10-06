
#include <cstdio>
#include <cwchar>

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

} // namespace

/// @brief 製品 Host の Window 表示、状態変更、Close を確認する
int wmain(int a_argumentCount, wchar_t* a_arguments[])
{
    if (a_argumentCount != 2 && a_argumentCount != 3)
    {
        return 1;
    }
    const bool isResizeMode = a_argumentCount == 3 && std::wcscmp(a_arguments[2], L"--resize") == 0;
    const bool isSustainMode = a_argumentCount == 3 && std::wcscmp(a_arguments[2], L"--sustain") == 0;
    if (a_argumentCount == 3 && !isResizeMode && !isSustainMode)
    {
        return 1;
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(a_arguments[1], nullptr, nullptr, nullptr, FALSE, 0,
                        nullptr, nullptr, &startup, &process))
    {
        return 2;
    }

    // 製品 Process の表示 Window を期限付きで待つ
    WindowSearch search{process.dwProcessId};
    for (int attempt = 0; attempt < 200 && !search.handle; ++attempt)
    {
        EnumWindows(&find_window, reinterpret_cast<LPARAM>(&search));
        if (!search.handle)
        {
            Sleep(25);
        }
    }

    int result = search.handle ? 0 : 3;
    // ImGui の約 5 秒の自動保存と Worker の連続進行も製品 Process 上で通過させる
    if (result == 0 && isSustainMode && WaitForSingleObject(process.hProcess, 6000) != WAIT_TIMEOUT)
    {
        DWORD exitCode = 0;
        GetExitCodeProcess(process.hProcess, &exitCode);
        std::fprintf(stderr, "Host exited before sustain interval: %lu\n", exitCode);
        result = 9;
    }
    if (result == 0 && isResizeMode)
    {
        result = exercise_window_states(search.handle, process.hProcess);
    }
    if (result == 0 && !PostMessageW(search.handle, WM_CLOSE, 0, 0))
    {
        result = 3;
    }
    if (result == 0 && WaitForSingleObject(process.hProcess, 5000) != WAIT_OBJECT_0)
    {
        result = 4;
    }
    if (result == 0)
    {
        DWORD exitCode = 0;
        if (!GetExitCodeProcess(process.hProcess, &exitCode) || exitCode != 0)
        {
            result = 5;
        }
    }

    // 失敗時も子 Process と Native Handle を残さない
    if (result != 0)
    {
        TerminateProcess(process.hProcess, static_cast<UINT>(result));
        WaitForSingleObject(process.hProcess, 5000);
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return result;
}
