#include <Cue/Build/Toolchain.h>

#include <Cue/Foundation/Assert.h>

#include <array>
#include <cstdlib>
#include <utility>

namespace
{
/// @brief Build検証中の予期しない例外をFatal境界へ渡す
[[noreturn]] void terminate_build_exception(const cue::AssertContext &a_assertContext) noexcept
{
    a_assertContext.fatal_handler().terminate("Build environment validation failed unexpectedly");
    std::abort();
}

/// @brief Candidate Versionが半開範囲に含まれるか判定する
[[nodiscard]] bool supports_version(const cue::BuildToolCandidate &a_candidate,
                                    const cue::BuildToolRequirement &a_requirement) noexcept
{
    return a_candidate.version.has_value() && *a_candidate.version >= a_requirement.minimumVersion &&
           *a_candidate.version < a_requirement.maximumVersionExclusive;
}

/// @brief Report全体の状態をUnknown優先で悪化させる
void combine_support(cue::BuildEnvironmentReport &a_report, cue::BuildEnvironmentSupport a_support) noexcept
{
    if (a_support == cue::BuildEnvironmentSupport::Unknown ||
        (a_support == cue::BuildEnvironmentSupport::Unsupported &&
         a_report.support == cue::BuildEnvironmentSupport::Supported))
    {
        a_report.support = a_support;
    }
}

/// @brief 一件の診断を追加してReport全体の状態へ反映する
void add_diagnostic(cue::BuildEnvironmentReport &a_report, cue::BuildEnvironmentDiagnostic &&a_diagnostic)
{
    combine_support(a_report, a_diagnostic.support);
    a_report.diagnostics.push_back(std::move(a_diagnostic));
}

/// @brief Tool Kindに対応するUser表示名を返す
[[nodiscard]] std::string_view tool_name(cue::BuildToolKind a_kind) noexcept
{
    switch (a_kind)
    {
    case cue::BuildToolKind::CMake:
        return "CMake";
    case cue::BuildToolKind::VisualStudio:
        return "Visual Studio";
    case cue::BuildToolKind::MsvcCompiler:
        return "MSVC compiler";
    case cue::BuildToolKind::WindowsSdk:
        return "Windows SDK";
    }
    return "Build tool";
}
} // namespace

