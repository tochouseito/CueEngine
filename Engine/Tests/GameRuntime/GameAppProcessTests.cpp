#include <GameRuntime/GameStartup.h>

#include <array>
#include <cstdio>
#include <cwchar>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

// Process 内 Module の検証だけに使い、製品の公開 API には依存させない
#include <psapi.h>

#include <Foundation/Windows/UtfConversion.h>
#include <Platform/FileSystem.h>

#include "../Support/TemporaryFiles.h"

namespace
{
#define CHECK(...)                                                                                                     \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(__VA_ARGS__))                                                                                            \
        {                                                                                                              \
            std::fprintf(stderr, "Game app process test line %d: %s\n", __LINE__, #__VA_ARGS__);                    \
            return __LINE__;                                                                                           \
        }                                                                                                              \
    } while (false)

/// @brief Test が起動した Process の Timeout 回収を保証する
struct Process final
{
    HANDLE handle = nullptr;
    DWORD id = 0;
    /// @brief Test の異常終了でも専用子 Process を残さない
    ~Process()
    {
        if (handle)
        {
            if (WaitForSingleObject(handle, 0) == WAIT_TIMEOUT)
            {
                TerminateProcess(handle, 98);
                WaitForSingleObject(handle, 3000);
            }
            CloseHandle(handle);
        }
    }
};

/// @brief 成功した Fixture Path を所有する
cue::Path path(cue::Result<cue::Path> a_value)
{
    return a_value.take_value();
}
/// @brief 所有 Fixture へ通常 File / Directory だけを複写し、Link を追わない
bool copy_tree(cue::IFileSystem &a_files, const cue::Path &a_source, const cue::Path &a_destination)
{
    auto info = a_files.stat(a_source);
    if (!info.has_value() || !*info.try_value() || (**info.try_value()).type == cue::FileType::Link)
        return false;
    if ((**info.try_value()).type == cue::FileType::Regular)
        return a_files.copy_file(a_source, a_destination, false).has_value();
    if (!a_files.create_directories(a_destination).has_value())
        return false;
    auto entries = a_files.list_directory(a_source);
    if (!entries.has_value())
        return false;
    for (const auto &entry : *entries.try_value())
        if (!copy_tree(a_files, entry, path(a_destination.join(entry.filename()))))
            return false;
    return true;
}
/// @brief 通常の Windows 引数規則で一つの所有 Path を引用する
std::wstring quote(std::wstring_view a_text)
{
    std::wstring result = L"\"";
    std::size_t slashes = 0;
    for (auto character : a_text)
    {
        if (character == L'\\')
        {
            ++slashes;
            continue;
        }
        result.append(character == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        slashes = 0;
        result += character;
    }
    result.append(slashes * 2, L'\\');
    return result + L'"';
}
/// @brief 実行 File と CWD を別々に指定し、Editor / Play Host を経由せず起動する
bool launch(Process &a_process, const cue::Path &a_executable, const cue::Path &a_cwd, std::wstring a_arguments)
{
    auto executable = cue::utf8_to_utf16(a_executable.utf8());
    auto cwd = cue::utf8_to_utf16(a_cwd.utf8());
    if (!executable.has_value() || !cwd.has_value())
        return false;
    std::wstring command = quote(*executable.try_value()) + L" " + a_arguments;
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.try_value()->c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        cwd.try_value()->c_str(), &startup, &process))
        return false;
    CloseHandle(process.hThread);
    a_process.handle = process.hProcess;
    a_process.id = process.dwProcessId;
    return true;
}
struct WindowSearch final
{
    DWORD processId;
    HWND window = nullptr;
};
/// @brief 起動した子だけの可視 Window を見つける
BOOL CALLBACK find_window(HWND a_window, LPARAM a_context)
{
    auto &search = *reinterpret_cast<WindowSearch *>(a_context);
    DWORD processId = 0;
    GetWindowThreadProcessId(a_window, &processId);
    if (processId == search.processId && IsWindowVisible(a_window))
    {
        search.window = a_window;
        return FALSE;
    }
    return TRUE;
}
/// @brief ゲーム DLL / Editor / Play Host の Module を製品が読み込んでいないことを確認する
bool has_only_product_modules(Process &a_process, std::wstring_view a_gameModule)
{
    std::array<HMODULE, 512> modules{};
    DWORD required = 0;
    if (!K32EnumProcessModules(a_process.handle, modules.data(), sizeof(modules), &required) ||
        required > sizeof(modules))
        return false;
    for (std::size_t index = 0; index < required / sizeof(HMODULE); ++index)
    {
        std::array<wchar_t, 32768> name{};
        const auto length =
            K32GetModuleFileNameExW(a_process.handle, modules[index], name.data(), static_cast<DWORD>(name.size()));
        if (!length || length >= name.size())
            return false;
        std::wstring_view file(name.data(), length);
        file.remove_prefix(file.find_last_of(L"/\\") == std::wstring_view::npos ? 0 : file.find_last_of(L"/\\") + 1);
        const auto matches = [&](std::wstring_view a_expected)
        {
            return CompareStringOrdinal(file.data(), static_cast<int>(file.size()), a_expected.data(),
                                        static_cast<int>(a_expected.size()), TRUE) == CSTR_EQUAL;
        };
        if (matches(a_gameModule) || matches(L"CueEditorHost.exe") || matches(L"CuePlayHost.exe"))
            return false;
    }
    return true;
}
/// @brief 終了後の検証結果だけを読む。計測中の File 監視はしない
std::string read(cue::IFileSystem &a_files, const cue::Path &a_file)
{
    auto bytes = a_files.read_all(a_file, 65536);
    if (!bytes.has_value())
        return {};
    const auto &data = *bytes.try_value();
    return std::string(data.empty() ? "" : reinterpret_cast<const char *>(data.data()), data.size());
}
/// @brief 検証 Report の一行を所有値で返す
std::string value(std::string_view a_report, std::string_view a_key)
{
    const auto key = std::string(a_key) + '=';
    const auto position = a_report.find(key);
    if (position == std::string_view::npos || (position && a_report[position - 1] != '\n'))
        return {};
    const auto begin = position + key.size();
    const auto end = a_report.find('\n', begin);
    return std::string(a_report.substr(begin, end == std::string_view::npos ? end : end - begin));
}
/// @brief 移動済み Package を別 CWD から実行し、可視描画・静的登録・回転・正常停止を検証する
int test_run(cue::IFileSystem &a_files, const cue::Path &a_package, const cue::Path &a_cwd, const cue::Path &a_report,
             const cue::GameStartupConfig &a_config)
{
    const auto executable = path(a_package.join(a_config.applicationName + ".exe"));
    auto report = cue::utf8_to_utf16(a_report.utf8());
    auto gameModule = cue::utf8_to_utf16(a_config.gameTarget + ".dll");
    CHECK(report.has_value() && gameModule.has_value());
    Process process;
    CHECK(launch(process, executable, a_cwd, L"--warp --frames 90 --validation-report " + quote(*report.try_value())));
    bool sawWindow = false;
    bool checkedModules = false;
    const auto deadline = GetTickCount64() + 20000;
    while (WaitForSingleObject(process.handle, 0) == WAIT_TIMEOUT && GetTickCount64() < deadline)
    {
        WindowSearch search{process.id};
        EnumWindows(find_window, reinterpret_cast<LPARAM>(&search));
        sawWindow = sawWindow || search.window != nullptr;
        if (search.window && !checkedModules)
        {
            CHECK(has_only_product_modules(process, *gameModule.try_value()));
            checkedModules = true;
        }
        Sleep(10);
    }
    DWORD exitCode = STILL_ACTIVE;
    CHECK(GetExitCodeProcess(process.handle, &exitCode) && exitCode == 0);
    CHECK(sawWindow && checkedModules);
    const auto text = read(a_files, a_report);
    CHECK(value(text, "staticRegistration") == "true" && value(text, "normalShutdown") == "true");
    CHECK(std::stoull(value(text, "completedRenders")) >= 90 &&
          std::stoull(value(text, "completedSimulationTicks")) > 0);
    CHECK(std::stoull(value(text, "meshCount")) >= 1 && value(text, "hasCamera") == "true");
    CHECK(!value(text, "initialWorld").empty() && value(text, "initialWorld") != value(text, "finalWorld"));
    return 0;
}
/// @brief 完成 Marker の欠損を通常の起動失敗として返し、Editor 接続を要求しない
int test_incomplete(cue::IFileSystem &a_files, const cue::Path &a_package, const cue::Path &a_cwd,
                    const cue::GameStartupConfig &a_config)
{
    auto marker = path(a_package.join("GamePackage.cuepackage"));
    auto hidden = path(a_package.join("GamePackage.saved"));
    CHECK(a_files.rename(marker, hidden).has_value());
    Process process;
    CHECK(launch(process, path(a_package.join(a_config.applicationName + ".exe")), a_cwd, L"--validate-only"));
    CHECK(WaitForSingleObject(process.handle, 8000) == WAIT_OBJECT_0);
    DWORD exitCode = 0;
    CHECK(GetExitCodeProcess(process.handle, &exitCode) && exitCode != 0);
    CHECK(a_files.rename(hidden, marker).has_value());
    return 0;
}
} // namespace

