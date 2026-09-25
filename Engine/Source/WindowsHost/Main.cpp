#include <cstdint>
#include <exception>
#include <stop_token>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <Cue/Platform/Diagnostics.h>
#include <Cue/WindowsHost/WindowsHost.h>

namespace
{
/// @brief Sceneがない段階ではFrame更新を行わない
cue::Result<void> update_frame(std::uint64_t, std::stop_token)
{
    return cue::Result<void>::success();
}
} // namespace

/// @brief Window終了までWindowsHostのstepを進める
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    try
    {
        cue::WindowsHost host({{"CueEngine Windows Host", {1280, 720}}, {2, true, 60}});

        // Hostの初期化中にRendererを作り、Render Callbackへ単色描画を登録する
        auto initResult = host.initialize_renderer(&update_frame);
        if (!initResult.has_value())
        {
            // 部分初期化を回収し、Cleanup の失敗より起動失敗を主原因として残す
            auto stopResult = host.shutdown();
            if (!stopResult.has_value())
            {
                cue::report_error("CueWindowsHost cleanup", *stopResult.try_error(), cue::DiagnosticSeverity::Error);
            }
            cue::report_error("CueWindowsHost", *initResult.try_error(), cue::DiagnosticSeverity::Fatal);
            return 1;
        }
        // Window が終了を要求するまで、Host に Event 処理と Frame 進行を委ねる
        while (true)
        {
            auto stepResult = host.step();
            if (!stepResult.has_value())
            {
                // Cleanup の失敗は別に報告し、Frame 失敗を主原因として残す
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
                // Window 終了要求は失敗ではないため、通常の Shutdown へ進む
                break;
            }
        }

        // 正常終了時も Worker と Window の停止結果を Process の終了Codeへ反映する
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
        // Result に変換されなかった例外を Process 境界で診断する
        cue::report_message("CueWindowsHost", "unexpected C++ exception", cue::DiagnosticSeverity::Fatal);
        return 2;
    }
}
