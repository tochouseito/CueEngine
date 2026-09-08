#include <Cue/Build/DiagnosticBundle.h>

#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Fatal.h>
#include <Cue/Foundation/Log.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <source_location>
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

/// @brief Test前提違反をSource Line由来のExit Codeで即時報告する
void require(bool a_condition, std::source_location a_location = std::source_location::current()) noexcept
{
    if (!a_condition)
    {
        std::_Exit(static_cast<int>((a_location.line() % 200U) + 20U));
    }
}

/// @brief 成功Resultから値所有権を取得し失敗をTest Errorとして扱う
template <typename T> [[nodiscard]] T take_value(cue::Result<T> a_result) noexcept
{
    require(a_result.has_value());
    return std::move(*a_result.try_value());
}

/// @brief Native Filesystem Pathを公開境界へ渡せるUTF-8 Generic Pathへ変換する
[[nodiscard]] std::string generic_utf8_path(const std::filesystem::path &a_path)
{
    const std::u8string encoded = a_path.generic_u8string();
    std::string result;
    result.reserve(encoded.size());
    for (const char8_t value : encoded)
    {
        result.push_back(static_cast<char>(value));
    }
    return result;
}

/// @brief 指定Project RootからDiagnostic検証用Build Planを作成する
[[nodiscard]] cue::BuildPlan make_plan(std::string_view a_projectRoot, const cue::AssertContext &a_assertContext)
{
    cue::BuildProfile profile = take_value(
        cue::BuildProfile::create(cue::BuildConfiguration::Debug, cue::BuildTarget::GameModule, a_assertContext));
    return take_value(cue::create_build_plan({std::string(a_projectRoot), std::move(profile),
                                              "01234567-89ab-4cde-8f01-23456789abcd", k_workspaceCompatibility},
                                             a_assertContext));
}

/// @brief 必須Fileを含むDiagnostic検証用Artifact Inventoryを作成する
[[nodiscard]] cue::BuildArtifactInventory make_artifact(const cue::BuildPlan &a_plan,
                                                        const cue::AssertContext &a_assertContext)
{
    return take_value(cue::BuildArtifactInventory::create(a_plan, "11234567-89ab-4cde-8f01-23456789abcd",
                                                          {{"CueGameModule.dll", 256U, std::string(64U, 'a')},
                                                           {"CueGameModule.metadata.json", 64U, std::string(64U, 'b')}},
                                                          a_assertContext));
}

/// @brief Redaction対象Pathを含むDiagnostic検証用Environment Reportを作成する
[[nodiscard]] cue::BuildEnvironmentReport make_environment()
{
    cue::BuildEnvironmentReport environment;
    environment.support = cue::BuildEnvironmentSupport::Supported;
    environment.engineSourceRoot = "C:/Users/Tester/CueEngine";
    environment.engineBinaryRoot = "C:/Users/Tester/CueEngine/out/build";
    environment.selectedTools.push_back({cue::BuildToolKind::CMake, "C:/Program Files/CMake/bin/cmake.exe",
                                         "C:/Program Files/CMake", cue::BuildToolVersion{4U, 2U, 0U, 0U},
                                         cue::BuildArchitecture::X64, true});
    environment.supportedConfigurations = {cue::BuildConfiguration::Debug};
    return environment;
}

