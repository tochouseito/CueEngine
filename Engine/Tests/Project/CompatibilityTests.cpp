#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Fatal.h>
#include <Cue/Foundation/Log.h>
#include <Cue/Project/Compatibility.h>
#include <Cue/Project/Error.h>

#include <array>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
class TestFatalHandler final : public cue::FatalHandler
{
  public:
    /// @brief Test中の回復不能失敗を即座に終了Codeへ変換する
    [[noreturn]] void terminate() noexcept override
    {
        std::_Exit(90);
    }

    /// @brief Message付き回復不能失敗を即座に終了Codeへ変換する
    [[noreturn]] void terminate(std::string_view) noexcept override
    {
        std::_Exit(91);
    }
};

/// @brief ResultのProject Error分類が期待値と一致するか判定する
template <typename Value>
[[nodiscard]] bool has_project_error(const cue::Result<Value> &a_result, cue::ProjectError a_error) noexcept
{
    return !a_result && a_result.try_error()->code().domain() == "Cue.Project" &&
           a_result.try_error()->code().value() == static_cast<std::int64_t>(a_error);
}

/// @brief Reportが指定理由をCapability単位で保持するか判定する
[[nodiscard]] bool has_reason(const cue::ProjectCompatibilityReport &a_report,
                              cue::ProjectCompatibilityReasonCode a_code,
                              std::optional<cue::ProjectCapability> a_capability,
                              std::optional<cue::CapabilityVersion> a_minimumVersion = std::nullopt) noexcept
{
    for (const cue::ProjectCompatibilityReason &reason : a_report.reasons())
    {
        if (reason.code == a_code && reason.capability == a_capability && reason.minimumVersion == a_minimumVersion)
        {
            return true;
        }
    }
    return false;
}

/// @brief 一要件と一観測を使って互換性EvaluatorをSynthetic実行する
[[nodiscard]] cue::Result<cue::ProjectCompatibilityReport> evaluate_single(
    const cue::ProjectCapabilityRequirement &a_requirement, const cue::ProjectCapabilityObservation &a_observation,
    const cue::AssertContext &a_assertContext) noexcept
{
    auto profile = cue::ProjectCapabilityProfile::create(std::span(&a_requirement, 1U), a_assertContext);
    auto snapshot = cue::ProjectCapabilitySnapshot::create(std::span(&a_observation, 1U), a_assertContext);
    if (!profile)
    {
        return cue::Result<cue::ProjectCompatibilityReport>::failure(std::move(*profile.try_error()));
    }
    if (!snapshot)
    {
        return cue::Result<cue::ProjectCompatibilityReport>::failure(std::move(*snapshot.try_error()));
    }
    return cue::evaluate_project_compatibility(
        1U, 1U, cue::EngineCompatibility{cue::EngineVersion{1U, 0U, 0U}, cue::EngineVersion{2U, 0U, 0U}},
        cue::EngineVersion{1U, 5U, 0U}, *profile.try_value(), *snapshot.try_value(), a_assertContext);
}

/// @brief 対応済み必須CapabilityがCompatibleかつRuntime有効になることを検証する
[[nodiscard]] bool test_compatible(const cue::AssertContext &a_assertContext)
{
    const cue::ProjectCapabilityRequirement requirement = {
        cue::ProjectCapability::Baseline3D,
        cue::CapabilityRequirementKind::Required,
        cue::CapabilityVersion{1U, 0U},
    };
    const cue::ProjectCapabilityObservation observation = {
        cue::ProjectCapability::Baseline3D,
        cue::CapabilityState::supported_enabled(),
        cue::CapabilityVersion{1U, 1U},
    };
    auto report = evaluate_single(requirement, observation, a_assertContext);
    return report && report.try_value()->status() == cue::ProjectCompatibilityStatus::Compatible &&
           report.try_value()->can_open() && report.try_value()->reasons().empty() &&
           report.try_value()->runtime_decisions().size() == 1U && report.try_value()->runtime_decisions()[0].isEnabled;
}

