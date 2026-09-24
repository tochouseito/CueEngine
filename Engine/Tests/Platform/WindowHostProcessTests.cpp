#include <cstdint>

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
} // namespace

/// @brief 実HostがWindowを表示しClose後に正常終了することを確認する
int wmain(int a_argumentCount, wchar_t* a_arguments[])
{
    // CTest から実 Host の Path を一つ受け取る
    if (a_argumentCount != 2)
    {
        return 1;
    }

    // 実 Host を別 Process で起動して表示と終了を確認する
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(a_arguments[1], nullptr, nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                        &startup, &process))
    {
        return 2;
    }

    // Window 表示まで最大5秒だけ待つ
    WindowSearch search{process.dwProcessId};
    for (int attempt = 0; attempt < 200 && !search.handle; ++attempt)
    {
        EnumWindows(&find_window, reinterpret_cast<LPARAM>(&search));
        if (!search.handle)
        {
            Sleep(25);
        }
    }

    // Title Bar の Close と同じ Message を送り、正常終了を待つ
    int result = 0;
    if (!search.handle || !PostMessageW(search.handle, WM_CLOSE, 0, 0))
    {
        result = 3;
    }
    else if (WaitForSingleObject(process.hProcess, 5000) != WAIT_OBJECT_0)
    {
        result = 4;
    }
    else
    {
        DWORD exitCode = 0;
        if (!GetExitCodeProcess(process.hProcess, &exitCode) || exitCode != 0)
        {
            result = 5;
        }
    }

    // 失敗時は子 Process を残さず、Native Handle も閉じる
    if (result != 0)
    {
        TerminateProcess(process.hProcess, static_cast<UINT>(result));
        WaitForSingleObject(process.hProcess, 5000);
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return result;
}
