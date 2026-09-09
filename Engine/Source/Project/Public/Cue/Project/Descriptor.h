#pragma once

#include <Cue/Foundation/Result.h>
#include <Cue/IO/RelativePath.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace cue
{
class AssertContext;
class FilesystemRoot;
class ProjectDescriptorMigrationOutcome;

inline constexpr std::uint32_t k_currentProjectDescriptorSchemaVersion = 2U;

/// @brief Project 移動や表示名変更でも変化しない UUID Version 4 Identity
class ProjectId final
{
  public:
    /// @brief 未検証または Nil の Identity を作らせないため既定構築を禁止する
    ProjectId() = delete;
    /// @brief Allocation 例外を境界外へ出さないため Identity の暗黙 Copy 構築を禁止する
    ProjectId(const ProjectId &) = delete;
    /// @brief Allocation 例外を境界外へ出さないため Identity の暗黙 Copy 代入を禁止する
    ProjectId &operator=(const ProjectId &) = delete;
    /// @brief 検証済み Identity の所有権を移動する
    ProjectId(ProjectId &&) noexcept = default;
    /// @brief 検証済み Identity を移動代入する
    ProjectId &operator=(ProjectId &&) noexcept = default;
    /// @brief Identity の所有 Storage を解放する
    ~ProjectId() = default;

    /// @brief lowercase 8-4-4-4-12 UUID Version 4 を検証して所有値を返す
    [[nodiscard]] static Result<ProjectId> parse(std::string_view a_text,
                                                 const AssertContext &a_assertContext) noexcept;

    /// @brief Descriptor へ保存する canonical UUID 文字列を返す
    [[nodiscard]] std::string_view text() const noexcept;

    /// @brief UUID の全 128-bit が一致するか比較する
    [[nodiscard]] bool operator==(const ProjectId &) const noexcept = default;

  private:
    /// @brief 検証済み canonical UUID だけから Identity を構築する
    explicit ProjectId(std::string &&a_text) noexcept;

    std::string m_text;
};

/// @brief CueEngine Release Version の比較可能な major.minor.patch 値
struct EngineVersion final
{
    std::uint32_t major = 0;
    std::uint32_t minor = 0;
    std::uint32_t patch = 0;

    /// @brief 3 要素を辞書順で比較する
    [[nodiscard]] auto operator<=>(const EngineVersion &) const noexcept = default;
};

/// @brief Project が許容する CueEngine Version の半開区間
struct EngineCompatibility final
{
    EngineVersion minimum;
    std::optional<EngineVersion> maximumExclusive;

    /// @brief Version 範囲の全要素が一致するか比較する
    [[nodiscard]] bool operator==(const EngineCompatibility &) const noexcept = default;
};

/// @brief Project Root 配下で役割が重ならない 4 種の Directory
class ProjectRoots final
{
  public:
    /// @brief 検証済み 4 種 Root の所有権を一つの値へ束ねる
    ProjectRoots(RelativePath &&a_sourceAssets, RelativePath &&a_runtimeAssets, RelativePath &&a_generated,
                 RelativePath &&a_saved) noexcept;
    /// @brief 必須 Root を欠く値を作らせないため既定構築を禁止する
    ProjectRoots() = delete;
    /// @brief 所有 Path の Allocation 例外を境界外へ出さないため Copy 構築を禁止する
    ProjectRoots(const ProjectRoots &) = delete;
    /// @brief 所有 Path の Allocation 例外を境界外へ出さないため Copy 代入を禁止する
    ProjectRoots &operator=(const ProjectRoots &) = delete;
    /// @brief 4 種 Root の所有権を移動する
    ProjectRoots(ProjectRoots &&) noexcept = default;
    /// @brief 4 種 Root の所有権を移動代入する
    ProjectRoots &operator=(ProjectRoots &&) noexcept = default;
    /// @brief 4 種 Root の所有 Storage を解放する
    ~ProjectRoots() = default;

    /// @brief Source Asset を配置する Project 内相対 Directory を返す
    [[nodiscard]] const RelativePath &source_assets() const noexcept;
    /// @brief Runtime Asset を配置する Project 内相対 Directory を返す
    [[nodiscard]] const RelativePath &runtime_assets() const noexcept;
    /// @brief 生成物を配置する Project 内相対 Directory を返す
    [[nodiscard]] const RelativePath &generated() const noexcept;
    /// @brief 保存データを配置する Project 内相対 Directory を返す
    [[nodiscard]] const RelativePath &saved() const noexcept;

  private:
    RelativePath m_sourceAssets;
    RelativePath m_runtimeAssets;
    RelativePath m_generated;
    RelativePath m_saved;
};

/// @brief Startup Sceneの永続IdentityとSource Asset Root相対Locatorを分離して所有する参照
class StartupSceneReference final
{
  public:
    /// @brief 無効な参照を作らせないため既定構築を禁止する
    StartupSceneReference() = delete;
    /// @brief 参照値を複製する
    StartupSceneReference(const StartupSceneReference &) = default;
    /// @brief 参照値を複製代入する
    StartupSceneReference &operator=(const StartupSceneReference &) = default;
    /// @brief 参照値を移動する
    StartupSceneReference(StartupSceneReference &&) noexcept = default;
    /// @brief 参照値を移動代入する
    StartupSceneReference &operator=(StartupSceneReference &&) noexcept = default;
    /// @brief 参照値を破棄する
    ~StartupSceneReference() = default;

    /// @brief SceneAssetIdのcanonical UUID Version 4文字列を返す
    [[nodiscard]] std::string_view scene_asset_id() const noexcept;
    /// @brief roots.sourceAssetsからの現在位置を返す
    [[nodiscard]] const RelativePath &source_locator() const noexcept;
    /// @brief IdentityとLocatorがともに一致するか比較する
    [[nodiscard]] bool equivalent_to(const StartupSceneReference &a_other) const noexcept;

  private:
    friend class ProjectDescriptor;
    friend Result<ProjectDescriptor> parse_project_descriptor(std::string_view, const AssertContext &) noexcept;
    friend Result<ProjectDescriptor> create_blank_project_descriptor(const ProjectId &, std::string_view,
                                                                     EngineCompatibility, std::string_view,
                                                                     const AssertContext &) noexcept;

    /// @brief 検証済みScene IdentityとSource Locatorを所有する参照へ束ねる
    StartupSceneReference(std::string &&a_sceneAssetId, RelativePath &&a_sourceLocator) noexcept;

    std::string m_sceneAssetId;
    RelativePath m_sourceLocator;
};

/// @brief 検証済み Project Descriptor v1／v2 の所有 Model
class ProjectDescriptor final
{
  public:
    /// @brief 必須値を欠く Descriptor を作らせないため既定構築を禁止する
    ProjectDescriptor() = delete;
    /// @brief 所有値の Allocation 例外を境界外へ出さないため Copy 構築を禁止する
    ProjectDescriptor(const ProjectDescriptor &) = delete;
    /// @brief 所有値の Allocation 例外を境界外へ出さないため Copy 代入を禁止する
    ProjectDescriptor &operator=(const ProjectDescriptor &) = delete;
    /// @brief Descriptor の所有権を移動する
    ProjectDescriptor(ProjectDescriptor &&) noexcept = default;
    /// @brief Descriptor を移動代入する
    ProjectDescriptor &operator=(ProjectDescriptor &&) noexcept = default;
    /// @brief Descriptor の所有 Storage を解放する
    ~ProjectDescriptor() = default;

    /// @brief 対応する Descriptor Format Version を返す
    [[nodiscard]] std::uint32_t schema_version() const noexcept;
    /// @brief Stable Project Identity を返す
    [[nodiscard]] const ProjectId &project_id() const noexcept;
    /// @brief UI 表示専用の Project 名を返す
    [[nodiscard]] std::string_view display_name() const noexcept;
    /// @brief CueEngine Version 互換範囲を返す
    [[nodiscard]] const EngineCompatibility &engine_compatibility() const noexcept;
    /// @brief Project Root 配下の役割別 Directory を返す
    [[nodiscard]] const ProjectRoots &roots() const noexcept;
    /// @brief Startup Scene参照を返す。未選択ならnullopt
    [[nodiscard]] const std::optional<StartupSceneReference> &default_scene() const noexcept;
    /// @brief 未知 Extension を意味解釈せず保持する canonical JSON Object を返す
    [[nodiscard]] std::string_view extensions_json() const noexcept;

    /// @brief 永続化対象の全論理値が一致するか比較する
    [[nodiscard]] bool equivalent_to(const ProjectDescriptor &a_other) const noexcept;

  private:
    friend Result<ProjectDescriptor> parse_project_descriptor(std::string_view, const AssertContext &) noexcept;
    friend Result<ProjectDescriptor> create_blank_project_descriptor(const ProjectId &, std::string_view,
                                                                     EngineCompatibility, std::string_view,
                                                                     const AssertContext &) noexcept;
    friend Result<ProjectDescriptorMigrationOutcome> migrate_project_descriptor(FilesystemRoot &,
                                                                                 const AssertContext &) noexcept;

    /// @brief Parser が検証した Descriptor v1／v2 の所有値を束ねる
    ProjectDescriptor(std::uint32_t a_schemaVersion, ProjectId &&a_projectId, std::string &&a_displayName,
                      EngineCompatibility a_engineCompatibility, ProjectRoots &&a_roots,
                      std::optional<StartupSceneReference> &&a_defaultScene, std::string &&a_extensionsJson) noexcept;

    std::uint32_t m_schemaVersion;
    ProjectId m_projectId;
    std::string m_displayName;
    EngineCompatibility m_engineCompatibility;
    ProjectRoots m_roots;
    std::optional<StartupSceneReference> m_defaultScene;
    std::string m_extensionsJson;
};

/// @brief Project Descriptor MigrationがDiskへ到達した状態
enum class ProjectDescriptorMigrationStatus : std::uint8_t
{
    Unchanged,
    Committed,
    PublishedButDurabilityUnknown
};

/// @brief Migration後ModelとAtomic公開結果を一つの診断可能なOutcomeとして所有する
class ProjectDescriptorMigrationOutcome final
{
  public:
    ProjectDescriptorMigrationOutcome() = delete;
    ProjectDescriptorMigrationOutcome(const ProjectDescriptorMigrationOutcome &) = delete;
    ProjectDescriptorMigrationOutcome &operator=(const ProjectDescriptorMigrationOutcome &) = delete;
    ProjectDescriptorMigrationOutcome(ProjectDescriptorMigrationOutcome &&) noexcept = default;
    ProjectDescriptorMigrationOutcome &operator=(ProjectDescriptorMigrationOutcome &&) noexcept = default;
    ~ProjectDescriptorMigrationOutcome() = default;

    /// @brief Current SchemaのMigration後Descriptorを返す
    [[nodiscard]] const ProjectDescriptor &descriptor() const noexcept;
    /// @brief Descriptorが未変更、Commit済み、または公開済みで耐久性不明か返す
    [[nodiscard]] ProjectDescriptorMigrationStatus status() const noexcept;
    /// @brief 公開後の耐久性確認失敗を返す。通常Commitまたは未変更ならnullptr
    [[nodiscard]] const Error *try_durability_error() const noexcept;

  private:
    friend Result<ProjectDescriptorMigrationOutcome> migrate_project_descriptor(FilesystemRoot &,
                                                                                 const AssertContext &) noexcept;

    /// @brief Migration後Descriptor、公開状態、任意の耐久性診断を所有する
    ProjectDescriptorMigrationOutcome(ProjectDescriptor &&a_descriptor, ProjectDescriptorMigrationStatus a_status,
                                      std::optional<Error> &&a_durabilityError) noexcept;

    ProjectDescriptor m_descriptor;
    ProjectDescriptorMigrationStatus m_status;
    std::optional<Error> m_durabilityError;
};

/// @brief UTF-8 JSON を一度だけ解析し、検証済み Descriptor v1 を構築する
[[nodiscard]] Result<ProjectDescriptor> parse_project_descriptor(std::string_view a_json,
                                                                 const AssertContext &a_assertContext) noexcept;

/// @brief Blank Project 用の固定 Root とDefault Scene参照を持つ検証済み Descriptor v2を構築する
[[nodiscard]] Result<ProjectDescriptor> create_blank_project_descriptor(const ProjectId &a_projectId,
                                                                        std::string_view a_displayName,
                                                                        EngineCompatibility a_engineCompatibility,
                                                                        std::string_view a_defaultSceneAssetId,
                                                                        const AssertContext &a_assertContext) noexcept;

/// @brief 所有 Model が Descriptor v1／v2 の全不変条件を満たすか再検証する
[[nodiscard]] Result<void> validate_project_descriptor(const ProjectDescriptor &a_descriptor,
                                                       const AssertContext &a_assertContext) noexcept;

/// @brief 検証済み Descriptor を deterministic UTF-8 JSON へ直列化する
[[nodiscard]] Result<std::string> serialize_project_descriptor(const ProjectDescriptor &a_descriptor,
                                                               const AssertContext &a_assertContext) noexcept;

/// @brief Project Root の CueProject.json を上限付きで読み込んで解析する
[[nodiscard]] Result<ProjectDescriptor> load_project_descriptor(FilesystemRoot &a_filesystem,
                                                                const AssertContext &a_assertContext) noexcept;

/// @brief Project Root の CueProject.json を Atomic に置換保存する
[[nodiscard]] Result<void> save_project_descriptor(FilesystemRoot &a_filesystem, const ProjectDescriptor &a_descriptor,
                                                   const AssertContext &a_assertContext) noexcept;

/// @brief CueProject.jsonを明示的に一段ずつCurrent SchemaへMigrationしAtomic置換する
///
/// Current Schemaは書込せずそのまま返す。公開前失敗は元Descriptorを維持し、公開後のDurabilityUnknownは
/// 成功OutcomeとしてDisk状態の不確実性を明示する
[[nodiscard]] Result<ProjectDescriptorMigrationOutcome> migrate_project_descriptor(
    FilesystemRoot &a_filesystem, const AssertContext &a_assertContext) noexcept;
} // namespace cue