namespace cue
{
BuildEnvironmentReport validate_build_environment(const BuildEnvironmentInventory &a_inventory,
                                                  const BuildEnvironmentRequirements &a_requirements,
                                                  const AssertContext &a_assertContext) noexcept
{
    try
    {
        BuildEnvironmentReport report;
        report.support = BuildEnvironmentSupport::Supported;
        report.engineSourceRoot = a_inventory.engineSourceRoot;
        report.engineBinaryRoot = a_inventory.engineBinaryRoot;
        report.supportedConfigurations = a_requirements.supportedConfigurations;
        report.selectedTools.reserve(a_requirements.tools.size());
        report.diagnostics.reserve(a_requirements.tools.size() + 3U);

        if (a_inventory.hostArchitecture != a_requirements.hostArchitecture)
        {
            add_diagnostic(report, {BuildEnvironmentDiagnosticCode::UnsupportedHostArchitecture,
                                    BuildEnvironmentSupport::Unsupported,
                                    std::nullopt,
                                    {},
                                    "Host architecture is not supported",
                                    "Run the x64 CueEditor on a Windows x64 host"});
        }

        for (const BuildToolRequirement &requirement : a_requirements.tools)
        {
            std::vector<const BuildToolCandidate *> compatible;
            compatible.reserve(a_inventory.candidates.size());
            bool found = false;
            bool unknownIdentity = false;
            std::string unknownPath;
            std::string observedPath;
            for (const BuildToolCandidate &candidate : a_inventory.candidates)
            {
                if (candidate.kind != requirement.kind)
                {
                    continue;
                }
                found = true;
                if (observedPath.empty())
                {
                    observedPath = candidate.nativePath;
                }
                if (!candidate.available || !candidate.version.has_value() ||
                    candidate.architecture == BuildArchitecture::Unknown)
                {
                    unknownIdentity = true;
                    if (unknownPath.empty())
                    {
                        unknownPath = candidate.nativePath;
                    }
                    continue;
                }
                if (candidate.architecture == requirement.architecture && supports_version(candidate, requirement))
                {
                    compatible.push_back(&candidate);
                }
            }

            if (compatible.size() == 1U && !unknownIdentity)
            {
                report.selectedTools.push_back(*compatible.front());
                continue;
            }
            if (compatible.size() > 1U || (compatible.size() == 1U && unknownIdentity))
            {
                add_diagnostic(report, {BuildEnvironmentDiagnosticCode::AmbiguousTool, BuildEnvironmentSupport::Unknown,
                                        requirement.kind, unknownPath,
                                        std::string(tool_name(requirement.kind)) + " has multiple viable candidates",
                                        "Select one compatible installation in the Editor workspace settings"});
                continue;
            }
            if (!found)
            {
                add_diagnostic(report, {BuildEnvironmentDiagnosticCode::MissingTool,
                                        BuildEnvironmentSupport::Unknown,
                                        requirement.kind,
                                        {},
                                        std::string(tool_name(requirement.kind)) + " was not found",
                                        "Install the required tool or repair the Engine toolchain configuration"});
                continue;
            }
            if (unknownIdentity)
            {
                add_diagnostic(report, {BuildEnvironmentDiagnosticCode::UnknownToolIdentity,
                                        BuildEnvironmentSupport::Unknown, requirement.kind, std::move(unknownPath),
                                        std::string(tool_name(requirement.kind)) + " identity could not be verified",
                                        "Repair the installation or select a readable executable"});
                continue;
            }
            add_diagnostic(report,
                           {BuildEnvironmentDiagnosticCode::UnsupportedTool, BuildEnvironmentSupport::Unsupported,
                            requirement.kind, std::move(observedPath),
                            std::string(tool_name(requirement.kind)) + " version or architecture is unsupported",
                            "Install a version compatible with this CueEngine build"});
        }

        if (!a_inventory.engineSourceAvailable)
        {
            add_diagnostic(report,
                           {BuildEnvironmentDiagnosticCode::MissingEngineSource, BuildEnvironmentSupport::Unknown,
                            std::nullopt, a_inventory.engineSourceRoot, "CueEngine source root is unavailable",
                            "Select a CueEngine checkout containing Engine/Source/GameModule"});
        }
        if (!a_inventory.engineBinaryAvailable)
        {
            add_diagnostic(report,
                           {BuildEnvironmentDiagnosticCode::MissingEngineBinary, BuildEnvironmentSupport::Unknown,
                            std::nullopt, a_inventory.engineBinaryRoot, "CueEngine binary root is unavailable",
                            "Configure and build CueEngine before building the game project"});
        }
        return report;
    }
    catch (...)
    {
        terminate_build_exception(a_assertContext);
    }
}

std::string format_native_path_for_log(std::string_view a_nativePath, const AssertContext &a_assertContext) noexcept
{
    try
    {
        constexpr std::array<char, 16U> digits = {'0', '1', '2', '3', '4', '5', '6', '7',
                                                  '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};
        std::string formatted;
        formatted.reserve(a_nativePath.size() + 2U);
        formatted.push_back('"');
        for (const unsigned char value : a_nativePath)
        {
            switch (value)
            {
            case '\\':
                formatted.append("\\\\");
                break;
            case '"':
                formatted.append("\\\"");
                break;
            case '\n':
                formatted.append("\\n");
                break;
            case '\r':
                formatted.append("\\r");
                break;
            case '\t':
                formatted.append("\\t");
                break;
            default:
                if (value < 0x20U || value == 0x7FU)
                {
                    formatted.append("\\x");
                    formatted.push_back(digits[(value >> 4U) & 0x0FU]);
                    formatted.push_back(digits[value & 0x0FU]);
                }
                else
                {
                    formatted.push_back(static_cast<char>(value));
                }
                break;
            }
        }
        formatted.push_back('"');
        return formatted;
    }
    catch (...)
    {
        terminate_build_exception(a_assertContext);
    }
}

std::string format_command_line_for_log(std::span<const std::string_view> a_arguments,
                                        const AssertContext &a_assertContext) noexcept
{
    try
    {
        std::string formatted;
        for (const std::string_view argument : a_arguments)
        {
            if (!formatted.empty())
            {
                formatted.push_back(' ');
            }
            formatted.append(format_native_path_for_log(argument, a_assertContext));
        }
        return formatted;
    }
    catch (...)
    {
        terminate_build_exception(a_assertContext);
    }
}
} // namespace cue
