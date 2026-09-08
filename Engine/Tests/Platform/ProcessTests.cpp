#include <Cue/Platform/Process.h>

#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Fatal.h>
#include <Cue/Foundation/Log.h>

#include <chrono>
#include <cstdlib>
#include <memory>
#include <string_view>
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

/// @brief Platform非依存CallerがNative Runnerなしで結果とCancelを検証するFake
class FakeRunner final : public cue::ChildProcessRunner
{
  public:
    [[nodiscard]] cue::Result<cue::ChildProcessResult> run(
        const cue::ChildProcessRequest &, const cue::ChildProcessCancellation &a_cancellation) noexcept override
    {
        std::vector<cue::ChildProcessOutputChunk> output;
        output.push_back({0U, cue::ChildProcessStream::StandardOutput, "out"});
        output.push_back({1U, cue::ChildProcessStream::StandardError, "error"});
        return cue::Result<cue::ChildProcessResult>::success(
            a_cancellation.is_cancel_requested() ? cue::ChildProcessResult::cancelled(std::move(output))
                                                 : cue::ChildProcessResult::exited(7U, std::move(output)));
    }
};
} // namespace

/// @brief Request所有値、Fake Runner、CancelとOutput順序のPortable契約を検証する
int main()
{
    TestFatalHandler fatalHandler;
    std::vector<std::unique_ptr<cue::LogSink>> sinks;
    cue::Logger logger(fatalHandler, std::move(sinks));
    cue::AssertContext assertContext(logger, fatalHandler);

    cue::ChildProcessRequest request("C:/Tools/tool.exe", {"--value", "a b"}, "C:/Project", {{"CUE_ALLOWED", "value"}},
                                     std::chrono::seconds(2));
    if (request.executable() != "C:/Tools/tool.exe" || request.arguments().size() != 2U ||
        request.working_directory() != "C:/Project" || request.environment_allowlist().size() != 1U ||
        request.timeout() != std::chrono::seconds(2))
    {
        return 1;
    }

    FakeRunner runner;
    cue::ChildProcessCancellation runningCancellation;
    auto exited = runner.run(request, runningCancellation);
    if (!exited || exited.try_value()->outcome() != cue::ChildProcessOutcome::Exited ||
        exited.try_value()->exit_code() != 7U || exited.try_value()->output().size() != 2U ||
        exited.try_value()->output()[0].sequence >= exited.try_value()->output()[1].sequence)
    {
        return 2;
    }

    cue::ChildProcessCancellation cancelledCancellation;
    cancelledCancellation.request_cancel();
    auto cancelled = runner.run(request, cancelledCancellation);
    return cancelled && cancelled.try_value()->outcome() == cue::ChildProcessOutcome::Cancelled &&
                   !cancelled.try_value()->exit_code().has_value() && cancelledCancellation.is_cancel_requested()
               ? 0
               : 3;
}
