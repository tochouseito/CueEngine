#include <exception>
#include <utility>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <EditorHost/EditorHost.h>
#include <Platform/Diagnostics.h>

namespace
{
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
        // EditorHost の構成を作成する
        cue::EditorHostConfig config;

        // CueEngine の Repository Root を取得する。EditorHost は構成に関係なく Development モードで起動する
        auto root = cue::Path::create(CUE_REPOSITORY_ROOT);
        if (!root.has_value())
        {
            cue::report_log_error("CueEditorHost storage", *root.try_error(), cue::LogLevel::Fatal);
            return 1;
        }
        // 製品 Host の既定は Product。Repository の Editor 起動は構成に関係なく Development を選ぶ
        config.storage.mode = cue::StorageMode::Development; // Editor が生成するファイルの保存先
        config.storage.repositoryRoot = root.take_value();   // CueEngine の Repository Root
        config.storage.applicationName = "CueEngineEditor"; // アプリケーション名

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
