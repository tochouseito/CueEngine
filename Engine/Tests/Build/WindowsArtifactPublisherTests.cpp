#include <Cue/Build/Windows/WindowsArtifactPublisher.h>

#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Fatal.h>
#include <Cue/Foundation/Log.h>
#include <Cue/Project/Descriptor.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{
constexpr std::string_view k_projectId = "41234567-89ab-4cde-8f01-23456789abcd";

#if CUE_TEST_BUILD_CONFIGURATION == 1
constexpr cue::BuildConfiguration k_configuration = cue::BuildConfiguration::Debug;
constexpr std::string_view k_configurationName = "Debug";
#elif CUE_TEST_BUILD_CONFIGURATION == 2
constexpr cue::BuildConfiguration k_configuration = cue::BuildConfiguration::Development;
constexpr std::string_view k_configurationName = "Development";
#elif CUE_TEST_BUILD_CONFIGURATION == 3
constexpr cue::BuildConfiguration k_configuration = cue::BuildConfiguration::Release;
constexpr std::string_view k_configurationName = "Release";
#else
#error CUE_TEST_BUILD_CONFIGURATION must identify a supported configuration
#endif

class TestFatalHandler final : public cue::FatalHandler
{
  public:
    /// @brief Test中の引数なしFatalを即時失敗として終了する
    [[noreturn]] void terminate() noexcept override
    {
        std::abort();
    }
    /// @brief Test中のFatalを即時失敗として終了する
    [[noreturn]] void terminate(std::string_view) noexcept override
    {
        std::abort();
    }
};

/// @brief 条件違反時にTest Processを失敗終了する
void require(bool a_condition) noexcept
{
    if (!a_condition)
    {
        std::abort();
    }
}

/// @brief Result成功値を所有値として取得する
template <typename T> [[nodiscard]] T take_value(cue::Result<T> a_result) noexcept
{
    require(a_result.has_value());
    return std::move(*a_result.try_value());
}

/// @brief UTF-8 Path表示をTest入力用stringへ変換する
[[nodiscard]] std::string generic_path(const std::filesystem::path &a_path)
{
    const std::u8string text = a_path.generic_u8string();
    return std::string(reinterpret_cast<const char *>(text.data()), text.size());
}

