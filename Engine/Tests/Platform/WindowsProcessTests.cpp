#include <Cue/Platform/Windows/WindowsProcess.h>

#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Fatal.h>
#include <Cue/Foundation/Log.h>

#include <Windows.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace
{
class TestFatalHandler final : public cue::FatalHandler
{
  public:
    /// @brief Test中の回復不能失敗を終了Codeへ変換する
    [[noreturn]] void terminate() noexcept override
    {
        std::_Exit(90);
    }

    /// @brief Message付き回復不能失敗を終了Codeへ変換する
    [[noreturn]] void terminate(std::string_view) noexcept override
    {
        std::_Exit(91);
    }
};

/// @brief 指定StreamのCapture Byte列をSequence順に連結する
[[nodiscard]] std::string collect_stream(const cue::ChildProcessResult &a_result, cue::ChildProcessStream a_stream)
{
    std::string result;
    for (const cue::ChildProcessOutputChunk &chunk : a_result.output())
    {
        if (chunk.stream == a_stream)
        {
            result.append(chunk.bytes);
        }
    }
    return result;
}

/// @brief 指定Markerを作る孫ProcessをCancelまたはTimeoutで残さないことを検証する
[[nodiscard]] bool test_tree_termination(cue::ChildProcessRunner &a_runner, std::string_view a_probe,
                                         std::string_view a_workingDirectory, bool a_cancel)
{
    const std::filesystem::path marker =
        std::filesystem::temp_directory_path() /
        (a_cancel ? "cue-process-cancel-marker.tmp" : "cue-process-timeout-marker.tmp");
    std::error_code error;
    std::filesystem::remove(marker, error);

    cue::ChildProcessCancellation cancellation;
    std::thread canceller;
    if (a_cancel)
    {
        canceller = std::thread(
            [&cancellation]() noexcept
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(150));
                cancellation.request_cancel();
            });
    }
    cue::ChildProcessRequest request(
        std::string(a_probe), {"spawn-tree", marker.string()}, std::string(a_workingDirectory), {},
        a_cancel ? std::optional<std::chrono::milliseconds>(std::chrono::seconds(5))
                 : std::optional<std::chrono::milliseconds>(std::chrono::milliseconds(150)));
    auto result = a_runner.run(request, cancellation);
    if (canceller.joinable())
    {
        canceller.join();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(900));
    const bool markerExists = std::filesystem::exists(marker, error);
    std::filesystem::remove(marker, error);
    const cue::ChildProcessOutcome expected =
        a_cancel ? cue::ChildProcessOutcome::Cancelled : cue::ChildProcessOutcome::TimedOut;
    return result && result.try_value()->outcome() == expected && !result.try_value()->exit_code().has_value() &&
           !markerExists;
}
} // namespace