/// @brief Stage失敗、Log、診断、直近Artifactを含むOperation Snapshotを作成する
[[nodiscard]] cue::BuildOperationSnapshot make_failed_operation(const cue::BuildPlan &a_plan,
                                                                const cue::AssertContext &a_assertContext)
{
    cue::BuildOperationSnapshot operation;
    operation.state = cue::GameBuildOperationState::Failed;
    operation.operationId = std::string(a_plan.operation_id());
    operation.profile = a_plan.profile();
    operation.stages = {{cue::BuildStage::Configure, cue::BuildStageOutcome::Succeeded, 0U},
                        {cue::BuildStage::Build, cue::BuildStageOutcome::Failed, 2U}};
    operation.logs = {{operation.operationId, cue::BuildStage::Configure, 0U, cue::ChildProcessStream::StandardOutput,
                       "Configuring C:\\Users\\Tester\\CueProject\\Source\\Game\n"},
                      {operation.operationId, cue::BuildStage::Build, 1U, cue::ChildProcessStream::StandardError,
                       "C:/Users/Tester/CueProject/Source/Game/Game.cpp(1): error\n"}};
    operation.diagnostics = {{"Cue.Build", 2, "Build failed", {"C:/Users/Tester/CueProject"}}};
    operation.latestSuccessfulArtifact = make_artifact(a_plan, a_assertContext);
    return operation;
}

/// @brief Manifestから指定PathのEntryを検索する
[[nodiscard]] const cue::BuildDiagnosticManifestEntry *find_entry(const cue::BuildDiagnosticBundle &a_bundle,
                                                                  std::string_view a_path) noexcept
{
    for (const cue::BuildDiagnosticManifestEntry &entry : a_bundle.manifest_entries())
    {
        if (entry.relativePath == a_path)
        {
            return &entry;
        }
    }
    return nullptr;
}

/// @brief Bundle全FileのByte列を検査用文字列へ連結する
[[nodiscard]] std::string bundle_text(const cue::BuildDiagnosticBundle &a_bundle)
{
    std::string text;
    for (const cue::BuildDiagnosticBundleFile &file : a_bundle.files())
    {
        for (const std::byte value : file.bytes)
        {
            text.push_back(static_cast<char>(std::to_integer<unsigned char>(value)));
        }
    }
    return text;
}