/// @brief Hardware未対応とEngine未実装を異なる理由でUnsupportedへ分類することを検証する
[[nodiscard]] bool test_supported_and_implemented_are_separate(const cue::AssertContext &a_assertContext)
{
    const cue::ProjectCapabilityRequirement requirement = {
        cue::ProjectCapability::RayTracing,
        cue::CapabilityRequirementKind::Required,
        std::nullopt,
    };
    const cue::ProjectCapabilityObservation unsupported = {
        cue::ProjectCapability::RayTracing,
        cue::CapabilityState::unsupported_implemented(),
        std::nullopt,
    };
    const cue::ProjectCapabilityObservation notImplemented = {
        cue::ProjectCapability::RayTracing,
        cue::CapabilityState::supported_not_implemented(),
        std::nullopt,
    };
    auto unsupportedReport = evaluate_single(requirement, unsupported, a_assertContext);
    auto implementationReport = evaluate_single(requirement, notImplemented, a_assertContext);
    return unsupportedReport && implementationReport &&
           unsupportedReport.try_value()->status() == cue::ProjectCompatibilityStatus::Unsupported &&
           implementationReport.try_value()->status() == cue::ProjectCompatibilityStatus::Unsupported &&
           has_reason(*unsupportedReport.try_value(), cue::ProjectCompatibilityReasonCode::HardwareUnsupported,
                      cue::ProjectCapability::RayTracing) &&
           has_reason(*implementationReport.try_value(), cue::ProjectCompatibilityReasonCode::EngineNotImplemented,
                      cue::ProjectCapability::RayTracing);
}

/// @brief Query失敗をUnsupportedへ潰さずUnknownと固有理由で返すことを検証する
[[nodiscard]] bool test_unknown_is_preserved(const cue::AssertContext &a_assertContext)
{
    const cue::ProjectCapabilityRequirement requirement = {
        cue::ProjectCapability::WaveOperations,
        cue::CapabilityRequirementKind::Required,
        std::nullopt,
    };
    const cue::ProjectCapabilityObservation observation = {
        cue::ProjectCapability::WaveOperations,
        cue::CapabilityState::query_failed_implemented(),
        std::nullopt,
    };
    auto report = evaluate_single(requirement, observation, a_assertContext);
    return report && report.try_value()->status() == cue::ProjectCompatibilityStatus::Unknown &&
           report.try_value()->can_open() &&
           has_reason(*report.try_value(), cue::ProjectCompatibilityReasonCode::CapabilityQueryFailed,
                      cue::ProjectCapability::WaveOperations) &&
           !report.try_value()->runtime_decisions()[0].isEnabled;
}

/// @brief Preferred未達とRuntime無効をProject Open拒否ではなくDegradedへ分類することを検証する
[[nodiscard]] bool test_degraded_and_open_separation(const cue::AssertContext &a_assertContext)
{
    const cue::ProjectCapabilityRequirement preferred = {
        cue::ProjectCapability::MeshShader,
        cue::CapabilityRequirementKind::Preferred,
        std::nullopt,
    };
    const cue::ProjectCapabilityObservation unsupported = {
        cue::ProjectCapability::MeshShader,
        cue::CapabilityState::unsupported_not_implemented(),
        std::nullopt,
    };
    auto preferredReport = evaluate_single(preferred, unsupported, a_assertContext);

    const cue::ProjectCapabilityRequirement required = {
        cue::ProjectCapability::EnhancedBarriers,
        cue::CapabilityRequirementKind::Required,
        std::nullopt,
    };
    const cue::ProjectCapabilityObservation disabled = {
        cue::ProjectCapability::EnhancedBarriers,
        cue::CapabilityState::supported_disabled(),
        std::nullopt,
    };
    auto disabledReport = evaluate_single(required, disabled, a_assertContext);
    return preferredReport && disabledReport &&
           preferredReport.try_value()->status() == cue::ProjectCompatibilityStatus::Degraded &&
           preferredReport.try_value()->can_open() &&
           disabledReport.try_value()->status() == cue::ProjectCompatibilityStatus::Degraded &&
           disabledReport.try_value()->can_open() && !disabledReport.try_value()->runtime_decisions()[0].isEnabled &&
           has_reason(*disabledReport.try_value(), cue::ProjectCompatibilityReasonCode::RuntimeDisabled,
                      cue::ProjectCapability::EnhancedBarriers);
}

