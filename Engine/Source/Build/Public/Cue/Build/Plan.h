#pragma once

#include <Cue/Build/Toolchain.h>
#include <Cue/Foundation/Result.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace cue
{
class AssertContext;

/// @brief Project Buildが生成するArtifact種別
enum class BuildTarget : std::uint8_t
{
    GameModule,
    ShippingProduct
};

/// @brief Shipping Productが要求する最小Trust Policy
enum class ShippingTrustMode : std::uint8_t
{
    UnsignedLocal,
    PublisherSigned
};

/// @brief M15 Project Buildが使用するCMake Generator
enum class BuildGenerator : std::uint8_t
{
    VisualStudio2026
};

/// @brief CMake Binary Treeを再利用できる互換入力
struct BuildWorkspaceCompatibility final
{
    BuildGenerator generator = BuildGenerator::VisualStudio2026;
    BuildArchitecture architecture = BuildArchitecture::Unknown;
    BuildToolVersion toolsetVersion;
    std::uint32_t engineBuildPolicyVersion = 0U;

    /// @brief 全互換入力が一致するか比較する
    [[nodiscard]] bool operator==(const BuildWorkspaceCompatibility &) const noexcept = default;
};

/// @brief Build ProfileまたはPlan検証の安定した失敗分類
enum class BuildPlanError : std::int64_t
{
    InvalidProfile = 1,
    InvalidProjectRoot,
    InvalidOperationId,
    UnsafeOutputPath,
    InvalidStageResult
};

/// @brief 再利用可能なConfiguration、Target、Trust Identityを保持する永続Build Profile
class BuildProfile final
{
  public:
    BuildProfile() = delete;
    BuildProfile(const BuildProfile &) = default;
    BuildProfile &operator=(const BuildProfile &) = default;
    BuildProfile(BuildProfile &&) noexcept = default;
    BuildProfile &operator=(BuildProfile &&) noexcept = default;
    ~BuildProfile() = default;

    /// @brief GameModule用ConfigurationとTargetを検証して所有Profileを構築する
    ///
    /// AssertContextは呼出中だけ借用し保持しない。不正な列挙値はInvalidProfileを返す。
    /// ShippingProductはTrust入力が必要なため拒否する。共有状態を変更しないため、呼出中有効な別入力から同時に使用できる。
    [[nodiscard]] static Result<BuildProfile> create(BuildConfiguration a_configuration, BuildTarget a_target,
                                                     const AssertContext &a_assertContext) noexcept;

    /// @brief Release Shipping Product用Trust入力を検証して所有Profileを構築する
    ///
    /// UnsignedLocalは空Publisher Key、PublisherSignedはDER SubjectPublicKeyInfoのlowercase SHA-256を要求する。
    /// AssertContextは呼出中だけ借用し保持しない。共有状態を変更しないため別入力から同時に使用できる。
    [[nodiscard]] static Result<BuildProfile> create_shipping_product(BuildConfiguration a_configuration,
                                                                      ShippingTrustMode a_minimumTrustMode,
                                                                      std::string a_publisherKeyId,
                                                                      const AssertContext &a_assertContext) noexcept;

    /// @brief Profile Wire形式のVersionを返す
    [[nodiscard]] std::uint32_t schema_version() const noexcept;
    /// @brief Build Configurationを返す
    [[nodiscard]] BuildConfiguration configuration() const noexcept;
    /// @brief Build Targetを返す
    [[nodiscard]] BuildTarget target() const noexcept;
    /// @brief Shipping Productの最小Trust Modeを返す
    ///
    /// GameModuleではnulloptを返す。
    [[nodiscard]] std::optional<ShippingTrustMode> minimum_trust_mode() const noexcept;
    /// @brief Publisher Public KeyのDER SubjectPublicKeyInfo SHA-256を返す
    ///
    /// GameModuleとUnsignedLocalでは空文字列を返す。返却ViewはProfileの寿命を超えて保持しない。
    [[nodiscard]] std::string_view publisher_key_id() const noexcept;

    /// @brief 全Profile値が一致するか比較する
    [[nodiscard]] bool operator==(const BuildProfile &) const noexcept = default;

  private:
    BuildProfile(BuildConfiguration a_configuration, BuildTarget a_target,
                 std::optional<ShippingTrustMode> a_minimumTrustMode, std::string a_publisherKeyId) noexcept;

    BuildConfiguration m_configuration;
    BuildTarget m_target;
    std::optional<ShippingTrustMode> m_minimumTrustMode;
    std::string m_publisherKeyId;
};

/// @brief UIから受け取る未検証Project Build要求
struct BuildRequest final
{
    std::string projectRoot;
    BuildProfile profile;
    std::string operationId;
    BuildWorkspaceCompatibility workspaceCompatibility;
};

/// @brief 検証済みRequestから決定した変更不能な一回のBuild入力と出力境界
///
/// Accessorが返す参照とstring_viewはこのBuildPlanの寿命を超えて保持しない。
class BuildPlan final
{
  public:
    BuildPlan() = delete;
    BuildPlan(const BuildPlan &) = delete;
    BuildPlan &operator=(const BuildPlan &) = delete;
    BuildPlan(BuildPlan &&) noexcept = default;
    BuildPlan &operator=(BuildPlan &&) noexcept = default;
    ~BuildPlan() = default;

    /// @brief 正規化済みUTF-8 Absolute Project Rootを返す
    [[nodiscard]] std::string_view project_root() const noexcept;
    /// @brief 検証済みBuild Profileを返す
    [[nodiscard]] const BuildProfile &profile() const noexcept;
    /// @brief lowercase canonical UUID v4 Operation IDを返す
    [[nodiscard]] std::string_view operation_id() const noexcept;
    /// @brief ConfigureがGeneratorとCache設定の選択に使用する共有Preset名を返す
    [[nodiscard]] std::string_view preset_name() const noexcept;
    /// @brief 互換Build入力を共有するBinary Tree Keyを返す
    [[nodiscard]] std::string_view workspace_key() const noexcept;
    /// @brief Workspace Keyの生成に使用した互換入力を返す
    [[nodiscard]] const BuildWorkspaceCompatibility &workspace_compatibility() const noexcept;
    /// @brief Plan固有Build WorkspaceのProcess間Lock Fileを返す
    [[nodiscard]] std::string_view workspace_lock_file() const noexcept;
    /// @brief Project Root内のCMake Binary Tree正本を返す
    ///
    /// RunnerはConfigure Presetの既定binaryDirを使用せず、このPathを-Bで明示する。BuildもBuild Presetではなく
    /// `--build <path>`へこの値を渡す。
    [[nodiscard]] std::string_view binary_directory() const noexcept;
    /// @brief Operation固有の未公開Artifact Directoryを返す
    [[nodiscard]] std::string_view candidate_directory() const noexcept;
    /// @brief Operation固有の診断保存Directoryを返す
    [[nodiscard]] std::string_view operation_directory() const noexcept;
    /// @brief Target、Configuration、Trust Identity別の公開Artifact Store Rootを返す
    [[nodiscard]] std::string_view artifact_store_directory() const noexcept;
    /// @brief CMakeへ渡す固定Target名を返す
    [[nodiscard]] std::string_view cmake_target_name() const noexcept;

    /// @brief Planの全決定値が一致するか比較する
    [[nodiscard]] bool equivalent_to(const BuildPlan &a_other) const noexcept;

  private:
    friend Result<BuildPlan> create_build_plan(const BuildRequest &, const AssertContext &) noexcept;

    BuildPlan(std::string a_projectRoot, BuildProfile a_profile, std::string a_operationId, std::string a_presetName,
              std::string a_workspaceKey, BuildWorkspaceCompatibility a_workspaceCompatibility,
              std::string a_workspaceLockFile, std::string a_binaryDirectory, std::string a_candidateDirectory,
              std::string a_operationDirectory, std::string a_artifactStoreDirectory) noexcept;

    std::string m_projectRoot;
    BuildProfile m_profile;
    std::string m_operationId;
    std::string m_presetName;
    std::string m_workspaceKey;
    BuildWorkspaceCompatibility m_workspaceCompatibility;
    std::string m_workspaceLockFile;
    std::string m_binaryDirectory;
    std::string m_candidateDirectory;
    std::string m_operationDirectory;
    std::string m_artifactStoreDirectory;
};

/// @brief ConfigureとBuildを区別するStage
enum class BuildStage : std::uint8_t
{
    Configure,
    Build
};

/// @brief Process終了、失敗、Cancel、Timeoutを混同しないStage結果
enum class BuildStageOutcome : std::uint8_t
{
    Succeeded,
    Failed,
    Cancelled,
    TimedOut
};

/// @brief Build Operationが観測する一方向Cancellation状態
enum class BuildCancellationState : std::uint8_t
{
    NotRequested,
    Requested
};

/// @brief 完了した一Stageの型付き結果
class BuildStageResult final
{
  public:
    BuildStageResult() = delete;

    /// @brief OutcomeとExit Codeの整合を検証して所有Stage結果を構築する
    ///
    /// AssertContextは呼出中だけ借用し保持しない。不正なStage、Outcome、Exit Codeの組合せは
    /// InvalidStageResultを返す。共有状態を変更しないため別入力から同時に使用できる。
    [[nodiscard]] static Result<BuildStageResult> create(BuildStage a_stage, BuildStageOutcome a_outcome,
                                                         std::optional<std::uint32_t> a_exitCode,
                                                         const AssertContext &a_assertContext) noexcept;

    /// @brief 完了したStageを返す
    [[nodiscard]] BuildStage stage() const noexcept;
    /// @brief 完了理由を返す
    [[nodiscard]] BuildStageOutcome outcome() const noexcept;
    /// @brief Native Processが終了した場合だけExit Codeを返す
    [[nodiscard]] std::optional<std::uint32_t> exit_code() const noexcept;

  private:
    BuildStageResult(BuildStage a_stage, BuildStageOutcome a_outcome, std::optional<std::uint32_t> a_exitCode) noexcept;

    BuildStage m_stage;
    BuildStageOutcome m_outcome;
    std::optional<std::uint32_t> m_exitCode;
};

/// @brief Version付きBuild Profileを固定順UTF-8 JSONへ直列化する
///
/// ProfileとAssertContextは呼出中だけ借用し、返却stringが結果を所有する。不正ProfileはInvalidProfileを返す。
/// 共有状態を変更しないため別入力から同時に使用できる。Allocation等の回復不能例外はFatalHandlerへ渡す。
[[nodiscard]] Result<std::string> serialize_build_profile(const BuildProfile &a_profile,
                                                          const AssertContext &a_assertContext) noexcept;

/// @brief Strict schemaVersion 1／2 JSONを検証済みBuild Profileへ変換する
///
/// JSONとAssertContextは呼出中だけ借用し、返却Profileが値を所有する。上限超過、未知Schema、重複または不正Memberは
/// InvalidProfileを返す。共有状態を変更しないため別入力から同時に使用できる。Allocation等の回復不能例外は
/// FatalHandlerへ渡す。
[[nodiscard]] Result<BuildProfile> parse_build_profile(std::string_view a_json,
                                                       const AssertContext &a_assertContext) noexcept;

/// @brief Requestを検証し、Root内出力だけを持つ決定的Build Planへ変換する
///
/// RequestとAssertContextは呼出中だけ借用し保持しない。返却されるmove-only BuildPlanがPathとProfileを所有する。
/// InvalidProfile、InvalidProjectRoot、InvalidOperationId、UnsafeOutputPathを入力条件に応じて返す。Outputの既存Path
/// Componentは Reparse Pointを拒否し、解決後もProject
/// Root内であることを検証する。共有状態を変更しないため別入力から同時に使用できる。
/// Allocationまたは予期しないFilesystem例外はFatalHandlerへ渡す。
[[nodiscard]] Result<BuildPlan> create_build_plan(const BuildRequest &a_request,
                                                  const AssertContext &a_assertContext) noexcept;
} // namespace cue