/// @brief リポジトリ外の日本語 / 空白 Path に配布物だけをコピーして製品起動を確認する
int wmain(int a_count, wchar_t **a_arguments)
{
    CHECK(a_count == 2);
    auto packageText = cue::utf16_to_utf8(a_arguments[1]);
    CHECK(packageText.has_value());
    auto package = cue::Path::create(*packageText.try_value());
    CHECK(package.has_value());
    cue::tests::TemporaryFiles fixture;
    CHECK(fixture.isCreated && fixture.files);
    auto &files = *fixture.files;
    const auto moved = path(fixture.root.join("配布済み Cube"));
    const auto cwd = path(fixture.root.join("別の作業 Directory"));
    CHECK(files.create_directories(cwd).has_value() && copy_tree(files, *package.try_value(), moved));
    auto config = cue::load_game_startup(files, moved);
    CHECK(config.has_value());
    for (const auto &name :
         {config.try_value()->gameTarget + ".dll", std::string("CueEditorHost.exe"), std::string("CuePlayHost.exe")})
    {
        auto present = files.exists(path(moved.join(name)));
        CHECK(present.has_value() && !*present.try_value());
    }
    auto result = test_run(files, moved, cwd, path(fixture.root.join("validation.txt")), *config.try_value());
    if (!result)
        result = test_incomplete(files, moved, cwd, *config.try_value());
    return result;
}
