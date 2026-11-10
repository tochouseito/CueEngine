#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

// Shell の型宣言は Win32 の型宣言後に読む
#include <shellapi.h>

namespace
{
/// @brief Fixture の所有 Directory 内だけへ短い File を作る
void write(const std::filesystem::path &a_file, std::string_view a_text)
{
    std::filesystem::create_directories(a_file.parent_path());
    std::ofstream file(a_file, std::ios::binary | std::ios::trunc);
    file.write(a_text.data(), static_cast<std::streamsize>(a_text.size()));
}
/// @brief Fixture の小さい設定だけを読む
std::string read(const std::filesystem::path &a_file)
{
    std::ifstream file(a_file, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
/// @brief 日本語 Path を Command Line 経由で取り違えないよう UTF-8 へ変換する
std::string utf8(std::wstring_view a_text)
{
    const auto count =
        WideCharToMultiByte(CP_UTF8, 0, a_text.data(), static_cast<int>(a_text.size()), nullptr, 0, nullptr, nullptr);
    std::string text(count, '\0');
    WideCharToMultiByte(CP_UTF8, 0, a_text.data(), static_cast<int>(a_text.size()), text.data(), count, nullptr,
                        nullptr);
    return text;
}
/// @brief Configure で渡された Project を次の Build Process へ File で引き継ぐ
std::filesystem::path from_utf8(std::string_view a_text)
{
    const auto count = MultiByteToWideChar(CP_UTF8, 0, a_text.data(), static_cast<int>(a_text.size()), nullptr, 0);
    std::wstring text(count, 0);
    MultiByteToWideChar(CP_UTF8, 0, a_text.data(), static_cast<int>(a_text.size()), text.data(), count);
    return std::filesystem::path(text);
}
/// @brief Service の Job に自動参加する孫 Process の進行を確認する
int heartbeat(const std::filesystem::path &a_file)
{
    for (unsigned count = 0; count < 3000; ++count)
    {
        write(a_file, std::to_string(count));
        Sleep(20);
    }
    return 0;
}
/// @brief 製品起動の独立寿命を GPU に依存せず検証する
int game()
{
    const auto cwd = std::filesystem::current_path();
    write(cwd / L"Started.txt", "started");
    Sleep(1200);
    write(cwd / L"Finished.txt", "finished");
    return 0;
}
/// @brief 配布済み exe 自身を通常 File としてコピーする
std::filesystem::path self()
{
    std::array<wchar_t, 32768> file{};
    const auto length = GetModuleFileNameW(nullptr, file.data(), static_cast<DWORD>(file.size()));
    return std::filesystem::path(std::wstring_view(file.data(), length));
}
/// @brief Package の Schema を本番と同じ形にし、identity 不一致も注入できるようにする
void package(const std::filesystem::path &a_project, std::string_view a_config, std::string_view a_mode)
{
    const auto root = a_project.parent_path();
    const auto output = root / L"Build" / L"Windows" / L"x64" / from_utf8(a_config) / L"App";
    std::filesystem::create_directories(output / L"Content" / L"Scenes");
    std::filesystem::copy_file(self(), output / L"FixtureApp.exe",
                               std::filesystem::copy_options::overwrite_existing);
    auto scene = read(root / L"Content" / L"Scenes" / L"Start.cuescene");
    const std::string sceneName = a_mode == "wrong-scene" ? "Other.cuescene" : "Start.cuescene";
    write(output / L"Content" / L"Scenes" / from_utf8(sceneName), scene);
    const auto config = a_mode == "wrong-config" ? std::string("Development") : std::string(a_config);
    const std::string identity =
        "formatVersion=1\nprojectId=8f533c49-d416-47ea-98c1-026822b1e6ed\ngameTarget=FixtureGame\n"
        "applicationName=FixtureApp\nplatform=Windows\narchitecture=x64\nconfiguration=" +
        config + '\n';
    write(output / L"GameStartup.cueapp",
          identity + "startupScene=Content/Scenes/" + sceneName +
              "\nwindowTitle=Fixture\nwindowWidth=640\nwindowHeight=480\ntargetFramesPerSecond=60\n");
    if (a_mode != "missing-marker")
        write(output / L"GamePackage.cuepackage", identity);
    for (const auto *name : {L"dxcompiler.dll", L"dxil.dll", L"EngineResources/Shader/Hlsl/StaticMesh.hlsl",
                             L"EngineResources/Shader/Hlsl/FullscreenTriangle.hlsl"})
        write(output / name, "Fixture resource");
    if (a_mode == "large-log")
    {
        // 短行を多数出し、保持上限と一回の poll の処理予算を確認する
        for (unsigned line = 0; line < 230000; ++line)
            std::fputs("line\n", stdout);
        const char embedded[] = {'N', '\0', 'U', '\n'};
        std::fwrite(embedded, 1, sizeof(embedded), stdout);
        std::fputs("LAST FIXTURE LINE\n", stdout);
        std::fflush(stdout);
    }
}
/// @brief SDK の DLL を Load せず、完成 Manifest / Hash / PDB の検証境界だけを再現する
void module(const std::filesystem::path &a_project, std::string_view a_config, std::string_view a_mode)
{
    const auto output = a_project.parent_path() / L"Build" / L"Windows" / L"x64" / from_utf8(a_config);
    write(output / L"Runtime" / L"FixtureGame.dll", "abc");
    write(output / L"Libraries" / L"FixtureGameStatic.lib", "hello");
    if (a_mode != "missing-symbols")
        write(output / L"Runtime" / L"FixtureGame.pdb", "Fixture PDB");
    else
        std::filesystem::remove(output / L"Runtime" / L"FixtureGame.pdb");
    const auto configuration = a_mode == "module-wrong-config" ? "Development" : std::string(a_config);
    std::string manifest = "formatVersion=1\nprojectId=8f533c49-d416-47ea-98c1-026822b1e6ed\ngameTarget=FixtureGame\n"
                           "configuration=" +
                           configuration +
                           "\nplatform=Windows\narchitecture=x64\nabiVersion=1\n"
                           "module=Runtime/FixtureGame.dll\nstaticLibrary=Libraries/FixtureGameStatic.lib\n"
                           "moduleSha256=ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\n"
                           "staticLibrarySha256=2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824\n";
    write(output / L"GameModule.cuebuild", manifest);
    if (a_mode == "module-bad-hash")
        write(output / L"Runtime" / L"FixtureGame.dll", "changed");
}
/// @brief 取消時に Root だけでなく子孫も止まることを再現する
int hang(const std::filesystem::path &a_root)
{
    auto executable = self();
    auto line = L"\"" + executable.wstring() + L"\" --heartbeat \"" + (a_root / L"Heartbeat.txt").wstring() + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                        a_root.c_str(), &startup, &process))
        return 30;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    Sleep(60000);
    return 31;
}
} // namespace

