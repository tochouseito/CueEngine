#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <array>
#include <string>
#include <string_view>

#include <Foundation/Windows/CommandLine.h>

namespace
{
constexpr std::array<std::wstring_view, 6> k_expected{
    L"--child", L"", L"日本語の Path", LR"(a"b)", LR"(a\"b)", LR"(C:\with space\)"
};
} // namespace

/// @brief CreateProcessW を経由し、子 Process の CRT が受け取った引数を検証する
int wmain(int a_count, wchar_t **a_arguments)
{
    if (a_count > 1)
    {
        if (a_count != static_cast<int>(k_expected.size()) + 1)
        {
            return 1;
        }
        for (std::size_t i = 0; i < k_expected.size(); ++i)
        {
            if (std::wstring_view(a_arguments[i + 1]) != k_expected[i])
            {
                return 2;
            }
        }
        return 0;
    }

    const std::array<std::string, 6> arguments{
        "--child", "", "日本語の Path", R"(a"b)", R"(a\"b)", R"(C:\with space\)"
    };
    std::wstring executable(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (length == 0 || length >= executable.size())
    {
        return 3;
    }
    executable.resize(length);
    auto line = cue::make_windows_command_line(executable, arguments);
    if (!line.has_value())
    {
        return 4;
    }
    const std::array<std::string, 1> invalid{std::string("a\0b", 3)};
    if (cue::make_windows_command_line(executable, invalid).has_value())
    {
        return 5;
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(executable.c_str(), line.try_value()->data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &startup, &child))
    {
        return 6;
    }
    CloseHandle(child.hThread);
    DWORD exitCode = 1;
    if (WaitForSingleObject(child.hProcess, 5000) == WAIT_OBJECT_0)
    {
        GetExitCodeProcess(child.hProcess, &exitCode);
    }
    else
    {
        TerminateProcess(child.hProcess, 1);
    }
    CloseHandle(child.hProcess);
    return exitCode == 0 ? 0 : 7;
}
