#pragma once

#include <Cue/Build/Plan.h>
#include <Cue/Foundation/Result.h>

#include <cstdint>
#include <string>
#include <vector>

namespace cue
{
class AssertContext;

/// @brief Windows Authenticode検証で観測した署名状態
enum class WindowsProductSignatureStatus : std::uint8_t
{
    Trusted,
    Unsigned,
    InvalidSignature,
    CertificateExpired,
    CertificateRevoked,
    ChainInvalid,
    VerificationUnavailable
};

/// @brief Shipping Product単体のTrust到達点
enum class WindowsProductDistributionStatus : std::uint8_t
{
    LocalExecutionOnly,
    PublisherVerifiedArtifact
};

/// @brief Windows Trust Providerから得た署名状態とPublisher Public Key Identity
struct WindowsProductTrustEvidence final
{
    WindowsProductSignatureStatus signatureStatus = WindowsProductSignatureStatus::VerificationUnavailable;
    std::string publisherKeyId;
};

/// @brief 最終PE検証で確定したImport、署名、配布到達点
struct WindowsProductSecurityValidation final
{
    std::uint64_t byteSize = 0U;
    std::string contentHash;
    std::vector<std::string> importedLibraries;
    WindowsProductTrustEvidence trustEvidence;
    WindowsProductDistributionStatus distributionStatus = WindowsProductDistributionStatus::LocalExecutionOnly;
};

/// @brief Product FileのAuthenticode状態とSigner Public Key Identityを検査する
///
/// PathとAssertContextは呼出中だけ借用し、返却EvidenceがPublisher Key IDを所有する。署名なしと検証不能を区別し、
/// Windows Trust PolicyがTrustedと判定した場合だけDER SubjectPublicKeyInfoのlowercase SHA-256を返す。
/// 失効情報はLocal Cacheだけを参照し、Online Revocationを行わないため、この結果単体をPublicDistributionReadyの
/// Evidenceにはしない。Fileは検査完了までWrite／Delete共有なしで保持する。共有状態を変更しないため別Fileから同時に呼べる。
[[nodiscard]] Result<WindowsProductTrustEvidence> inspect_windows_product_trust(
    std::string a_absoluteProductPath, const AssertContext &a_assertContext) noexcept;

/// @brief Build Profileと署名EvidenceからProduct ArtifactのTrust到達点をFail-closedで決定する
///
/// UnsignedLocalは署名状態にかかわらずLocalExecutionOnlyとし、Public配布可能とは扱わない。PublisherSignedは
/// Windows Trust成功と期待Publisher Key IDの一致を必須とする。入力は呼出中だけ借用し、共有状態を変更しない。
[[nodiscard]] Result<WindowsProductDistributionStatus> evaluate_windows_product_trust_policy(
    const BuildProfile &a_profile, const WindowsProductTrustEvidence &a_evidence,
    const AssertContext &a_assertContext) noexcept;

/// @brief 最終Shipping ProductのPE Hardening、Import、Authenticode Policyを機械検証する
///
/// x64 Executable、ASLR、High Entropy VA、DEP、CFG、CET、Stack Cookie、System32限定Dependent Load、
/// Version付きImport Allowlist、Game Module Loader不在を検証する。PublisherSignedでは同じRead Handleを用いて
/// Windows TrustとPublisher Identityも検証する。PathとProfileは呼出中だけ借用し、共有状態を変更しない。
/// 成功時は同じRead Handleから計算したSize／SHA-256、正規化済みDirect Import一覧、観測した署名Evidence、
/// Artifact単体の配布到達点を返す。Size／HashはSecurity Evidenceを同一Byte Snapshotへ結び付ける。
/// PublicDistributionReadyは外部署名、Online Revocation、Manifest署名、外部Trust Anchorを別途要求する。
[[nodiscard]] Result<WindowsProductSecurityValidation> validate_windows_shipping_product_security(
    std::string a_absoluteProductPath, const BuildProfile &a_profile, const AssertContext &a_assertContext) noexcept;
} // namespace cue