/// @brief CMake と製品の代役として、引数・終了値・Pipe・Job の実境界だけを検証する
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    try
    {
        int count = 0;
        auto **arguments = CommandLineToArgvW(GetCommandLineW(), &count);
        if (!arguments)
            return 40;
        const auto free = [](wchar_t **a_values) { LocalFree(a_values); };
        std::unique_ptr<wchar_t *, decltype(free)> owned(arguments, free);
        if (count == 1)
            return game();
        if (count == 3 && std::wstring_view(arguments[1]) == L"--heartbeat")
            return heartbeat(arguments[2]);
        bool isBuild = false;
        std::filesystem::path tree;
        std::filesystem::path project;
        std::filesystem::path sourceRoot;
        std::string config = "Debug";
        std::string target;
        for (int index = 1; index < count; ++index)
        {
            const std::wstring_view value(arguments[index]);
            if (value == L"--build" || value == L"-B")
            {
                isBuild = value == L"--build";
                if (++index >= count)
                    return 41;
                tree = arguments[index];
            }
            else if (value == L"--config")
            {
                if (++index >= count)
                    return 42;
                config = utf8(arguments[index]);
            }
            else if (value == L"--target")
            {
                if (++index >= count)
                    return 46;
                target = utf8(arguments[index]);
            }
            else if (value.starts_with(L"-DCUE_GAME_PROJECT="))
                project = value.substr(std::wstring_view(L"-DCUE_GAME_PROJECT=").size());
            else if (value == L"-S")
            {
                if (++index >= count)
                    return 47;
                sourceRoot = arguments[index];
            }
        }
        if (tree.empty())
            return 43;
        if (isBuild)
            project = from_utf8(read(tree / L"FixtureProject.txt"));
        if (project.empty() || !std::filesystem::exists(project))
            return 44;
        const auto root = project.parent_path();
        if (std::filesystem::current_path() != root || (!isBuild && sourceRoot != root))
            return 48;
        const auto mode = read(root / L"Mode.txt");
        if (!isBuild)
        {
            write(tree / L"FixtureProject.txt", utf8(project.wstring()));
            // 旧 Solution が残る場合も、IDE に返すのはゲーム Root Directory とする
            write(tree / L"CueEngine.slnx", "Fixture old solution\n");
            if (mode == "ambiguous-solution")
                write(tree / L"CueEngine.sln", "Another old solution\n");
            std::fputs("CONFIGURE FIXTURE\n", stdout);
            std::fflush(stdout);
            if (mode == "configure-fail")
                return 17;
            if (mode == "hang")
                return hang(root);
            return 0;
        }
        std::fputs("BUILD FIXTURE\n", stderr);
        if (mode == "build-fail")
            return 23;
        if (target == "Cue.GameProjectBuild")
            module(project, config, mode);
        else if (target == "Cue.GameAppPackage")
            package(project, config, mode);
        else
            return 47;
        return 0;
    }
    catch (...)
    {
        return 45;
    }
}
