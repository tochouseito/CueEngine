#pragma once

#include <Cue/Build/Plan.h>
#include <Cue/Foundation/Result.h>
#include <Cue/Project/Descriptor.h>

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
}

namespace cue::package
{
inline constexpr std::uint32_t k_packageManifestSchemaVersion = 1U;
inline constexpr std::uint32_t k_monolithicPackageManifestSchemaVersion = 2U;
inline constexpr std::size_t k_maximumPackageManifestBytes = 1024U * 1024U;
inline constexpr std::size_t k_maximumPackageManifestStringBytes = 64U * 1024U;
inline constexpr std::size_t k_maximumPackageFileEntries = 256U;
inline constexpr std::size_t k_requiredPackageFileEntries = 5U;
inline constexpr std::size_t k_maximumPackageRelativePathBytes = 1024U;
inline constexpr std::size_t k_maximumPackagePathSegments = 32U;
inline constexpr std::uint64_t k_maximumPackagedFileBytes = 8ULL * 1024ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t k_maximumPackageInventoryBytes = 16ULL * 1024ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t k_maximumRuntimePeImageBytes = 128ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t k_maximumRuntimePeInventoryBytes = 256ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t k_maximumMonolithicExecutableBytes = 512ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t k_maximumMonolithicProjectDataBytes = 1024ULL * 1024ULL;
inline constexpr std::uint64_t k_maximumMonolithicSceneDataBytes = 64ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t k_maximumMonolithicSignatureBytes = 256ULL * 1024ULL;
inline constexpr std::string_view k_windowsSystemImportAllowlistVersion = "windows-10-1903-x64-v1";

/// @brief Package Manifestで一Fileの用途を固定するRole
enum class PackageFileRole : std::uint8_t
{
    RuntimeHost = 1,
    GameModule,
    GameModuleMetadata,
    ProjectRuntimeData,
    StartupSceneRuntimeData,
    RuntimeDependency,
    ApplicationExecutable
};

/// @brief Package内の実行境界をModular HostまたはMonolithic Productへ固定する
enum class PackageExecutionModel : std::uint8_t
{
    Modular = 1,
    Monolithic
};

/// @brief Package Root相対Pathと検証用Content Identityを所有する一Entry
class PackageFileEntry final
{
  public:
    /// @brief 未検証Entryを作らせないため既定構築を禁止する
    PackageFileEntry() = delete;
    /// @brief 所有Entryを複製する
    PackageFileEntry(const PackageFileEntry &) = default;
    /// @brief 所有Entryを複製代入する
    PackageFileEntry &operator=(const PackageFileEntry &) = default;
    /// @brief 所有Entryを移動する
    PackageFileEntry(PackageFileEntry &&) noexcept = default;
    /// @brief 所有Entryを移動代入する
    PackageFileEntry &operator=(PackageFileEntry &&) noexcept = default;
    /// @brief 所有Entryを破棄する
    ~PackageFileEntry() = default;

    /// @brief Role、Package相対Path、Size、SHA-256を検証してEntryを構築する
    [[nodiscard]] static Result<PackageFileEntry> create(PackageFileRole a_role, std::string a_relativePath,
                                                         std::uint64_t a_byteSize, std::string a_sha256,
                                                         const AssertContext &a_assertContext) noexcept;

    /// @brief Fileの固定用途を返す
    [[nodiscard]] PackageFileRole role() const noexcept;
    /// @brief Package Root相対の正規化済みPathを返す
    [[nodiscard]] std::string_view relative_path() const noexcept;
    /// @brief Fileの未変換Byte数を返す
    [[nodiscard]] std::uint64_t byte_size() const noexcept;
    /// @brief Fileのlowercase SHA-256を返す
    [[nodiscard]] std::string_view sha256() const noexcept;

  private:
    /// @brief 検証済みEntry値を所有する
    PackageFileEntry(PackageFileRole a_role, std::string a_relativePath, std::uint64_t a_byteSize,
                     std::string a_sha256) noexcept;

