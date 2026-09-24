#include <Cue/RuntimeHost/RuntimeHost.h>

#include <cstdint>
#include <cstdio>
#include <exception>
#include <stop_token>
#include <string_view>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace
{
struct RuntimeOptions final
{
    std::uint64_t testFrameLimit = 0;
    bool useWorkerThreads = true;
    bool failRenderForTest = false;
};

/// @brief Test用の自動終了と失敗注入だけを起動引数から読み取る
cue::Result<RuntimeOptions> parse_options(std::wstring_view a_commandLine)
{
    RuntimeOptions options;
    constexpr std::wstring_view k_framePrefix = L"--test-frames=";
    bool hasSingleThread = false;
    while (!a_commandLine.empty())
    {
        while (!a_commandLine.empty() && a_commandLine.front() == L' ')
        {
            a_commandLine.remove_prefix(1);
        }
        if (a_commandLine.empty())
        {
            break;
        }
        const auto separator = a_commandLine.find(L' ');
        const auto option = a_commandLine.substr(0, separator);
        if (option == L"--single-thread" && !hasSingleThread)
        {
            options.useWorkerThreads = false;
            hasSingleThread = true;
        }
        else if (option == L"--test-fail-render" && !options.failRenderForTest)
        {
            options.failRenderForTest = true;
        }
        else if (option.starts_with(k_framePrefix) && options.testFrameLimit == 0)
        {
            std::uint64_t frameLimit = 0;
            for (wchar_t digit : option.substr(k_framePrefix.size()))
            {
                if (digit < L'0' || digit > L'9' || frameLimit > 1000)
                {
                    return cue::Result<RuntimeOptions>::failure(
                        {cue::ErrorCategory::InvalidArgument, "CueRuntimeHost.options"});
                }
                frameLimit = frameLimit * 10 + static_cast<std::uint64_t>(digit - L'0');
            }
            if (frameLimit == 0 || frameLimit > 1000)
            {
                return cue::Result<RuntimeOptions>::failure(
                    {cue::ErrorCategory::InvalidArgument, "CueRuntimeHost.options"});
            }
            options.testFrameLimit = frameLimit;
        }
        else
        {
            return cue::Result<RuntimeOptions>::failure(
                {cue::ErrorCategory::InvalidArgument, "CueRuntimeHost.options"});
        }
        a_commandLine.remove_prefix(separator == std::wstring_view::npos ? a_commandLine.size() : separator + 1);
    }
    return cue::Result<RuntimeOptions>::success(options);
}

/// @brief Host境界の失敗をDebuggerへ渡して失敗終了する
int report_failure(const cue::Error& a_error)
{
    char message[256]{};
    std::snprintf(message, sizeof(message), "CueRuntimeHost: %s (nativeCode=%lld)\n",
                  a_error.operation.c_str(), static_cast<long long>(a_error.nativeCode));
    OutputDebugStringA(message);
    return 1;
}

/// @brief 元の失敗を保ちながらWorkerとWindowのCleanupを終える
int stop_with_failure(cue::RuntimeHost& a_host, const cue::Error& a_error)
{
    auto stopResult = a_host.shutdown();
    if (!stopResult.has_value())
    {
        [[maybe_unused]] const int cleanupExitCode = report_failure(*stopResult.try_error());
    }
    return report_failure(a_error);
}

/// @brief Sceneがない段階ではFrame更新を行わない
cue::Result<void> update_frame(std::uint64_t, std::stop_token)
{
    return cue::Result<void>::success();
}
} // namespace

/// @brief Window終了までMessage PumpとFrameControllerを進める
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR a_commandLine, int)
{
    try
    {
        auto optionsResult = parse_options(a_commandLine ? a_commandLine : L"");
        if (!optionsResult.has_value())
        {
            return report_failure(*optionsResult.try_error());
        }
        const auto options = optionsResult.take_value();
        cue::RuntimeHost host({{"CueEngine Runtime Host", {1280, 720}},
                               {2, options.useWorkerThreads, 60}, options.testFrameLimit});

        // Renderer接続前はRenderを空処理とし、Test指定時だけ失敗を注入する
        auto initResult = host.initialize(&update_frame,
                                          [failRender = options.failRenderForTest](std::uint64_t, std::stop_token) {
                                              if (failRender)
                                              {
                                                  return cue::Result<void>::failure(
                                                      {cue::ErrorCategory::InvalidState, "CueRuntimeHost.test.render"});
                                              }
                                              return cue::Result<void>::success();
                                          });
        if (!initResult.has_value())
        {
            return stop_with_failure(host, *initResult.try_error());
        }
        while (true)
        {
            auto pumpResult = host.pump_events();
            if (!pumpResult.has_value())
            {
                return stop_with_failure(host, *pumpResult.try_error());
            }
            if (!*pumpResult.try_value())
            {
                break;
            }
            auto stepResult = host.frame_controller().step();
            if (!stepResult.has_value())
            {
                return stop_with_failure(host, *stepResult.try_error());
            }
        }
        auto shutdownResult = host.shutdown();
        return shutdownResult.has_value() ? 0 : report_failure(*shutdownResult.try_error());
    }
    catch (const std::exception&)
    {
        OutputDebugStringW(L"CueRuntimeHost: unexpected C++ exception\n");
        return 2;
    }
}
