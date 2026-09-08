#pragma once

#include <Cue/Build/Service.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cue
{
class AssertContext;

/// @brief Build Diagnostic Bundle生成、保存、再読込の安定した失敗分類
enum class BuildDiagnosticBundleError : std::int64_t
{
    InvalidInput = 1,
    InvalidLimits,
    FileCountLimitExceeded,
    FileSizeLimitExceeded,
    TotalSizeLimitExceeded,
    DestinationAlreadyExists,
    FilesystemFailure,
    InvalidBundle
};

/// @brief Diagnostic BundleのFile数、Byte数、Path Mapping数を制限するPolicy
struct BuildDiagnosticBundleLimits final
{
    std::size_t maximumFileCount = 16U;
    std::uint64_t maximumFileBytes = 4U * 1024U * 1024U;
    std::uint64_t maximumTotalBytes = 16U * 1024U * 1024U;
    std::size_t maximumPathMappings = 256U;
};

/// @brief LogとMetadata内のSensitive Native PathをTokenへ置換する一件の規則
struct BuildDiagnosticPathMapping final
{
    std::string nativePrefix;
    std::string replacement;
};

/// @brief Bundleへ保存する検証済みBuild Planの非Source値Snapshot
struct BuildDiagnosticPlanSnapshot final
{
    std::string projectRoot;
    std::string presetName;
    std::string workspaceKey;
    std::string binaryDirectory;
    std::string candidateDirectory;
    std::string operationDirectory;
    std::string artifactStoreDirectory;
    std::string targetName;
};

/// @brief 一操作の診断に必要なPlan、Environment、Stage、Log、Artifact入力
struct BuildDiagnosticBundleInput final
{
    BuildOperationSnapshot operation;
    BuildDiagnosticPlanSnapshot plan;
    std::optional<BuildEnvironmentReport> environment;
    std::vector<BuildDiagnosticPathMapping> pathMappings;
};

/// @brief Bundle内の一Fileを相対Pathと未変換Byte列で所有する
struct BuildDiagnosticBundleFile final
{
    std::string relativePath;
    std::vector<std::byte> bytes;
};

/// @brief Manifestが示す収集Fileまたは欠損理由
struct BuildDiagnosticManifestEntry final
{
    std::string relativePath;
    bool collected = false;
    std::uint64_t byteSize = 0U;
    std::string missingReason;
};

/// @brief ManifestとBounded File集合を持つ再表示可能なDiagnostic Bundle
///
/// Accessorが返すspanとstring_viewはこのBundleの寿命を超えて保持しない。
class BuildDiagnosticBundle final
{
  public:
    /// @brief ManifestとFileを持たない無効Bundleの生成を禁止する
    BuildDiagnosticBundle() = delete;

    /// @brief Bundleのlowercase UUID Version 4 Operation IDを返す
    [[nodiscard]] std::string_view operation_id() const noexcept;
    /// @brief Bundle対象Buildの完了Stateを返す
    [[nodiscard]] GameBuildOperationState state() const noexcept;
    /// @brief 収集項目と欠損理由を固定順で返す
    [[nodiscard]] std::span<const BuildDiagnosticManifestEntry> manifest_entries() const noexcept;
    /// @brief manifest.jsonを含む保存対象FileをPath昇順で返す
    [[nodiscard]] std::span<const BuildDiagnosticBundleFile> files() const noexcept;

  private:
    friend Result<BuildDiagnosticBundle> create_build_diagnostic_bundle(const BuildDiagnosticBundleInput &,
                                                                        const BuildDiagnosticBundleLimits &,
                                                                        const AssertContext &) noexcept;
    friend Result<BuildDiagnosticBundle> read_build_diagnostic_bundle_directory(std::string_view,
                                                                                const BuildDiagnosticBundleLimits &,
                                                                                const AssertContext &) noexcept;

    /// @brief 検証済みManifestとFile集合の所有権からBundleを構築する
    BuildDiagnosticBundle(std::string a_operationId, GameBuildOperationState a_state,
                          std::vector<BuildDiagnosticManifestEntry> a_entries,
                          std::vector<BuildDiagnosticBundleFile> a_files) noexcept;

    std::string m_operationId;
    GameBuildOperationState m_state;
    std::vector<BuildDiagnosticManifestEntry> m_entries;
    std::vector<BuildDiagnosticBundleFile> m_files;
};

/// @brief BuildPlanからSource本文を含まないBundle用Snapshotを作成する
///
/// PlanとAssertContextは呼出中だけ借用し、返却Snapshotが全文字列を所有する。共有状態を変更しないため別入力から同時に呼べる。
/// Allocation等の予期しない例外はFatalHandlerへ渡す。
[[nodiscard]] BuildDiagnosticPlanSnapshot make_build_diagnostic_plan_snapshot(
    const BuildPlan &a_plan, const AssertContext &a_assertContext) noexcept;

/// @brief Sensitive Pathを置換し、SourceとCredentialを含まないBounded Bundleを生成する
///
/// Input、Limits、AssertContextは呼出中だけ借用し、返却Bundleが全FileとManifestを所有する。入力不整合、上限不正、
/// File数／File Size／Total Size超過は対応するBuildDiagnosticBundleErrorを返す。共有状態を変更しないため別入力から
/// 同時に呼べる。Allocation等の予期しない例外はFatalHandlerへ渡す。
[[nodiscard]] Result<BuildDiagnosticBundle> create_build_diagnostic_bundle(
    const BuildDiagnosticBundleInput &a_input, const BuildDiagnosticBundleLimits &a_limits,
    const AssertContext &a_assertContext) noexcept;

/// @brief 新規Absolute DirectoryへBundle Fileを保存し、既存Destinationは変更しない
///
/// Bundle、Destination、AssertContextは呼出中だけ借用する。同じDestinationへの並行Writeは行わない。既存Destination、
/// 不正Bundle、Filesystem失敗を安定Errorとして返し、部分生成Directoryを成功として扱わない。予期しない例外は
/// FatalHandlerへ渡す。
[[nodiscard]] Result<void> write_build_diagnostic_bundle_directory(const BuildDiagnosticBundle &a_bundle,
                                                                   std::string_view a_destination,
                                                                   const AssertContext &a_assertContext) noexcept;

/// @brief Directoryを上限付きで再読込し、ManifestとFile集合の一致を検証する
///
/// Source、Limits、AssertContextは呼出中だけ借用し、返却Bundleが読込Byte列を所有する。読込中にSourceを変更しない場合、
/// 別Directoryを同時に読める。上限不正、Filesystem失敗、未知File、Reparse Point、Manifest不一致、各上限超過を
/// 対応するBuildDiagnosticBundleErrorとして返す。予期しない例外はFatalHandlerへ渡す。
[[nodiscard]] Result<BuildDiagnosticBundle> read_build_diagnostic_bundle_directory(
    std::string_view a_source, const BuildDiagnosticBundleLimits &a_limits,
    const AssertContext &a_assertContext) noexcept;
} // namespace cue