    PackageFileRole m_role;
    std::string m_relativePath;
    std::uint64_t m_byteSize;
    std::string m_sha256;
};

/// @brief Runtime Dependency一件と取得元Build Configurationを対応付ける入力値
struct RuntimeDependencyCandidate final
{
    /// @brief Dependencyを取得したBuild Configuration
    BuildConfiguration sourceConfiguration;
    /// @brief Runtime Dependency Roleを持つPackage File Entry
    PackageFileEntry file;
};

/// @brief Runtime Dependency候補を単一Configurationと重複なしの決定順Inventoryへ検証する
///
/// 候補の所有権を取得し、期待Configurationと異なる取得元、RuntimeDependency以外のRole、PDB、Pathの
/// case-insensitive alias、Resource Limit違反を拒否する。成功時はPackage相対Path順のEntry列だけを返す。
[[nodiscard]] Result<std::vector<PackageFileEntry>> validate_runtime_dependency_inventory(
    BuildConfiguration a_expectedConfiguration, std::vector<RuntimeDependencyCandidate> a_candidates,
    const AssertContext &a_assertContext) noexcept;

/// @brief 検証対象PE Imageの論理File名と不変Byte Snapshot
struct RuntimePeImageView final
{
    std::string_view fileName;
    std::span<const std::byte> bytes;
};

/// @brief Game Moduleから到達するApp-local DLLを依存先優先の安全な事前Load順へ検証する
///
/// 通常ImportとDelay-load Importを再帰検査し、固定Allowlist外のImport、未登録／未到達DLL、循環、
/// 不正PE、Export Forwarderを拒否する。成功時のIndex列は`a_appLocalDependencies`を参照する。
[[nodiscard]] Result<std::vector<std::size_t>> create_runtime_dependency_load_order(
    BuildConfiguration a_configuration, RuntimePeImageView a_gameModule,
    std::span<const RuntimePeImageView> a_appLocalDependencies, const AssertContext &a_assertContext) noexcept;

/// @brief Runtime Host、Game Module、App-local DLLのx64 PE Import閉包を固定Allowlistへ照合する
///
/// 通常ImportとDelay-load Importを再帰検査する。Hostの非System Import、構成違いMSVC Runtime、未登録／未到達
/// App-local DLL、循環、不正PE、Export ForwarderをPublish前に拒否する。全Viewは呼出中だけ借用する。
[[nodiscard]] Result<void> validate_runtime_dependency_closure(
    BuildConfiguration a_configuration, RuntimePeImageView a_runtimeHost, RuntimePeImageView a_gameModule,
    std::span<const RuntimePeImageView> a_appLocalDependencies, const AssertContext &a_assertContext) noexcept;

/// @brief Standalone Runtime PackageのVersion付きIdentityと完全File Inventory
class PackageManifest final
{
  public:
    /// @brief 未検証Manifestを作らせないため既定構築を禁止する
    PackageManifest() = delete;
    /// @brief 所有Manifestを複製する
    PackageManifest(const PackageManifest &) = default;
    /// @brief 所有Manifestを複製代入する
    PackageManifest &operator=(const PackageManifest &) = default;
    /// @brief 所有Manifestを移動する
    PackageManifest(PackageManifest &&) noexcept = default;
    /// @brief 所有Manifestを移動代入する
    PackageManifest &operator=(PackageManifest &&) noexcept = default;
    /// @brief 所有Manifestを破棄する
    ~PackageManifest() = default;

    /// @brief Identity、Configuration、Startup Scene、File Role集合を検証してManifestを構築する
    [[nodiscard]] static Result<PackageManifest> create(std::string a_projectId, EngineVersion a_engineVersion,
                                                        BuildConfiguration a_configuration,
                                                        std::string a_startupSceneAssetId,
                                                        std::string a_startupSceneRuntimeDataPath,
                                                        std::vector<PackageFileEntry> a_files,
                                                        const AssertContext &a_assertContext) noexcept;

    /// @brief Release Monolithic Identity、Trust、三つの必須Roleを検証してManifest v2を構築する
    [[nodiscard]] static Result<PackageManifest> create_monolithic(
        std::string a_projectId, EngineVersion a_engineVersion, BuildConfiguration a_configuration,
        std::string a_startupSceneAssetId, std::string a_startupSceneRuntimeDataPath, ShippingTrustMode a_trustMode,
        std::optional<std::string> a_publisherKeyId, std::optional<std::string> a_manifestSignaturePath,
        std::vector<PackageFileEntry> a_files, const AssertContext &a_assertContext) noexcept;