/// @brief 小さいManifest Fileを検証用文字列として読む
[[nodiscard]] std::string read_text(const std::filesystem::path &a_path)
{
    std::ifstream input(a_path, std::ios::binary);
    require(static_cast<bool>(input));
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

/// @brief Test Project契約を満たすDescriptorを構築する
[[nodiscard]] cue::ProjectDescriptor make_descriptor(const cue::AssertContext &a_assertContext)
{
    cue::ProjectId projectId = take_value(cue::ProjectId::parse(k_projectId, a_assertContext));
    return take_value(cue::create_blank_project_descriptor(projectId, "Artifact Publisher Test",
                                                           {{1U, 0U, 0U}, std::nullopt}, a_assertContext));
}

/// @brief Test用Build PlanをOperation ID別に構築する
[[nodiscard]] cue::BuildPlan make_plan(const std::filesystem::path &a_projectRoot, std::string a_operationId,
                                       const cue::AssertContext &a_assertContext)
{
    cue::BuildProfile profile =
        take_value(cue::BuildProfile::create(k_configuration, cue::BuildTarget::GameModule, a_assertContext));
    constexpr cue::BuildWorkspaceCompatibility compatibility{
        cue::BuildGenerator::VisualStudio2026, cue::BuildArchitecture::X64, {19U, 51U, 36256U, 0U}, 1U};
    return take_value(cue::create_build_plan(
        {generic_path(a_projectRoot), std::move(profile), std::move(a_operationId), compatibility}, a_assertContext));
}

/// @brief Artifact公開、Lock取消、失敗時Current保全を一つのProject Rootで検証する
void test_windows_artifact_publisher(const std::filesystem::path &a_probe, const cue::AssertContext &a_assertContext)
{
    const std::filesystem::path projectRoot =
        a_probe.parent_path() / ("CueBuildArtifactPublisherTests-" + std::string(k_configurationName));
    std::error_code error;
    std::filesystem::remove_all(projectRoot, error);
    require(!error);
    require(std::filesystem::create_directories(projectRoot));

    cue::ProjectDescriptor descriptor = make_descriptor(a_assertContext);
    std::unique_ptr<cue::BuildArtifactPublisher> publisher = take_value(
        cue::create_windows_build_artifact_publisher(generic_path(projectRoot), descriptor, a_assertContext));
    cue::BuildPlan plan = make_plan(projectRoot, "01234567-89ab-4cde-8f01-23456789abcd", a_assertContext);
    const std::filesystem::path binaryDirectory = std::filesystem::path(plan.binary_directory());
    const std::filesystem::path outputDirectory = binaryDirectory / "bin" / k_configurationName;
    require(std::filesystem::create_directories(outputDirectory));
    require(std::filesystem::copy_file(a_probe, outputDirectory / "CueGameModule.dll"));

    cue::ChildProcessCancellation cancellation;
    auto lease = take_value(publisher->acquire_build_lease(plan, cancellation));
    require(lease.has_value());

    std::unique_ptr<cue::BuildArtifactPublisher> contender = take_value(
        cue::create_windows_build_artifact_publisher(generic_path(projectRoot), descriptor, a_assertContext));
    cue::ChildProcessCancellation contenderCancellation;
    using LeaseResult = cue::Result<std::optional<std::unique_ptr<cue::BuildWorkspaceLease>>>;
    std::unique_ptr<LeaseResult> contenderResult;
    std::thread contenderThread(
        [&]()
        {
            contenderResult =
                std::make_unique<LeaseResult>(contender->acquire_build_lease(plan, contenderCancellation));
        });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    contenderCancellation.request_cancel();
    contenderThread.join();
    require(contenderResult != nullptr && contenderResult->has_value() && !contenderResult->try_value()->has_value());

    auto published = take_value(publisher->publish(plan, cancellation, std::move(*lease)));
    require(published.has_value());
    require(published->artifact_id() == plan.operation_id());
    require(published->files().size() == 2U);
    const std::filesystem::path store = std::filesystem::path(plan.artifact_store_directory());
    const std::filesystem::path version = store / "Versions" / std::string(plan.operation_id());
    require(std::filesystem::is_regular_file(version / "CueGameModule.dll"));
    require(std::filesystem::is_regular_file(version / "CueGameModule.metadata.json"));
    const std::string metadata = read_text(version / "CueGameModule.metadata.json");
    require(metadata.find(std::string(k_projectId)) != std::string::npos);
    require(metadata.find("\"configuration\": \"" + std::string(k_configurationName) + "\"") != std::string::npos);
    require(metadata.find("\"compilerVersion\": 1951") != std::string::npos);
    require(metadata.find("\"fullVersion\": 195136256") != std::string::npos);
    require(metadata.find("\"build\": 0") != std::string::npos);
    const std::filesystem::path currentPath = store / "Current.json";
    const std::string current = read_text(currentPath);
    require(current.find(std::string(plan.operation_id())) != std::string::npos);
    require(current.find("sha256") != std::string::npos);

    cue::BuildPlan failedPlan = make_plan(projectRoot, "11234567-89ab-4cde-8f01-23456789abcd", a_assertContext);
    auto failedLease = take_value(publisher->acquire_build_lease(failedPlan, cancellation));
    require(failedLease.has_value());
    require(std::filesystem::remove(outputDirectory / "CueGameModule.dll"));
    require(!publisher->publish(failedPlan, cancellation, std::move(*failedLease)).has_value());
    require(read_text(currentPath) == current);

    std::filesystem::remove_all(projectRoot, error);
    require(!error);
}
} // namespace

/// @brief Windows Artifact PublisherのProcess間契約とAtomic Current保全を検証する
int main(int a_argumentCount, char **a_arguments)
{
    require(a_argumentCount == 2);
    TestFatalHandler fatalHandler;
    std::vector<std::unique_ptr<cue::LogSink>> sinks;
    cue::Logger logger(fatalHandler, std::move(sinks));
    cue::AssertContext assertContext(logger, fatalHandler);
    test_windows_artifact_publisher(std::filesystem::path(a_arguments[1]), assertContext);
    return 0;
}
