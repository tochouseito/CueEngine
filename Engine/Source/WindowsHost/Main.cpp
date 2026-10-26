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
        // Repository 起動時の生成物を開発用保存先へ揃える
        config.storage.mode = cue::StorageMode::Development;
        config.storage.repositoryRoot = root.take_value();
        cue::WindowsHost host(std::move(config));

        // Window を生成して表示する
        auto initResult = host.initialize();
        if (!initResult.has_value())
        {
            cue::report_log_error("CueWindowsHost", *initResult.try_error(), cue::LogLevel::Fatal);
            // 初期化途中の資源も Host の停止経路で回収する
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
            // Window の Event と Runtime の Frame 処理を進める
            auto stepResult = host.step();
            if (!stepResult.has_value())
            {
                cue::report_log_error("CueWindowsHost", *stepResult.try_error(), cue::LogLevel::Fatal);
                // Frame 実行失敗後も Worker と GPU の停止を試す
                auto stopResult = host.shutdown();
                if (!stopResult.has_value())
                {
                    cue::report_log_error("CueWindowsHost cleanup", *stopResult.try_error(), cue::LogLevel::Error);
                }
                return 1;
            }
            // Close 要求を受けたら新しい Frame の投入をやめる
            if (!*stepResult.try_value())
            {
                break;
            }
        }

        // Window の破棄結果を Process の終了 Code へ反映する
        // 通常終了でも明示停止の結果を確認する
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