/// @brief Windows RunnerのExit、Capture順序、Quoting、Environment Allowlist、Process Tree終了を検証する
int main(int a_count, char **a_arguments)
{
    if (a_count != 2)
    {
        return 80;
    }
    TestFatalHandler fatalHandler;
    std::vector<std::unique_ptr<cue::LogSink>> sinks;
    cue::Logger logger(fatalHandler, std::move(sinks));
    cue::AssertContext assertContext(logger, fatalHandler);
    auto created = cue::create_windows_child_process_runner(assertContext);
    if (!created)
    {
        return 81;
    }
    std::unique_ptr<cue::ChildProcessRunner> runner = std::move(*created.try_value());
    const std::string probe = a_arguments[1];
    const std::string workingDirectory = std::filesystem::current_path().string();

    cue::ChildProcessCancellation mixedCancellation;
    cue::ChildProcessRequest mixedRequest(probe, {"mixed"}, workingDirectory, {}, std::chrono::seconds(5));
    auto mixed = runner->run(mixedRequest, mixedCancellation);
    if (!mixed)
    {
        return 120;
    }
    if (mixed.try_value()->outcome() != cue::ChildProcessOutcome::Exited || mixed.try_value()->exit_code() != 7U)
    {
        return 121;
    }
    if (collect_stream(*mixed.try_value(), cue::ChildProcessStream::StandardOutput) != "OUT-1\nOUT-2\n" ||
        collect_stream(*mixed.try_value(), cue::ChildProcessStream::StandardError) != "ERR-1\n")
    {
        return 122;
    }
    if (mixed.try_value()->output().size() < 3U)
    {
        return 123;
    }
    if (mixed.try_value()->output()[0].stream != cue::ChildProcessStream::StandardOutput ||
        mixed.try_value()->output()[1].stream != cue::ChildProcessStream::StandardError)
    {
        return 124;
    }

    cue::ChildProcessCancellation discardedCancellation;
    cue::ChildProcessRequest discardedRequest(probe, {"spam"}, workingDirectory, {}, std::chrono::seconds(5), 0U);
    auto discarded = runner->run(discardedRequest, discardedCancellation);
    if (!discarded || discarded.try_value()->exit_code() != 7U || !discarded.try_value()->output().empty())
    {
        return 125;
    }

    cue::ChildProcessCancellation quoteCancellation;
    cue::ChildProcessRequest quoteRequest(probe, {"echo", "a b", "quote\"tail\\", "& echo injected"}, workingDirectory,
                                          {}, std::chrono::seconds(5));
    auto quoted = runner->run(quoteRequest, quoteCancellation);
    const std::string quotedOutput =
        quoted ? collect_stream(*quoted.try_value(), cue::ChildProcessStream::StandardOutput) : std::string{};
    if (!quoted || quoted.try_value()->exit_code() != 0U ||
        quotedOutput != "ARG=a b\nARG=quote\"tail\\\nARG=& echo injected\n")
    {
        return 83;
    }

    cue::ChildProcessCancellation environmentCancellation;
    if (SetEnvironmentVariableA("CUE_SECRET", "must-not-be-inherited") == FALSE)
    {
        return 89;
    }
    cue::ChildProcessRequest environmentRequest(probe, {"environment"}, workingDirectory, {{"CUE_ALLOWED", "yes"}},
                                                std::chrono::seconds(5));
    auto environment = runner->run(environmentRequest, environmentCancellation);
    static_cast<void>(SetEnvironmentVariableA("CUE_SECRET", nullptr));
    const std::string environmentOutput =
        environment ? collect_stream(*environment.try_value(), cue::ChildProcessStream::StandardOutput) : std::string{};
    if (!environment || environment.try_value()->exit_code() != 0U ||
        environmentOutput.find("CWD=" + workingDirectory) == std::string::npos ||
        environmentOutput.find("ALLOWED=yes") == std::string::npos ||
        environmentOutput.find("SECRET=<missing>") == std::string::npos)
    {
        return 84;
    }

    if (!test_tree_termination(*runner, probe, workingDirectory, true))
    {
        return 85;
    }
    if (!test_tree_termination(*runner, probe, workingDirectory, false))
    {
        return 86;
    }

    cue::ChildProcessCancellation gracefulCancellation;
    std::thread gracefulStopper(
        /// @brief ProbeのWindow生成後に正常停止を通知する
        [&gracefulCancellation]()
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            gracefulCancellation.request_graceful_stop();
        });
    cue::ChildProcessRequest gracefulRequest(probe, {"graceful-window"}, workingDirectory, {},
                                             std::chrono::seconds(10));
    auto graceful = runner->run(gracefulRequest, gracefulCancellation);
    gracefulStopper.join();
    const std::string gracefulOutput =
        graceful ? collect_stream(*graceful.try_value(), cue::ChildProcessStream::StandardOutput) : std::string{};
    if (!graceful || graceful.try_value()->outcome() != cue::ChildProcessOutcome::Cancelled ||
        graceful.try_value()->exit_code().has_value() || gracefulOutput.find("WINDOW_READY") == std::string::npos ||
        gracefulOutput.find("WINDOW_STOPPED") == std::string::npos)
    {
        return 94;
    }

    cue::ChildProcessCancellation failingGracefulCancellation;
    std::thread failingGracefulStopper(
        /// @brief ProbeのWindow生成後に失敗を伴う正常停止を通知する
        [&failingGracefulCancellation]()
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            failingGracefulCancellation.request_graceful_stop();
        });
    cue::ChildProcessRequest failingGracefulRequest(probe, {"graceful-window-fail"}, workingDirectory, {},
                                                    std::chrono::seconds(10));
    auto failingGraceful = runner->run(failingGracefulRequest, failingGracefulCancellation);
    failingGracefulStopper.join();
    const std::string failingGracefulOutput =
        failingGraceful ? collect_stream(*failingGraceful.try_value(), cue::ChildProcessStream::StandardOutput)
                        : std::string{};
    if (!failingGraceful || failingGraceful.try_value()->outcome() != cue::ChildProcessOutcome::Exited ||
        failingGraceful.try_value()->exit_code() != 42U ||
        failingGracefulOutput.find("WINDOW_READY") == std::string::npos ||
        failingGracefulOutput.find("WINDOW_STOPPED") == std::string::npos)
    {
        return 95;
    }

    const std::filesystem::path invalidExecutable =
        std::filesystem::temp_directory_path() / "cue-process-invalid-executable.txt";
    {
        std::ofstream invalidFile(invalidExecutable, std::ios::binary | std::ios::trunc);
        invalidFile << "not an executable";
        if (!invalidFile)
        {
            return 90;
        }
    }
    DWORD failureHandleCountBefore = 0U;
    DWORD failureHandleCountAfter = 0U;
    if (GetProcessHandleCount(GetCurrentProcess(), &failureHandleCountBefore) == FALSE)
    {
        return 91;
    }
    cue::ChildProcessCancellation failureCancellation;
    cue::ChildProcessRequest failureRequest(invalidExecutable.string(), {}, workingDirectory, {},
                                            std::chrono::seconds(5));
    auto failed = runner->run(failureRequest, failureCancellation);
    if (GetProcessHandleCount(GetCurrentProcess(), &failureHandleCountAfter) == FALSE)
    {
        return 92;
    }
    std::error_code removeError;
    std::filesystem::remove(invalidExecutable, removeError);
    if (failed || failureHandleCountAfter != failureHandleCountBefore)
    {
        return 93;
    }

    DWORD handleCountBefore = 0U;
    if (GetProcessHandleCount(GetCurrentProcess(), &handleCountBefore) == FALSE)
    {
        return 88;
    }
    for (std::uint32_t index = 0U; index < 8U; ++index)
    {
        cue::ChildProcessCancellation repeatCancellation;
        cue::ChildProcessRequest repeatRequest(probe, {"echo"}, workingDirectory, {}, std::chrono::seconds(5));
        auto repeated = runner->run(repeatRequest, repeatCancellation);
        if (!repeated || repeated.try_value()->outcome() != cue::ChildProcessOutcome::Exited ||
            repeated.try_value()->exit_code() != 0U)
        {
            return 89;
        }
    }
    DWORD handleCountAfter = 0U;
    if (GetProcessHandleCount(GetCurrentProcess(), &handleCountAfter) == FALSE || handleCountAfter != handleCountBefore)
    {
        std::cout << "handle-count before=" << handleCountBefore << " after=" << handleCountAfter << '\n';
        return 87;
    }
    return 0;
}
