#include <Windows.h>

#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace
{
/// @brief UTF-16 Test値をUTF-8へ変換する
[[nodiscard]] std::string to_utf8(std::wstring_view a_text)
{
    if (a_text.empty())
    {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, a_text.data(), static_cast<int>(a_text.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (size <= 0)
    {
        return {};
    }
    std::string result(static_cast<std::size_t>(size), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, a_text.data(), static_cast<int>(a_text.size()),
                            result.data(), size, nullptr, nullptr) != size)
    {
        return {};
    }
    return result;
}

/// @brief 指定Standard Handleへ一回のByte列を書き込む
[[nodiscard]] bool write_stream(DWORD a_stream, std::string_view a_text) noexcept
{
    DWORD written = 0U;
    return WriteFile(GetStdHandle(a_stream), a_text.data(), static_cast<DWORD>(a_text.size()), &written, nullptr) !=
               FALSE &&
           written == a_text.size();
}

/// @brief Probe自身と引数をWindows規則でQuoteする
void append_argument(std::wstring &a_commandLine, std::wstring_view a_argument)
{
    if (!a_commandLine.empty())
    {
        a_commandLine.push_back(L' ');
    }
    a_commandLine.push_back(L'"');
    std::size_t slashCount = 0U;
    for (const wchar_t character : a_argument)
    {
        if (character == L'\\')
        {
            ++slashCount;
            continue;
        }
        if (character == L'"')
        {
            a_commandLine.append(slashCount * 2U + 1U, L'\\');
            a_commandLine.push_back(L'"');
            slashCount = 0U;
            continue;
        }
        a_commandLine.append(slashCount, L'\\');
        slashCount = 0U;
        a_commandLine.push_back(character);
    }
    a_commandLine.append(slashCount * 2U, L'\\');
    a_commandLine.push_back(L'"');
}

/// @brief 同じJobへ継承される遅延Marker Childを開始する
[[nodiscard]] bool spawn_delayed_marker(std::wstring_view a_marker)
{
    std::vector<wchar_t> executable(32768U, L'\0');
    const DWORD size = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (size == 0U || size >= executable.size())
    {
        return false;
    }
    const std::wstring executablePath(executable.data(), size);
    std::wstring commandLine;
    append_argument(commandLine, executablePath);
    append_argument(commandLine, L"delayed-marker");
    append_argument(commandLine, a_marker);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (CreateProcessW(executablePath.c_str(), commandLine.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                       nullptr, &startup, &process) == FALSE)
    {
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}
} // namespace

/// @brief Windows Process RunnerのCapture、Quoting、Environment、Tree終了を検証するChild Probe
int wmain(int a_count, wchar_t **a_arguments)
{
    if (a_count < 2)
    {
        return 90;
    }
    const std::wstring_view mode(a_arguments[1]);
    if (mode == L"mixed")
    {
        if (!write_stream(STD_OUTPUT_HANDLE, "OUT-1\n"))
        {
            return 91;
        }
        Sleep(100U);
        if (!write_stream(STD_ERROR_HANDLE, "ERR-1\n"))
        {
            return 92;
        }
        Sleep(100U);
        return write_stream(STD_OUTPUT_HANDLE, "OUT-2\n") ? 7 : 93;
    }
    if (mode == L"echo")
    {
        for (int index = 2; index < a_count; ++index)
        {
            const std::string line = "ARG=" + to_utf8(a_arguments[index]) + "\n";
            if (!write_stream(STD_OUTPUT_HANDLE, line))
            {
                return 94;
            }
        }
        return 0;
    }
    if (mode == L"environment")
    {
        std::vector<wchar_t> currentDirectory(32768U, L'\0');
        const DWORD length = GetCurrentDirectoryW(static_cast<DWORD>(currentDirectory.size()), currentDirectory.data());
        wchar_t allowed[128] = {};
        const DWORD allowedLength = GetEnvironmentVariableW(L"CUE_ALLOWED", allowed, 128U);
        wchar_t secret[128] = {};
        const DWORD secretLength = GetEnvironmentVariableW(L"CUE_SECRET", secret, 128U);
        const std::string text =
            "CWD=" + to_utf8(std::wstring_view(currentDirectory.data(), length)) +
            "\nALLOWED=" + (allowedLength > 0U ? to_utf8(std::wstring_view(allowed, allowedLength)) : "<missing>") +
            "\nSECRET=" + (secretLength > 0U ? to_utf8(std::wstring_view(secret, secretLength)) : "<missing>") + "\n";
        return write_stream(STD_OUTPUT_HANDLE, text) ? 0 : 95;
    }
    if (mode == L"delayed-marker" && a_count == 3)
    {
        Sleep(800U);
        HANDLE marker =
            CreateFileW(a_arguments[2], GENERIC_WRITE, 0U, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (marker == INVALID_HANDLE_VALUE)
        {
            return 96;
        }
        CloseHandle(marker);
        return 0;
    }
    if (mode == L"spawn-tree" && a_count == 3)
    {
        if (!spawn_delayed_marker(a_arguments[2]))
        {
            return 97;
        }
        Sleep(10000U);
        return 0;
    }
    return 98;
}
