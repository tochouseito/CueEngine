#include <exception>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <Platform/Diagnostics.h>
#include <WindowsHost/WindowsHost.h>

/// @brief Window を表示し、終了要求まで Message を処理する
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    try
    {
        cue::WindowsHost host({"CueEngine Windows Host", {1280, 720}});

        // Window を生成して表示する
        auto initResult = host.initialize();
        if (!initResult.has_value())
        {
            auto stopResult = host.shutdown();
            if (!stopResult.has_value())
            {
                cue::report_error("CueWindowsHost cleanup", *stopResult.try_error(), cue::DiagnosticSeverity::Error);
            }
            cue::report_error("CueWindowsHost", *initResult.try_error(), cue::DiagnosticSeverity::Fatal);
            return 1;
        }

        // Window の終了要求を受けるまで Message を処理する
        while (true)
        {
            auto stepResult = host.step();
            if (!stepResult.has_value())
            {
                auto stopResult = host.shutdown();
                if (!stopResult.has_value())
                {
                    cue::report_error("CueWindowsHost cleanup", *stopResult.try_error(), cue::DiagnosticSeverity::Error);
                }
                cue::report_error("CueWindowsHost", *stepResult.try_error(), cue::DiagnosticSeverity::Fatal);
                return 1;
            }
            if (!*stepResult.try_value())
            {
                break;
            }

            // 描画や Frame 処理がない間は次の Window Message まで待機する
            if (!WaitMessage())
            {
                auto stopResult = host.shutdown();
                if (!stopResult.has_value())
                {
                    cue::report_error("CueWindowsHost cleanup", *stopResult.try_error(), cue::DiagnosticSeverity::Error);
                }
                cue::report_message("CueWindowsHost", "WaitMessage failed", cue::DiagnosticSeverity::Fatal);
                return 1;
            }
        }

        // Window の破棄結果を Process の終了 Code へ反映する
        auto shutdownResult = host.shutdown();
        if (!shutdownResult.has_value())
        {
            cue::report_error("CueWindowsHost", *shutdownResult.try_error(), cue::DiagnosticSeverity::Fatal);
            return 1;
        }
        return 0;
    }
    catch (const std::exception&)
    {
        cue::report_message("CueWindowsHost", "unexpected C++ exception", cue::DiagnosticSeverity::Fatal);
        return 2;
    }
}