/// @brief 失敗Build、Redaction、欠損理由、上限、Directory再読込を検証する
void test_diagnostic_bundle(std::string_view a_testRoot, const cue::AssertContext &a_assertContext)
{
    const std::filesystem::path projectRoot = std::filesystem::path(a_testRoot) / "CueBuildDiagnosticProject";
    std::error_code cleanupError;
    std::filesystem::remove_all(projectRoot, cleanupError);
    require(!cleanupError);
    require(std::filesystem::create_directories(projectRoot));
    cue::BuildPlan plan = make_plan(projectRoot.generic_string(), a_assertContext);
    cue::BuildDiagnosticBundleInput input{make_failed_operation(plan, a_assertContext),
                                          cue::make_build_diagnostic_plan_snapshot(plan, a_assertContext),
                                          make_environment(),
                                          {{"C:/Users/Tester", "<USER_HOME>"}}};
    cue::BuildDiagnosticBundleLimits limits;
    cue::BuildDiagnosticBundle bundle = take_value(cue::create_build_diagnostic_bundle(input, limits, a_assertContext));
    require(bundle.state() == cue::GameBuildOperationState::Failed);
    require(bundle.files().size() == 8U);
    require(find_entry(bundle, "environment.json") != nullptr);
    require(find_entry(bundle, "environment.json")->collected);
    require(find_entry(bundle, "artifact.json") != nullptr);
    require(find_entry(bundle, "artifact.json")->collected);
    const std::string allText = bundle_text(bundle);
    require(allText.find("C:/Users/Tester") == std::string::npos);
    require(allText.find("C:\\Users\\Tester") == std::string::npos);
    require(allText.find("<PROJECT_ROOT>") != std::string::npos);
    require(allText.find("Source/Game/Game.cpp") != std::string::npos);
    require(allText.find("4.2.0.0") != std::string::npos);
    require(allText.find("supportedConfigurations") != std::string::npos);

    const std::filesystem::path destination =
        std::filesystem::path(a_testRoot) / L"CueBuildDiagnosticBundleTests-\u8A3A\u65AD-01234567";
    const std::string destinationUtf8 = generic_utf8_path(destination);
    std::filesystem::remove_all(destination, cleanupError);
    require(!cleanupError);
    require(cue::write_build_diagnostic_bundle_directory(bundle, destinationUtf8, a_assertContext).has_value());
    require(!cue::write_build_diagnostic_bundle_directory(bundle, destinationUtf8, a_assertContext).has_value());
    cue::BuildDiagnosticBundle reloaded =
        take_value(cue::read_build_diagnostic_bundle_directory(destinationUtf8, limits, a_assertContext));
    require(reloaded.operation_id() == bundle.operation_id());
    require(reloaded.state() == bundle.state());
    require(reloaded.manifest_entries().size() == bundle.manifest_entries().size());

    cue::BuildDiagnosticBundleLimits smallLimits = limits;
    smallLimits.maximumFileBytes = 32U;
    smallLimits.maximumTotalBytes = 512U;
    require(!cue::create_build_diagnostic_bundle(input, smallLimits, a_assertContext).has_value());
    require(!cue::read_build_diagnostic_bundle_directory(destinationUtf8, smallLimits, a_assertContext).has_value());

    const std::filesystem::path unexpectedDirectory = destination / "unexpected";
    require(std::filesystem::create_directory(unexpectedDirectory));
    require(!cue::read_build_diagnostic_bundle_directory(destinationUtf8, limits, a_assertContext).has_value());
    require(std::filesystem::remove(unexpectedDirectory));

    const auto manifestFile =
        std::find_if(bundle.files().begin(), bundle.files().end(),
                     /// @brief Tamper検証対象のManifest Fileを検出する
                     [](const auto &a_file) noexcept { return a_file.relativePath == "manifest.json"; });
    require(manifestFile != bundle.files().end());
    std::string tamperedManifest;
    for (const std::byte value : manifestFile->bytes)
    {
        tamperedManifest.push_back(static_cast<char>(std::to_integer<unsigned char>(value)));
    }
    const std::size_t entryBegin = tamperedManifest.find("{\"path\":");
    const std::size_t entryEnd = tamperedManifest.find('\n', entryBegin);
    require(entryBegin != std::string::npos && entryEnd != std::string::npos);
    tamperedManifest.insert(entryEnd + 1U, tamperedManifest.substr(entryBegin, entryEnd - entryBegin + 1U));
    std::ofstream manifestStream(destination / "manifest.json", std::ios::binary | std::ios::trunc);
    manifestStream.write(tamperedManifest.data(), static_cast<std::streamsize>(tamperedManifest.size()));
    manifestStream.close();
    require(manifestStream.good());
    require(!cue::read_build_diagnostic_bundle_directory(destinationUtf8, limits, a_assertContext).has_value());

    input.environment.reset();
    input.operation.latestSuccessfulArtifact.reset();
    cue::BuildDiagnosticBundle missing =
        take_value(cue::create_build_diagnostic_bundle(input, limits, a_assertContext));
    require(!find_entry(missing, "environment.json")->collected);
    require(!find_entry(missing, "environment.json")->missingReason.empty());
    require(!find_entry(missing, "artifact.json")->collected);
    require(!find_entry(missing, "artifact.json")->missingReason.empty());

    std::filesystem::remove_all(destination, cleanupError);
    require(!cleanupError);
    std::filesystem::remove_all(projectRoot, cleanupError);
    require(!cleanupError);
}
} // namespace

/// @brief Build Diagnostic Bundleの安全な生成、保存、再読込契約を検証する
int main(int a_argumentCount, char **a_arguments)
{
    require(a_argumentCount == 2);
    TestFatalHandler fatalHandler;
    std::vector<std::unique_ptr<cue::LogSink>> sinks;
    cue::Logger logger(fatalHandler, std::move(sinks));
    cue::AssertContext assertContext(logger, fatalHandler);
    test_diagnostic_bundle(a_arguments[1], assertContext);
    return 0;
}
