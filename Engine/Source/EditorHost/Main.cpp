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
        cue::EditorHostConfig config;
        auto root = cue::Path::create(CUE_REPOSITORY_ROOT);
        if (!root.has_value())
        {
            cue::report_log_error("CueEditorHost storage", *root.try_error(), cue::LogLevel::Fatal);
            return 1;
        }
        // 製品 Host の既定は Product。Repository の Editor 起動は構成に関係なく Development を選ぶ
        config.storage.mode = cue::StorageMode::Development;
        config.storage.repositoryRoot = root.take_value();
        config.storage.applicationName = "CueEngineEditor";
        cue::EditorHost host(std::move(config));
        auto initResult = host.initialize();
        if (!initResult.has_value())
        {
            return stop_after_failure(host, *initResult.try_error());
        }

        while (true)
        {
            auto stepResult = host.step();
            if (!stepResult.has_value())
            {
                return stop_after_failure(host, *stepResult.try_error());
            }
            if (!*stepResult.try_value())
            {
                break;
            }
        }

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
        cue::report_log("CueEditorHost", "unexpected C++ exception", cue::LogLevel::Fatal);
        return 2;
    }
}
