#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <string_view>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
// Command Line の宣言は Win32 型の後で Include する
#include <shellapi.h>

/// @brief 実 IDE に接続せず、非同期 Bridge の入力と応答を再現する
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    int count = 0;
    auto **raw = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!raw)
        return 1;
    const auto free = [](wchar_t **a_values) { LocalFree(a_values); };
    std::unique_ptr<wchar_t *, decltype(free)> owned(raw, free);
    std::map<std::wstring, std::wstring> values;
    for (int index = 1; index + 1 < count; index += 2)
        values.emplace(raw[index], raw[index + 1]);
    const auto workspace = values[L"--workspace"];
    if (workspace.empty() || values.contains(L"--solution"))
        return 1;
    if (workspace.find(L"Hang") != std::wstring::npos)
    {
        Sleep(30000);
        return 1;
    }
    std::wstring operation = values[L"--command"];
    std::string command(operation.begin(), operation.end());
    const auto request = values[L"--request-id"];
    std::string id(request.begin(), request.end());
    if (workspace.find(L"BadIdentity") != std::wstring::npos)
        id = "999999";
    if (workspace.find(L"Large") != std::wstring::npos)
    {
        std::string big(100000, 'x');
        DWORD written = 0;
        WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), big.data(), static_cast<DWORD>(big.size()), &written, nullptr);
        return 0;
    }
    const bool failed = workspace.find(L"Failed") != std::wstring::npos;
    const auto target = values[L"--process-id"];
    std::string pid(target.begin(), target.end());
    if (pid.empty())
        pid = "0";
    const auto output = "formatVersion=1\nrequestId=" + id + "\noperation=" + command +
                        "\nsuccess=" + (failed ? "false" : "true") + "\nideProcessId=1234\ntargetProcessId=" + pid +
                        "\nmessage=Fixture complete\n";
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), output.data(), static_cast<DWORD>(output.size()), &written, nullptr);
    return failed ? 17 : 0;
}