    /// @brief Manifest Wire Schema Versionを返す
    [[nodiscard]] std::uint32_t schema_version() const noexcept;
    /// @brief Packageが属するlowercase Project UUIDを返す
    [[nodiscard]] std::string_view project_id() const noexcept;
    /// @brief Package生成に使用したEngine Versionを返す
    [[nodiscard]] const EngineVersion &engine_version() const noexcept;
    /// @brief Package全体で固定したBuild Configurationを返す
    [[nodiscard]] BuildConfiguration configuration() const noexcept;
    /// @brief Runtime Host方式または単一Executable方式の実行Modelを返す
    [[nodiscard]] PackageExecutionModel execution_model() const noexcept;
    /// @brief Startup Sceneのlowercase UUIDを返す
    [[nodiscard]] std::string_view startup_scene_asset_id() const noexcept;
    /// @brief Startup Scene Runtime DataのPackage相対Pathを返す
    [[nodiscard]] std::string_view startup_scene_runtime_data_path() const noexcept;
    /// @brief Monolithic Manifestの固定Executable名を返し、v1 Modularではnulloptを返す
    [[nodiscard]] std::optional<std::string_view> application_executable() const noexcept;
    /// @brief Monolithic Manifestの最低Trust Modeを返し、v1 Modularではnulloptを返す
    [[nodiscard]] std::optional<ShippingTrustMode> trust_mode() const noexcept;
    /// @brief PublisherSigned ManifestのPublisher Key IDを返す
    [[nodiscard]] std::optional<std::string_view> publisher_key_id() const noexcept;
    /// @brief PublisherSigned ManifestのDetached Signature相対Pathを返す
    [[nodiscard]] std::optional<std::string_view> manifest_signature_path() const noexcept;
    /// @brief Package相対PathのUTF-8 Byte昇順に固定した全File Entryを返す
    [[nodiscard]] std::span<const PackageFileEntry> files() const noexcept;

  private:
    /// @brief 検証済みManifest値を所有する
    PackageManifest(std::uint32_t a_schemaVersion, std::string a_projectId, EngineVersion a_engineVersion,
                    BuildConfiguration a_configuration, PackageExecutionModel a_executionModel,
                    std::string a_startupSceneAssetId, std::string a_startupSceneRuntimeDataPath,
                    std::optional<std::string> a_applicationExecutable, std::optional<ShippingTrustMode> a_trustMode,
                    std::optional<std::string> a_publisherKeyId, std::optional<std::string> a_manifestSignaturePath,
                    std::vector<PackageFileEntry> a_files) noexcept;

    std::uint32_t m_schemaVersion;
    std::string m_projectId;
    EngineVersion m_engineVersion;
    BuildConfiguration m_configuration;
    PackageExecutionModel m_executionModel;
    std::string m_startupSceneAssetId;
    std::string m_startupSceneRuntimeDataPath;
    std::optional<std::string> m_applicationExecutable;
    std::optional<ShippingTrustMode> m_trustMode;
    std::optional<std::string> m_publisherKeyId;
    std::optional<std::string> m_manifestSignaturePath;
    std::vector<PackageFileEntry> m_files;
};

/// @brief 検証済みManifestをCanonical UTF-8 JSONへ直列化する
[[nodiscard]] Result<std::string> serialize_package_manifest(const PackageManifest &a_manifest,
                                                             const AssertContext &a_assertContext) noexcept;

/// @brief Resource Limit内のManifest JSONを未知Versionと不正Inventoryを許容せず解析する
[[nodiscard]] Result<PackageManifest> parse_package_manifest(std::string_view a_json,
                                                             const AssertContext &a_assertContext) noexcept;

/// @brief Manifest EntryとMemory上の同一File Byte列をSizeとSHA-256へ照合する
[[nodiscard]] Result<void> verify_package_file_bytes(const PackageFileEntry &a_entry,
                                                     std::span<const std::byte> a_bytes,
                                                     const AssertContext &a_assertContext) noexcept;

/// @brief Package RootのFileをManifestのSizeとSHA-256へ照合する
///
/// Package RootとManifestは呼出中だけ借用する。v1はManifest Entryだけを検証する。v2はRootを完全列挙し、
/// Manifest、Payload、任意の署名以外のFile、未知または空のDirectory、間接Pathも拒否する。
[[nodiscard]] Result<void> verify_package_manifest_files(std::string_view a_packageRoot,
                                                         const PackageManifest &a_manifest,
                                                         const AssertContext &a_assertContext) noexcept;
} // namespace cue::package
