#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// Shell API は Win32 の型宣言後に Include する
#include <shellapi.h>

#include <EditorHost/EditorHost.h>
#include <EditorHost/EditorLaunchOptions.h>
#include <EditorIde/Windows/WindowsIdeService.h>
#include <EngineInstallation/EngineInstallation.h>
#include <Foundation/Windows/UtfConversion.h>
#include <Platform/Diagnostics.h>
#include <Platform/Windows/WindowsFileSystem.h>
#include <ProjectBuild/Windows/WindowsGameBuildService.h>

namespace
{
/// @brief Editor を生成した環境の Tool 配置を所有値へ変換する。起動 Shell の PATH を検索しない
cue::Result<std::unique_ptr<cue::IGameBuildService>> create_build_service(cue::IFileSystem &a_files,
                                                                          const cue::EngineInstallation &a_installation)
{
    // 配布版は同梱 SDK と利用者の CMake を使い、生成元 Toolchain を引き継がない
    if (a_installation.isPackaged)
    {
        auto cmake = cue::locate_engine_cmake(a_files, a_installation);
        if (!cmake.has_value())
            return cue::Result<std::unique_ptr<cue::IGameBuildService>>::failure(*cmake.try_error());
        return cue::create_windows_game_build_service({a_installation.root, cmake.take_value(), {}, {}});
    }
    auto cmake = cue::Path::create(CUE_EDITOR_CMAKE);
    auto toolchain = cue::Path::create(CUE_EDITOR_TOOLCHAIN);
    auto vcpkg = cue::Path::create(CUE_EDITOR_VCPKG_ROOT);
    if (!cmake.has_value() || !toolchain.has_value() || !vcpkg.has_value())
        return cue::Result<std::unique_ptr<cue::IGameBuildService>>::failure(
            {cue::ErrorCategory::InvalidArgument, "EditorHost.build.tool_paths"});
    return cue::create_windows_game_build_service(
        {a_installation.root, cmake.take_value(), toolchain.take_value(), vcpkg.take_value()});
}

/// @brief Editor 横に配置した事前 Build 済み Bridge を明示し、操作ごとの dotnet run を避ける
cue::Result<std::unique_ptr<cue::IEditorIdeService>> create_ide_service(cue::IFileSystem &a_files)
{
    auto directory = a_files.executable_directory();
    if (!directory.has_value())
        return cue::Result<std::unique_ptr<cue::IEditorIdeService>>::failure(*directory.try_error());
    auto executable = directory.try_value()->join("VisualStudioBridge.exe");
    if (!executable.has_value())
        return cue::Result<std::unique_ptr<cue::IEditorIdeService>>::failure(*executable.try_error());
    return cue::create_windows_ide_service({executable.take_value(), 120000});
}

/// @brief OS が保持する Command Line を標準の Windows 規則で分割し、UTF-8 の共通引数へ変換する
cue::Result<cue::EditorLaunchOptions> read_launch_options()
{
    int count = 0;
    auto **arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!arguments)
    {
        return cue::Result<cue::EditorLaunchOptions>::failure(
            {cue::ErrorCategory::PlatformFailure, "CommandLineToArgvW", GetLastError()});
    }
    // UTF-8 への変換失敗や例外でも、OS の確保した引数配列を必ず解放する
    const auto release = [](wchar_t **a_values) { LocalFree(a_values); };
    std::unique_ptr<wchar_t *, decltype(release)> owned(arguments, release);
    std::vector<std::string> values;
    for (int index = 1; index < count; ++index)
    {
        auto value = cue::utf16_to_utf8(arguments[index]);
        if (!value.has_value())
        {
            return cue::Result<cue::EditorLaunchOptions>::failure(*value.try_error());
        }
        values.push_back(value.take_value());
    }
    std::vector<std::string_view> views(values.begin(), values.end());
    return cue::parse_editor_arguments(views);
}

/// @brief Logger の起動前に発生した設定失敗を画面へ示し、通常の終了失敗として返す
int report_startup_failure(const cue::Error &a_error)
{
    cue::report_log_error("CueEditorHost project", a_error, cue::LogLevel::Error);
    const auto message =
        "Project could not be opened.\n" + a_error.operation + "\nNative code: " + std::to_string(a_error.nativeCode);
    auto text = cue::utf8_to_utf16(message);
    MessageBoxW(nullptr, text.has_value() ? text.try_value()->c_str() : L"Project could not be opened.",
                L"CueEngine Editor - Project", MB_OK | MB_ICONERROR);
    return 1;
}

/// @brief 実行失敗を保持して停止し、Cleanup の失敗も診断する
int stop_after_failure(cue::EditorHost &a_host, const cue::Error &a_error)
{
    // 元の実行失敗を診断し、後始末の失敗と区別して残す
    cue::report_log_error("CueEditorHost", a_error, cue::LogLevel::Fatal);
    auto stopResult = a_host.shutdown();
    if (!stopResult.has_value())
    {
        cue::report_log_error("CueEditorHost cleanup", *stopResult.try_error(), cue::LogLevel::Error);
    }
    return 1;
}
} // namespace