/// @brief FormatとEngine VersionだけがProject Open可否を拒否することを検証する
[[nodiscard]] bool test_open_eligibility(const cue::AssertContext &a_assertContext)
{
    auto profile = cue::ProjectCapabilityProfile::create({}, a_assertContext);
    auto snapshot = cue::ProjectCapabilitySnapshot::create({}, a_assertContext);
    if (!profile || !snapshot)
    {
        return false;
    }
    auto formatReport = cue::evaluate_project_compatibility(
        2U, 1U, cue::EngineCompatibility{cue::EngineVersion{1U, 0U, 0U}, std::nullopt}, cue::EngineVersion{1U, 0U, 0U},
        *profile.try_value(), *snapshot.try_value(), a_assertContext);
    auto engineReport = cue::evaluate_project_compatibility(
        1U, 1U, cue::EngineCompatibility{cue::EngineVersion{2U, 0U, 0U}, cue::EngineVersion{3U, 0U, 0U}},
        cue::EngineVersion{1U, 9U, 0U}, *profile.try_value(), *snapshot.try_value(), a_assertContext);
    return formatReport && engineReport && !formatReport.try_value()->can_open() &&
           !engineReport.try_value()->can_open() &&
           has_reason(*formatReport.try_value(), cue::ProjectCompatibilityReasonCode::UnsupportedProjectFormat,
                      std::nullopt) &&
           has_reason(*engineReport.try_value(), cue::ProjectCompatibilityReasonCode::EngineVersionTooOld,
                      std::nullopt);
}

/// @brief Version不足とVersion不明をUnsupportedとUnknownへ分けることを検証する
[[nodiscard]] bool test_capability_versions(const cue::AssertContext &a_assertContext)
{
    const cue::ProjectCapabilityRequirement requirement = {
        cue::ProjectCapability::VariableRateShading,
        cue::CapabilityRequirementKind::Required,
        cue::CapabilityVersion{2U, 0U},
    };
    const cue::ProjectCapabilityObservation low = {
        cue::ProjectCapability::VariableRateShading,
        cue::CapabilityState::supported_enabled(),
        cue::CapabilityVersion{1U, 0U},
    };
    const cue::ProjectCapabilityObservation unknown = {
        cue::ProjectCapability::VariableRateShading,
        cue::CapabilityState::supported_enabled(),
        std::nullopt,
    };
    auto lowReport = evaluate_single(requirement, low, a_assertContext);
    auto unknownReport = evaluate_single(requirement, unknown, a_assertContext);
    return lowReport && unknownReport &&
           lowReport.try_value()->status() == cue::ProjectCompatibilityStatus::Unsupported &&
           unknownReport.try_value()->status() == cue::ProjectCompatibilityStatus::Unknown &&
           has_reason(*lowReport.try_value(), cue::ProjectCompatibilityReasonCode::CapabilityVersionTooLow,
                      cue::ProjectCapability::VariableRateShading, cue::CapabilityVersion{2U, 0U}) &&
           has_reason(*unknownReport.try_value(), cue::ProjectCompatibilityReasonCode::CapabilityVersionUnknown,
                      cue::ProjectCapability::VariableRateShading, cue::CapabilityVersion{2U, 0U});
}

/// @brief ProfileとSnapshotの重複Capabilityを評価前に拒否することを検証する
[[nodiscard]] bool test_duplicate_inputs(const cue::AssertContext &a_assertContext)
{
    constexpr std::array requirements = {
        cue::ProjectCapabilityRequirement{cue::ProjectCapability::Baseline3D, cue::CapabilityRequirementKind::Required,
                                          std::nullopt},
        cue::ProjectCapabilityRequirement{cue::ProjectCapability::Baseline3D, cue::CapabilityRequirementKind::Preferred,
                                          std::nullopt},
    };
    const std::array observations = {
        cue::ProjectCapabilityObservation{cue::ProjectCapability::Baseline3D, cue::CapabilityState::supported_enabled(),
                                          std::nullopt},
        cue::ProjectCapabilityObservation{cue::ProjectCapability::Baseline3D,
                                          cue::CapabilityState::supported_disabled(), std::nullopt},
    };
    auto profile = cue::ProjectCapabilityProfile::create(requirements, a_assertContext);
    auto snapshot = cue::ProjectCapabilitySnapshot::create(observations, a_assertContext);
    return has_project_error(profile, cue::ProjectError::InvalidCompatibilityInput) &&
           has_project_error(snapshot, cue::ProjectError::InvalidCompatibilityInput);
}

