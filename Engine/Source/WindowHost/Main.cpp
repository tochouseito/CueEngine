#include "WindowHost.h"

#include <cstdio>
#include <exception>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace
{
/// @brief Errorの診断値をDebuggerへ渡して失敗終了する
int report_failure(const cue::Error* a_error)
{
    if (a_error)
    {
        char message[256]{};
        std::snprintf(message, sizeof(message), "CueWindowHost: %s (nativeCode=%lld)\n",
                      a_error->operation.c_str(), static_cast<long long>(a_error->nativeCode));
        OutputDebugStringA(message);
    }
    return 1;
}
} // namespace

/// @brief Windowの終了要求までFrameControllerを直接進める
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR a_commandLine, int)
{
    try
    {
        cue::WindowHost host(a_commandLine ? a_commandLine : L"");
        auto initResult = host.initialize();
        if (!initResult.has_value())
        {
            return report_failure(initResult.try_error());
        }
        while (true)
        {
            auto pumpResult = host.pump_events();
            if (!pumpResult.has_value())
            {
                return report_failure(pumpResult.try_error());
            }
            if (!*pumpResult.try_value())
            {
                break;
            }

            auto stepResult = host.frame_controller().step();
            if (!stepResult.has_value())
            {
                return report_failure(stepResult.try_error());
            }
        }
        auto shutdownResult = host.shutdown();
        return shutdownResult.has_value() ? 0 : report_failure(shutdownResult.try_error());
    }
    catch (const std::exception&)
    {
        OutputDebugStringW(L"CueWindowHost: unexpected C++ exception\n");
        return 2;
    }
}
