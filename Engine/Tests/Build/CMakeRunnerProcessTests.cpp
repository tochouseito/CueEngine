#include <Cue/Build/CMakeRunner.h>

#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Fatal.h>
#include <Cue/Foundation/Log.h>
#include <Cue/Platform/Windows/WindowsProcess.h>

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
constexpr cue::BuildWorkspaceCompatibility k_workspaceCompatibility{
    cue::BuildGenerator::VisualStudio2026, cue::BuildArchitecture::X64, {19U, 51U, 0U, 0U}, 1U};

class TestFatalHandler final : public cue::FatalHandler
{
  public:
    /// @brief 想定外の引数なしFatal終了を固有Exit Codeで検出する
    [[noreturn]] void terminate() noexcept override
    {
        std::_Exit(90);
    }
    /// @brief 想定外のMessage付きFatal終了を固有Exit Codeで検出する
    [[noreturn]] void terminate(std::string_view) noexcept override
    {
        std::_Exit(91);
    }
};

class Observer final : public cue::CMakeStageObserver
{
  public:
    /// @brief Process境界TestではStage開始通知だけを受理する
    void on_stage_started(cue::BuildStage) noexcept override
    {
    }
    /// @brief Process境界TestではStage完了通知だけを受理する
    void on_stage_completed(const cue::CMakeStageRecord &) noexcept override
    {
    }
};

/// @brief 一つのConfigurationを実Process境界経由でConfigureとBuildまで往復検証する
[[nodiscard]] bool run_configuration(cue::BuildConfiguration a_configuration, std::string_view a_probe,
                                     cue::ChildProcessRunner &a_runner, const cue::AssertContext &a_assertContext)
{
    auto profile = cue::BuildProfile::create(a_configuration, cue::BuildTarget::GameModule, a_assertContext);
    cue::BuildRequest request{std::filesystem::current_path().string(), *profile.try_value(),
                              "01234567-89ab-4cde-8f01-23456789abcd", k_workspaceCompatibility};
    auto plan = cue::create_build_plan(request, a_assertContext);
    if (!plan)
    {
        return false;
    }
    cue::CMakeRunnerSettings settings{std::string(a_probe),
                                      std::filesystem::current_path().string(),
                                      {},
                                      std::chrono::seconds(5),
                                      std::chrono::seconds(5),
                                      "14.51.36231"};
    cue::ChildProcessCancellation cancellation;
    Observer observer;
    auto result = cue::run_cmake_build(*plan.try_value(), settings, cue::CMakeConfigureMode::Required, a_runner,
                                       cancellation, observer, a_assertContext);
    return result && result.try_value()->succeeded() && result.try_value()->stages().size() == 2U;
}
} // namespace

/// @brief Windows Process境界で3構成のCMake Argument Vectorを往復検証する
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
    auto runner = cue::create_windows_child_process_runner(assertContext);
    if (!runner)
    {
        return 81;
    }
    return run_configuration(cue::BuildConfiguration::Debug, a_arguments[1], **runner.try_value(), assertContext) &&
                   run_configuration(cue::BuildConfiguration::Development, a_arguments[1], **runner.try_value(),
                                     assertContext) &&
                   run_configuration(cue::BuildConfiguration::Release, a_arguments[1], **runner.try_value(),
                                     assertContext)
               ? 0
               : 82;
}