/// @brief 現在環境のHardware・実装・Enablement観測値を共有Descriptorへ保存しないことを検証する
[[nodiscard]] bool test_snapshot_is_not_project_data(const cue::AssertContext &a_assertContext)
{
    auto projectId = cue::ProjectId::parse("12345678-1234-4abc-8def-1234567890ab", a_assertContext);
    if (!projectId)
    {
        return false;
    }
    auto descriptor =
        cue::create_blank_project_descriptor(*projectId.try_value(), "Compatibility Test",
                                             cue::EngineCompatibility{cue::EngineVersion{1U, 0U, 0U}, std::nullopt},
                                             "00000000-0000-4000-8000-000000000099", a_assertContext);
    auto serialized = descriptor ? cue::serialize_project_descriptor(*descriptor.try_value(), a_assertContext)
                                 : cue::Result<std::string>::failure(std::move(*descriptor.try_error()));
    return serialized && serialized.try_value()->find("\"requiredCapabilities\":[]") != std::string::npos &&
           serialized.try_value()->find("hardware") == std::string::npos &&
           serialized.try_value()->find("implementation") == std::string::npos &&
           serialized.try_value()->find("enablement") == std::string::npos;
}

/// @brief 不正なFormat Versionと空のEngine範囲を診断結果へ混ぜず入力Errorとして拒否する
[[nodiscard]] bool test_invalid_version_inputs(const cue::AssertContext &a_assertContext)
{
    auto profile = cue::ProjectCapabilityProfile::create({}, a_assertContext);
    auto snapshot = cue::ProjectCapabilitySnapshot::create({}, a_assertContext);
    if (!profile || !snapshot)
    {
        return false;
    }
    auto invalidFormat = cue::evaluate_project_compatibility(
        0U, 1U, cue::EngineCompatibility{cue::EngineVersion{1U, 0U, 0U}, std::nullopt}, cue::EngineVersion{1U, 0U, 0U},
        *profile.try_value(), *snapshot.try_value(), a_assertContext);
    auto invalidRange = cue::evaluate_project_compatibility(
        1U, 1U, cue::EngineCompatibility{cue::EngineVersion{1U, 0U, 0U}, cue::EngineVersion{1U, 0U, 0U}},
        cue::EngineVersion{1U, 0U, 0U}, *profile.try_value(), *snapshot.try_value(), a_assertContext);
    return has_project_error(invalidFormat, cue::ProjectError::InvalidCompatibilityInput) &&
           has_project_error(invalidRange, cue::ProjectError::InvalidCompatibilityInput);
}
} // namespace

/// @brief Project互換性契約の全Synthetic状態を終了Codeで検証する
int main()
{
    TestFatalHandler fatalHandler;
    std::vector<std::unique_ptr<cue::LogSink>> sinks;
    cue::Logger logger(fatalHandler, std::move(sinks));
    cue::AssertContext assertContext(logger, fatalHandler);

    if (!test_compatible(assertContext))
    {
        return 1;
    }
    if (!test_supported_and_implemented_are_separate(assertContext))
    {
        return 2;
    }
    if (!test_unknown_is_preserved(assertContext))
    {
        return 3;
    }
    if (!test_degraded_and_open_separation(assertContext))
    {
        return 4;
    }
    if (!test_open_eligibility(assertContext))
    {
        return 5;
    }
    if (!test_capability_versions(assertContext))
    {
        return 6;
    }
    if (!test_duplicate_inputs(assertContext))
    {
        return 7;
    }
    if (!test_snapshot_is_not_project_data(assertContext))
    {
        return 8;
    }
    return test_invalid_version_inputs(assertContext) ? 0 : 9;
}