/// @brief Editor 用 Window を表示し、Close 要求まで Frame を進める
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    try
    {
        // Window や Worker を生成する前に、指定された Project 全体の検証を完了する
        auto options = read_launch_options();
        if (!options.has_value())
        {
            return report_startup_failure(*options.try_error());
        }
        auto files = cue::create_windows_file_system();
        if (!files.has_value())
        {
            return report_startup_failure(*files.try_error());
        }
        // 配布識別と共有 Lease は Host 全体より長く保ち、Hub の修復・削除と競合させる
        auto installationFiles = cue::create_windows_file_system();
        auto developmentRoot = cue::Path::create(CUE_REPOSITORY_ROOT);
        if (!installationFiles.has_value() || !developmentRoot.has_value())
            return report_startup_failure(!installationFiles.has_value() ? *installationFiles.try_error()
                                                                         : *developmentRoot.try_error());
        auto installation =
            cue::resolve_engine_installation(**installationFiles.try_value(), *developmentRoot.try_value());
        if (!installation.has_value())
            return report_startup_failure(*installation.try_error());
        auto engineLease = cue::acquire_engine_use_lease(**installationFiles.try_value(), *installation.try_value());
        if (!engineLease.has_value())
            return report_startup_failure(*engineLease.try_error());
        std::optional<cue::Project> project;
        std::optional<cue::Scene> initialScene;
        if (!options.try_value()->projectFile.is_empty())
        {
            auto loaded = cue::Project::load(**files.try_value(), options.try_value()->projectFile);
            if (!loaded.has_value())
                return report_startup_failure(*loaded.try_error());
            project = loaded.take_value();
            if (project->descriptor().engineVersion &&
                *project->descriptor().engineVersion != installation.try_value()->version)
                return report_startup_failure(
                    {cue::ErrorCategory::InvalidArgument, "EditorHost.project_engine_version"});

            // 指定された起動 Scene は表示基盤の生成前に検証する。未指定なら空の Shell を開始する
            if (const auto *sceneFile = project->startup_scene_file())
            {
                auto scene = cue::Scene::load(**files.try_value(), *sceneFile);
                if (!scene.has_value())
                    return report_startup_failure(*scene.try_error());
                initialScene = scene.take_value();
            }
        }
        // 読み込み済みの値と FileSystem の所有権を EditorHost へ移す
        cue::EditorHostConfig config;
        config.enableProjectOpening = true;
        config.window.title = project ? "CueEngine Editor - " + project->descriptor().name : "CueEngine Editor";
        config.project = std::move(project);
        config.initialScene = std::move(initialScene);
        config.fileSystem = files.take_value();
        auto ide = create_ide_service(*config.fileSystem);
        if (ide.has_value())
            config.ideService = ide.take_value();
        else
            config.ideUnavailableReason = ide.try_error()->operation;
        // Tool の移動や未導入で Build が使えなくても、Scene 編集は開始できるよう理由を表示へ渡す
        auto build = create_build_service(*config.fileSystem, *installation.try_value());
        if (build.has_value())
            config.gameBuildService = build.take_value();
        else
            config.gameBuildUnavailableReason = build.try_error()->operation;

        // 配布物は変更せず、Log / Cache をユーザー領域へ分離する
        config.storage.mode =
            installation.try_value()->isPackaged ? cue::StorageMode::Product : cue::StorageMode::Development;
        config.storage.repositoryRoot = installation.try_value()->root;
        config.storage.applicationName = "CueEngineEditor-" + installation.try_value()->version;

        // EditorHost を生成し、Window と Renderer を初期化する
        cue::EditorHost host(std::move(config));
        // UI・Window・Renderer・Runtime を順に初期化する
        auto initResult = host.initialize();
        if (!initResult.has_value())
        {
            return stop_after_failure(host, *initResult.try_error());
        }

        // メインループ。Close 要求が来るまで step() を繰り返す
        while (true)
        {
            // Message Pump と採用 Frame の処理を一周期進める
            auto stepResult = host.step();
            if (!stepResult.has_value())
            {
                return stop_after_failure(host, *stepResult.try_error());
            }
            // 通常の Close 要求は失敗とせず Main Loop を終了する
            if (!*stepResult.try_value())
            {
                break;
            }
        }

        // GPU と Worker の停止を明示し、停止失敗も終了 Code に反映する
        auto stopResult = host.shutdown();
        if (!stopResult.has_value())
        {
            cue::report_log_error("CueEditorHost", *stopResult.try_error(), cue::LogLevel::Fatal);
            return 1;
        }
        return 0;
    }
    catch (const std::exception &)
    {
        // Result に変換されなかった C++ 例外を最上位で診断する
        cue::report_log("CueEditorHost", "unexpected C++ exception", cue::LogLevel::Fatal);
        return 2;
    }
}
