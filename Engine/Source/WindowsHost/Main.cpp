#include <exception>
#include <utility>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <Platform/Diagnostics.h>
#include <WindowsHost/WindowsHost.h>

/// @brief Window を表示し、終了要求まで Message を処理する
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    try
    {
        // 設定 File の導入までは起動設定を所有値で構築する
        cue::WindowsHostConfig config{{"CueEngine Windows Host", {1280, 720}}, {}, {}};
        // Repository 内の検証 Host は全 Build 構成で開発保存先を明示する
        auto root = cue::Path::create(CUE_REPOSITORY_ROOT);
        if (!root.has_value())
        {
            cue::report_log_error("CueWindowsHost storage", *root.try_error(), cue::LogLevel::Fatal);
            return 1;
        }
        config.storage.mode = cue::StorageMode::Development;
        config.storage.repositoryRoot = root.take_value();
        cue::WindowsHost host(std::move(config));

        // Window を生成して表示する
        auto initResult = host.initialize();
        if (!initResult.has_value())
        {
            cue::report_log_error("CueWindowsHost", *initResult.try_error(), cue::LogLevel::Fatal);
            auto stopResult = host.shutdown();
            if (!stopResult.has_value())
            {
                cue::report_log_error("CueWindowsHost cleanup", *stopResult.try_error(), cue::LogLevel::Error);
            }
            return 1;
        }

        // Window の終了要求まで Message と Frame を同じ Main Loop で進める
        while (true)
        {
            auto stepResult = host.step();
            if (!stepResult.has_value())
            {
                cue::report_log_error("CueWindowsHost", *stepResult.try_error(), cue::LogLevel::Fatal);
                auto stopResult = host.shutdown();
                if (!stopResult.has_value())
                {
                    cue::report_log_error("CueWindowsHost cleanup", *stopResult.try_error(), cue::LogLevel::Error);
                }
                return 1;
            }
            if (!*stepResult.try_value())
            {
                break;
            }
        }

        // Window の破棄結果を Process の終了 Code へ反映する
        auto shutdownResult = host.shutdown();
        if (!shutdownResult.has_value())
        {
            cue::report_log_error("CueWindowsHost", *shutdownResult.try_error(), cue::LogLevel::Fatal);
            return 1;
        }
        return 0;
    }
    catch (const std::exception&)
    {
        cue::report_log("CueWindowsHost", "unexpected C++ exception", cue::LogLevel::Fatal);
        return 2;
    }
}
