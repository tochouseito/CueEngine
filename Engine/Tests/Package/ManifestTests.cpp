#include <Cue/Package/Error.h>
#include <Cue/Package/Manifest.h>

#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Fatal.h>
#include <Cue/Foundation/Log.h>

#if defined(_WIN32)
#include <Windows.h>
#endif

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
constexpr std::string_view k_projectId = "41234567-89ab-4cde-8f01-23456789abcd";
constexpr std::string_view k_sceneId = "51234567-89ab-4cde-8f01-23456789abcd";
constexpr std::string_view k_scenePath = "Data/Scenes/51234567-89ab-4cde-8f01-23456789abcd.cueruntime.json";
constexpr std::string_view k_xHash = "2d711642b726b04401627ca9fbac32f5c8530fb1903cc4db02258717921a4881";

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

/// @brief Resultが期待するPackage ErrorをRootに持つか返す
template <typename T>
[[nodiscard]] bool is_package_error(const cue::Result<T> &a_result, cue::package::PackageError a_error) noexcept
{
    return !a_result && a_result.try_error()->root_code().domain() == "Cue.Package" &&
           a_result.try_error()->root_code().value() == static_cast<std::int64_t>(a_error);
}

/// @brief 固定一Byte内容を表す検証済みManifest Entryを構築する
[[nodiscard]] cue::package::PackageFileEntry make_entry(cue::package::PackageFileRole a_role, std::string a_path,
                                                        const cue::AssertContext &a_assertContext)
{
    return take_value(
        cue::package::PackageFileEntry::create(a_role, std::move(a_path), 1U, std::string(k_xHash), a_assertContext));
}

/// @brief 必須五Roleを一つずつ持つ最小Package Inventoryを構築する
[[nodiscard]] std::vector<cue::package::PackageFileEntry> make_required_files(const cue::AssertContext &a_assertContext)
{
    using cue::package::PackageFileRole;
    std::vector<cue::package::PackageFileEntry> files;
    files.push_back(make_entry(PackageFileRole::RuntimeHost, "CueRuntimeHost.exe", a_assertContext));
    files.push_back(make_entry(PackageFileRole::GameModule, "Game/CueGameModule.dll", a_assertContext));
    files.push_back(
        make_entry(PackageFileRole::GameModuleMetadata, "Game/CueGameModule.metadata.json", a_assertContext));
    files.push_back(make_entry(PackageFileRole::ProjectRuntimeData, "Data/CueProject.runtime.json", a_assertContext));
    files.push_back(make_entry(PackageFileRole::StartupSceneRuntimeData, std::string(k_scenePath), a_assertContext));
    return files;
}

/// @brief 固定Identityと必須Inventoryから検証済みManifestを構築する
[[nodiscard]] cue::package::PackageManifest make_manifest(const cue::AssertContext &a_assertContext)
{
    return take_value(cue::package::PackageManifest::create(
        std::string(k_projectId), {1U, 2U, 3U}, cue::BuildConfiguration::Development, std::string(k_sceneId),
        std::string(k_scenePath), make_required_files(a_assertContext), a_assertContext));
}

/// @brief Test Root相対Fileを親Directory作成付きで書き込む
void write_file(const std::filesystem::path &a_root, std::string_view a_relativePath, std::string_view a_bytes)
{
    const std::filesystem::path path = a_root / std::filesystem::path(a_relativePath);
    std::error_code error;
    require(std::filesystem::create_directories(path.parent_path(), error) || !error);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(a_bytes.data(), static_cast<std::streamsize>(a_bytes.size()));
    require(static_cast<bool>(output));
}

/// @brief Manifestが列挙する固定File集合を同じ一Byte内容で作成する
void write_manifest_files(const std::filesystem::path &a_root, const cue::package::PackageManifest &a_manifest)
{
    for (const cue::package::PackageFileEntry &entry : a_manifest.files())
    {
        write_file(a_root, entry.relative_path(), "x");
    }
}

/// @brief Test PEへLittle-endian 16-bit値を書き込む
void write_u16(std::vector<std::byte> &a_bytes, std::size_t a_offset, std::uint16_t a_value) noexcept
{
    a_bytes[a_offset] = static_cast<std::byte>(a_value & 0xffU);
    a_bytes[a_offset + 1U] = static_cast<std::byte>((a_value >> 8U) & 0xffU);
}

/// @brief Test PEへLittle-endian 32-bit値を書き込む
void write_u32(std::vector<std::byte> &a_bytes, std::size_t a_offset, std::uint32_t a_value) noexcept
{
    for (std::size_t index = 0U; index < 4U; ++index)
    {
        a_bytes[a_offset + index] = static_cast<std::byte>((a_value >> (index * 8U)) & 0xffU);
    }
}

/// @brief Test PEへLittle-endian 64-bit値を書き込む
void write_u64(std::vector<std::byte> &a_bytes, std::size_t a_offset, std::uint64_t a_value) noexcept
{
    write_u32(a_bytes, a_offset, static_cast<std::uint32_t>(a_value));
    write_u32(a_bytes, a_offset + 4U, static_cast<std::uint32_t>(a_value >> 32U));
}

/// @brief Test PEのSection内へNUL終端ASCII文字列を書き込む
void write_ascii(std::vector<std::byte> &a_bytes, std::size_t a_offset, std::string_view a_text) noexcept
{
    for (std::size_t index = 0U; index < a_text.size(); ++index)
    {
        a_bytes[a_offset + index] = static_cast<std::byte>(a_text[index]);
    }
    a_bytes[a_offset + a_text.size()] = std::byte{0U};
}

/// @brief 指定Import、Delay Import、Forwarderを持つ最小x64 PE Test Imageを生成する
[[nodiscard]] std::vector<std::byte> make_test_pe(std::span<const std::string_view> a_imports,
                                                  std::span<const std::string_view> a_delayImports,
                                                  bool a_hasForwarder = false)
{
    std::vector<std::byte> bytes(0x1000U, std::byte{0U});
    write_u16(bytes, 0U, 0x5a4dU);
    write_u32(bytes, 0x3cU, 0x80U);
    write_u32(bytes, 0x80U, 0x00004550U);
    write_u16(bytes, 0x84U, 0x8664U);
    write_u16(bytes, 0x86U, 1U);
    write_u16(bytes, 0x94U, 240U);
    constexpr std::size_t optional = 0x98U;
    write_u16(bytes, optional, 0x020bU);
    write_u32(bytes, optional + 60U, 0x200U);
    write_u32(bytes, optional + 108U, 16U);
    constexpr std::size_t section = 0x188U;
    write_u32(bytes, section + 8U, 0x0e00U);
    write_u32(bytes, section + 12U, 0x1000U);
    write_u32(bytes, section + 16U, 0x0e00U);
    write_u32(bytes, section + 20U, 0x200U);

    if (!a_imports.empty())
    {
        write_u32(bytes, optional + 120U, 0x1000U);
        write_u32(bytes, optional + 124U,
                  static_cast<std::uint32_t>((a_imports.size() + 1U) * 20U));
        std::size_t nameOffset = 0x300U;
        for (std::size_t index = 0U; index < a_imports.size(); ++index)
        {
            const std::uint32_t lookupRva = 0x1500U + static_cast<std::uint32_t>(index * 0x20U);
            const std::uint32_t addressRva = 0x1600U + static_cast<std::uint32_t>(index * 0x20U);
            write_u32(bytes, 0x200U + index * 20U, lookupRva);
            write_u32(bytes, 0x200U + index * 20U + 12U,
                      0x1000U + static_cast<std::uint32_t>(nameOffset - 0x200U));
            write_u32(bytes, 0x200U + index * 20U + 16U, addressRva);
            write_u64(bytes, 0x700U + index * 0x20U, 0x8000000000000001ULL);
            write_u64(bytes, 0x800U + index * 0x20U, 0x8000000000000001ULL);
            write_ascii(bytes, nameOffset, a_imports[index]);
            nameOffset += a_imports[index].size() + 1U;
        }
    }
    if (!a_delayImports.empty())
    {
        write_u32(bytes, optional + 216U, 0x1200U);
        write_u32(bytes, optional + 220U,
                  static_cast<std::uint32_t>((a_delayImports.size() + 1U) * 32U));
        std::size_t nameOffset = 0x500U;
        for (std::size_t index = 0U; index < a_delayImports.size(); ++index)
        {
            write_u32(bytes, 0x400U + index * 32U, 1U);
            write_u32(bytes, 0x400U + index * 32U + 4U,
                      0x1200U + static_cast<std::uint32_t>(nameOffset - 0x400U));
            write_u32(bytes, 0x400U + index * 32U + 8U,
                      0x1700U + static_cast<std::uint32_t>(index * 8U));
            write_u32(bytes, 0x400U + index * 32U + 12U,
                      0x1800U + static_cast<std::uint32_t>(index * 0x20U));
            write_u32(bytes, 0x400U + index * 32U + 16U,
                      0x1900U + static_cast<std::uint32_t>(index * 0x20U));
            write_u64(bytes, 0xa00U + index * 0x20U, 0x8000000000000001ULL);
            write_u64(bytes, 0xb00U + index * 0x20U, 0x8000000000000001ULL);
            write_ascii(bytes, nameOffset, a_delayImports[index]);
            nameOffset += a_delayImports[index].size() + 1U;
        }
    }
    if (a_hasForwarder)
    {
        write_u32(bytes, optional + 112U, 0x1400U);
        write_u32(bytes, optional + 116U, 0x100U);
        write_u32(bytes, 0x600U + 20U, 1U);
        write_u32(bytes, 0x600U + 28U, 0x1450U);
        write_u32(bytes, 0x650U, 0x1470U);
    }
    return bytes;
}

/// @brief 最初に一致するManifest Tokenを指定Byte列へ置換する
[[nodiscard]] std::string replace_first(std::string a_text, std::string_view a_from, std::string_view a_to)
{
    const std::size_t position = a_text.find(a_from);
    require(position != std::string::npos);
    a_text.replace(position, a_from.size(), a_to);
    return a_text;
}

/// @brief Canonical直列化、再解析、未知SchemaとResource Limit拒否を検証する
[[nodiscard]] bool test_manifest_wire_contract(const cue::AssertContext &a_assertContext)
{
    const cue::package::PackageManifest manifest = make_manifest(a_assertContext);
    const std::string serialized = take_value(cue::package::serialize_package_manifest(manifest, a_assertContext));
    auto parsed = cue::package::parse_package_manifest(serialized, a_assertContext);
    if (!parsed || parsed.try_value()->schema_version() != cue::package::k_packageManifestSchemaVersion ||
        parsed.try_value()->project_id() != k_projectId || parsed.try_value()->engine_version().major != 1U ||
        parsed.try_value()->engine_version().minor != 2U || parsed.try_value()->engine_version().patch != 3U ||
        parsed.try_value()->configuration() != cue::BuildConfiguration::Development ||
        parsed.try_value()->startup_scene_asset_id() != k_sceneId ||
        parsed.try_value()->startup_scene_runtime_data_path() != k_scenePath ||
        parsed.try_value()->files().size() != 5U)
    {
        return false;
    }
    auto serializedAgain = cue::package::serialize_package_manifest(*parsed.try_value(), a_assertContext);
    if (!serializedAgain || *serializedAgain.try_value() != serialized || serialized.back() != '\n')
    {
        return false;
    }

    std::string futureVersion = serialized;
    const std::size_t version = futureVersion.find("\"schemaVersion\":1");
    require(version != std::string::npos);
    futureVersion[version + std::string_view("\"schemaVersion\":").size()] = '2';
    auto unsupported = cue::package::parse_package_manifest(futureVersion, a_assertContext);

    std::string unknownMember = serialized;
    unknownMember.insert(unknownMember.size() - 2U, ",\"unexpected\":1");
    auto unknown = cue::package::parse_package_manifest(unknownMember, a_assertContext);

    std::string unknownRole = serialized;
    const std::size_t role = unknownRole.find("runtimeHost");
    require(role != std::string::npos);
    unknownRole.replace(role, std::string_view("runtimeHost").size(), "unknownRole");
    auto invalidRole = cue::package::parse_package_manifest(unknownRole, a_assertContext);

    const std::string oversized(cue::package::k_maximumPackageManifestBytes + 1U, ' ');
    auto overLimit = cue::package::parse_package_manifest(oversized, a_assertContext);
    auto zeroSize = cue::package::parse_package_manifest(
        replace_first(serialized, "\"sizeBytes\":1", "\"sizeBytes\":0"), a_assertContext);
    auto maximumSize = cue::package::parse_package_manifest(
        replace_first(serialized, "\"sizeBytes\":1", "\"sizeBytes\":8589934592"), a_assertContext);
    auto negativeSize = cue::package::parse_package_manifest(
        replace_first(serialized, "\"sizeBytes\":1", "\"sizeBytes\":-1"), a_assertContext);
    auto fractionalSize = cue::package::parse_package_manifest(
        replace_first(serialized, "\"sizeBytes\":1", "\"sizeBytes\":1.0"), a_assertContext);
    auto exponentialSize = cue::package::parse_package_manifest(
        replace_first(serialized, "\"sizeBytes\":1", "\"sizeBytes\":1e0"), a_assertContext);
    auto oversizedFile = cue::package::parse_package_manifest(
        replace_first(serialized, "\"sizeBytes\":1", "\"sizeBytes\":8589934593"), a_assertContext);
    return is_package_error(unsupported, cue::package::PackageError::UnsupportedPackageManifestVersion) &&
           is_package_error(unknown, cue::package::PackageError::InvalidPackageManifest) &&
           is_package_error(invalidRole, cue::package::PackageError::InvalidPackageManifest) &&
           is_package_error(overLimit, cue::package::PackageError::PackageManifestResourceLimitExceeded) &&
           zeroSize && maximumSize && is_package_error(negativeSize, cue::package::PackageError::InvalidPackageManifest) &&
           is_package_error(fractionalSize, cue::package::PackageError::InvalidPackageManifest) &&
           is_package_error(exponentialSize, cue::package::PackageError::InvalidPackageManifest) &&
           is_package_error(oversizedFile, cue::package::PackageError::PackageManifestResourceLimitExceeded);
}

/// @brief Path、Hash、必須Role、case alias、PDBの不正Inventory拒否を検証する
[[nodiscard]] bool test_manifest_validation(const cue::AssertContext &a_assertContext)
{
    using cue::package::PackageFileRole;
    auto absolute = cue::package::PackageFileEntry::create(PackageFileRole::RuntimeDependency, "C:/escape.dll", 1U,
                                                           std::string(k_xHash), a_assertContext);
    auto invalidHash = cue::package::PackageFileEntry::create(PackageFileRole::RuntimeDependency, "Runtime/Hash.dll",
                                                              1U, std::string(64U, 'A'), a_assertContext);
    auto zeroSize = cue::package::PackageFileEntry::create(PackageFileRole::RuntimeDependency, "Runtime/Zero.dll", 0U,
                                                           std::string(k_xHash), a_assertContext);

    std::vector<cue::package::PackageFileEntry> missingFiles = make_required_files(a_assertContext);
    missingFiles.pop_back();
    auto missing = cue::package::PackageManifest::create(
        std::string(k_projectId), {1U, 0U, 0U}, cue::BuildConfiguration::Debug, std::string(k_sceneId),
        std::string(k_scenePath), std::move(missingFiles), a_assertContext);

    std::vector<cue::package::PackageFileEntry> duplicateFiles = make_required_files(a_assertContext);
    duplicateFiles.push_back(make_entry(PackageFileRole::RuntimeHost, "CueRuntimeHost.exe", a_assertContext));
    auto duplicate = cue::package::PackageManifest::create(
        std::string(k_projectId), {1U, 0U, 0U}, cue::BuildConfiguration::Debug, std::string(k_sceneId),
        std::string(k_scenePath), std::move(duplicateFiles), a_assertContext);

    std::vector<cue::package::PackageFileEntry> aliases = make_required_files(a_assertContext);
    aliases.push_back(make_entry(PackageFileRole::RuntimeDependency, "Runtime/A.dll", a_assertContext));
    aliases.push_back(make_entry(PackageFileRole::RuntimeDependency, "Runtime/B.dll", a_assertContext));
    aliases.push_back(make_entry(PackageFileRole::RuntimeDependency, "Runtime/a.dll", a_assertContext));
    auto alias = cue::package::PackageManifest::create(std::string(k_projectId), {1U, 0U, 0U},
                                                       cue::BuildConfiguration::Debug, std::string(k_sceneId),
                                                       std::string(k_scenePath), std::move(aliases), a_assertContext);

    std::vector<cue::package::PackageFileEntry> pdbFiles = make_required_files(a_assertContext);
    pdbFiles.push_back(make_entry(PackageFileRole::RuntimeDependency, "Runtime/Cue.pdb", a_assertContext));
    auto pdb = cue::package::PackageManifest::create(std::string(k_projectId), {1U, 0U, 0U},
                                                     cue::BuildConfiguration::Debug, std::string(k_sceneId),
                                                     std::string(k_scenePath), std::move(pdbFiles), a_assertContext);

    std::vector<cue::package::PackageFileEntry> nestedDependencyFiles = make_required_files(a_assertContext);
    nestedDependencyFiles.push_back(
        make_entry(PackageFileRole::RuntimeDependency, "Runtime/Vendor/Cue.dll", a_assertContext));
    auto nestedDependency = cue::package::PackageManifest::create(
        std::string(k_projectId), {1U, 0U, 0U}, cue::BuildConfiguration::Debug, std::string(k_sceneId),
        std::string(k_scenePath), std::move(nestedDependencyFiles), a_assertContext);

    return is_package_error(absolute, cue::package::PackageError::InvalidPackagePath) &&
           is_package_error(invalidHash, cue::package::PackageError::InvalidPackageManifest) &&
           zeroSize && zeroSize.try_value()->byte_size() == 0U &&
           is_package_error(missing, cue::package::PackageError::InvalidPackageManifest) &&
           is_package_error(duplicate, cue::package::PackageError::InvalidPackageManifest) &&
           is_package_error(alias, cue::package::PackageError::InvalidPackageManifest) &&
           is_package_error(pdb, cue::package::PackageError::InvalidPackageManifest) &&
           is_package_error(nestedDependency, cue::package::PackageError::InvalidPackageManifest);
}

/// @brief Runtime Dependencyの単一Configuration、Role、決定順、重複拒否を検証する
[[nodiscard]] bool test_runtime_dependency_inventory(const cue::AssertContext &a_assertContext)
{
    using cue::package::PackageFileRole;
    std::vector<cue::package::RuntimeDependencyCandidate> valid;
    valid.push_back({cue::BuildConfiguration::Development,
                     make_entry(PackageFileRole::RuntimeDependency, "Runtime/Z.dll", a_assertContext)});
    valid.push_back({cue::BuildConfiguration::Development,
                     make_entry(PackageFileRole::RuntimeDependency, "Runtime/A.dll", a_assertContext)});
    auto inventory = cue::package::validate_runtime_dependency_inventory(cue::BuildConfiguration::Development,
                                                                         std::move(valid), a_assertContext);

    std::vector<cue::package::RuntimeDependencyCandidate> mixed;
    mixed.push_back({cue::BuildConfiguration::Debug,
                     make_entry(PackageFileRole::RuntimeDependency, "Runtime/Debug.dll", a_assertContext)});
    mixed.push_back({cue::BuildConfiguration::Release,
                     make_entry(PackageFileRole::RuntimeDependency, "Runtime/Release.dll", a_assertContext)});
    auto configurationMix = cue::package::validate_runtime_dependency_inventory(cue::BuildConfiguration::Debug,
                                                                                std::move(mixed), a_assertContext);

    std::vector<cue::package::RuntimeDependencyCandidate> aliases;
    aliases.push_back({cue::BuildConfiguration::Debug,
                       make_entry(PackageFileRole::RuntimeDependency, "Runtime/A.dll", a_assertContext)});
    aliases.push_back({cue::BuildConfiguration::Debug,
                       make_entry(PackageFileRole::RuntimeDependency, "Runtime/a.dll", a_assertContext)});
    auto duplicateAlias = cue::package::validate_runtime_dependency_inventory(cue::BuildConfiguration::Debug,
                                                                              std::move(aliases), a_assertContext);

    std::vector<cue::package::RuntimeDependencyCandidate> wrongRole;
    wrongRole.push_back({cue::BuildConfiguration::Debug,
                         make_entry(PackageFileRole::RuntimeHost, "CueRuntimeHost.exe", a_assertContext)});
    auto role = cue::package::validate_runtime_dependency_inventory(cue::BuildConfiguration::Debug,
                                                                    std::move(wrongRole), a_assertContext);

    std::vector<cue::package::RuntimeDependencyCandidate> oversized;
    oversized.push_back(
        {cue::BuildConfiguration::Debug,
         take_value(cue::package::PackageFileEntry::create(
             PackageFileRole::RuntimeDependency, "Runtime/LargeA.dll", cue::package::k_maximumPackagedFileBytes,
             std::string(k_xHash), a_assertContext))});
    oversized.push_back(
        {cue::BuildConfiguration::Debug,
         take_value(cue::package::PackageFileEntry::create(
             PackageFileRole::RuntimeDependency, "Runtime/LargeB.dll", cue::package::k_maximumPackagedFileBytes,
             std::string(k_xHash), a_assertContext))});
    oversized.push_back(
        {cue::BuildConfiguration::Debug,
         take_value(cue::package::PackageFileEntry::create(
             PackageFileRole::RuntimeDependency, "Runtime/LargeC.dll", 1U, std::string(k_xHash), a_assertContext))});
    auto overLimit = cue::package::validate_runtime_dependency_inventory(cue::BuildConfiguration::Debug,
                                                                         std::move(oversized), a_assertContext);

    std::vector<cue::package::RuntimeDependencyCandidate> overCount;
    overCount.reserve(cue::package::k_maximumPackageFileEntries - cue::package::k_requiredPackageFileEntries + 1U);
    for (std::size_t index = 0U;
         index < cue::package::k_maximumPackageFileEntries - cue::package::k_requiredPackageFileEntries + 1U;
         ++index)
    {
        overCount.push_back(
            {cue::BuildConfiguration::Debug,
             make_entry(PackageFileRole::RuntimeDependency,
                        "Runtime/Dependency" + std::to_string(index) + ".dll", a_assertContext)});
    }
    auto tooMany = cue::package::validate_runtime_dependency_inventory(cue::BuildConfiguration::Debug,
                                                                        std::move(overCount), a_assertContext);

    return inventory && inventory.try_value()->size() == 2U &&
           inventory.try_value()->front().relative_path() == "Runtime/A.dll" &&
           inventory.try_value()->back().relative_path() == "Runtime/Z.dll" &&
           is_package_error(configurationMix, cue::package::PackageError::InvalidPackageManifest) &&
           is_package_error(duplicateAlias, cue::package::PackageError::InvalidPackageManifest) &&
           is_package_error(role, cue::package::PackageError::InvalidPackageManifest) &&
           is_package_error(overLimit, cue::package::PackageError::PackageManifestResourceLimitExceeded) &&
           is_package_error(tooMany, cue::package::PackageError::PackageManifestResourceLimitExceeded);
}

/// @brief x64 PE通常／Delay Import閉包、構成Allowlist、未登録／未到達、Forwarder拒否を検証する
[[nodiscard]] bool test_runtime_dependency_closure(const cue::AssertContext &a_assertContext)
{
    constexpr std::array hostImports = {std::string_view("KERNEL32.dll"), std::string_view("VCRUNTIME140D.dll")};
    constexpr std::array gameImports = {std::string_view("kernel32.dll"), std::string_view("LocalA.dll")};
    constexpr std::array localADelayImports = {std::string_view("LocalB.dll")};
    constexpr std::array localBImports = {std::string_view("api-ms-win-crt-runtime-l1-1-0.dll")};
    const std::vector<std::byte> host = make_test_pe(hostImports, {});
    const std::vector<std::byte> game = make_test_pe(gameImports, {});
    const std::vector<std::byte> localA = make_test_pe({}, localADelayImports);
    const std::vector<std::byte> localB = make_test_pe(localBImports, {});
    const std::array dependencies = {
        cue::package::RuntimePeImageView{"LocalA.dll", localA},
        cue::package::RuntimePeImageView{"LocalB.dll", localB}};
    auto valid = cue::package::validate_runtime_dependency_closure(
        cue::BuildConfiguration::Debug, {"CueRuntimeHost.exe", host}, {"CueGameModule.dll", game}, dependencies,
        a_assertContext);

    std::vector<std::byte> namedImportGame = game;
    write_u64(namedImportGame, 0x700U, 0x1a00U);
    write_u16(namedImportGame, 0xc00U, 7U);
    write_ascii(namedImportGame, 0xc02U, "CueImportedFunction");
    auto validNamedImport = cue::package::validate_runtime_dependency_closure(
        cue::BuildConfiguration::Debug, {"CueRuntimeHost.exe", host}, {"CueGameModule.dll", namedImportGame},
        dependencies, a_assertContext);

    constexpr std::array spacedImports = {std::string_view("My Library.dll")};
    const std::vector<std::byte> spacedGame = make_test_pe(spacedImports, {});
    const std::vector<std::byte> spacedDependencyImage = make_test_pe({}, {});
    const std::array spacedDependency = {
        cue::package::RuntimePeImageView{"My Library.dll", spacedDependencyImage}};
    auto validSpacedImport = cue::package::validate_runtime_dependency_closure(
        cue::BuildConfiguration::Debug, {"CueRuntimeHost.exe", host}, {"CueGameModule.dll", spacedGame},
        spacedDependency, a_assertContext);

    constexpr std::array missingImports = {std::string_view("Missing.dll")};
    const std::vector<std::byte> missingGame = make_test_pe(missingImports, {});
    auto missing = cue::package::validate_runtime_dependency_closure(
        cue::BuildConfiguration::Debug, {"CueRuntimeHost.exe", host}, {"CueGameModule.dll", missingGame}, {},
        a_assertContext);

    constexpr std::array releaseRuntime = {std::string_view("msvcp140.dll")};
    const std::vector<std::byte> mixedGame = make_test_pe(releaseRuntime, {});
    auto mixed = cue::package::validate_runtime_dependency_closure(
        cue::BuildConfiguration::Debug, {"CueRuntimeHost.exe", host}, {"CueGameModule.dll", mixedGame}, {},
        a_assertContext);

    constexpr std::array hostLocalImport = {std::string_view("LocalA.dll")};
    const std::vector<std::byte> invalidHost = make_test_pe(hostLocalImport, {});
    auto hostBoundary = cue::package::validate_runtime_dependency_closure(
        cue::BuildConfiguration::Debug, {"CueRuntimeHost.exe", invalidHost}, {"CueGameModule.dll", game}, dependencies,
        a_assertContext);

    const std::vector<std::byte> noImports = make_test_pe({}, {});
    const std::array unreachableDependencies = {
        cue::package::RuntimePeImageView{"LocalA.dll", noImports},
        cue::package::RuntimePeImageView{"LocalB.dll", noImports}};
    auto unreachable = cue::package::validate_runtime_dependency_closure(
        cue::BuildConfiguration::Debug, {"CueRuntimeHost.exe", host}, {"CueGameModule.dll", game},
        unreachableDependencies, a_assertContext);

    const std::vector<std::byte> forwarded = make_test_pe({}, {}, true);
    const std::array forwardedDependency = {cue::package::RuntimePeImageView{"LocalA.dll", forwarded}};
    auto forwarder = cue::package::validate_runtime_dependency_closure(
        cue::BuildConfiguration::Debug, {"CueRuntimeHost.exe", host}, {"CueGameModule.dll", game},
        forwardedDependency, a_assertContext);

    const std::vector<std::byte> malformed(128U, std::byte{0U});
    auto invalidPe = cue::package::validate_runtime_dependency_closure(
        cue::BuildConfiguration::Debug, {"CueRuntimeHost.exe", malformed}, {"CueGameModule.dll", game}, dependencies,
        a_assertContext);

    std::vector<std::byte> missingImportThunk = game;
    write_u32(missingImportThunk, 0x200U, 0U);
    write_u32(missingImportThunk, 0x200U + 16U, 0U);
    auto invalidImportThunk = cue::package::validate_runtime_dependency_closure(
        cue::BuildConfiguration::Debug, {"CueRuntimeHost.exe", host}, {"CueGameModule.dll", missingImportThunk},
        dependencies, a_assertContext);

    std::vector<std::byte> shortImportAddressTable = game;
    write_u64(shortImportAddressTable, 0x800U, 0U);
    auto mismatchedImportThunkCount = cue::package::validate_runtime_dependency_closure(
        cue::BuildConfiguration::Debug, {"CueRuntimeHost.exe", host},
        {"CueGameModule.dll", shortImportAddressTable}, dependencies, a_assertContext);

    std::vector<std::byte> invalidImportByName = game;
    write_u64(invalidImportByName, 0x700U, 0x00ffffffU);
    auto invalidImportNameRva = cue::package::validate_runtime_dependency_closure(
        cue::BuildConfiguration::Debug, {"CueRuntimeHost.exe", host}, {"CueGameModule.dll", invalidImportByName},
        dependencies, a_assertContext);

    std::vector<std::byte> unterminatedImportThunk = game;
    write_u32(unterminatedImportThunk, 0x200U, 0x1df8U);
    write_u64(unterminatedImportThunk, 0xff8U, 0x8000000000000001ULL);
    auto unterminatedImport = cue::package::validate_runtime_dependency_closure(
        cue::BuildConfiguration::Debug, {"CueRuntimeHost.exe", host},
        {"CueGameModule.dll", unterminatedImportThunk}, dependencies, a_assertContext);

    std::vector<std::byte> missingDelayThunk = localA;
    write_u32(missingDelayThunk, 0x400U + 12U, 0U);
    auto invalidDelayThunk = cue::package::validate_runtime_dependency_closure(
        cue::BuildConfiguration::Debug, {"CueRuntimeHost.exe", host}, {"CueGameModule.dll", game},
        std::array{cue::package::RuntimePeImageView{"LocalA.dll", missingDelayThunk},
                   cue::package::RuntimePeImageView{"LocalB.dll", localB}},
        a_assertContext);

    std::vector<std::byte> shortDelayAddressTable = localA;
    write_u64(shortDelayAddressTable, 0xa00U, 0U);
    auto mismatchedDelayThunkCount = cue::package::validate_runtime_dependency_closure(
        cue::BuildConfiguration::Debug, {"CueRuntimeHost.exe", host}, {"CueGameModule.dll", game},
        std::array{cue::package::RuntimePeImageView{"LocalA.dll", shortDelayAddressTable},
                   cue::package::RuntimePeImageView{"LocalB.dll", localB}},
        a_assertContext);

    std::vector<std::byte> excessiveSections = game;
    write_u16(excessiveSections, 0x86U, 97U);
    auto invalidSectionCount = cue::package::validate_runtime_dependency_closure(
        cue::BuildConfiguration::Debug, {"CueRuntimeHost.exe", host}, {"CueGameModule.dll", excessiveSections},
        dependencies, a_assertContext);

    std::vector<std::byte> headerCrossingHost = host;
    write_u32(headerCrossingHost, 0x110U, 0x1f8U);
    write_u32(headerCrossingHost, 0x114U, 20U);
    std::fill(headerCrossingHost.begin() + 0x1f8U, headerCrossingHost.begin() + 0x20cU, std::byte{0U});
    auto invalidHeaderRange = cue::package::validate_runtime_dependency_closure(
        cue::BuildConfiguration::Debug, {"CueRuntimeHost.exe", headerCrossingHost},
        {"CueGameModule.dll", game}, dependencies, a_assertContext);

    std::vector<std::byte> headerSectionAliasHost = host;
    write_u32(headerSectionAliasHost, 0x110U, 0x1f8U);
    write_u32(headerSectionAliasHost, 0x114U, 20U);
    write_u32(headerSectionAliasHost, 0x194U, 0x1f8U);
    write_u32(headerSectionAliasHost, 0x19cU, 0x300U);
    std::fill(headerSectionAliasHost.begin() + 0x300U, headerSectionAliasHost.begin() + 0x314U, std::byte{0U});
    auto invalidHeaderSectionAlias = cue::package::validate_runtime_dependency_closure(
        cue::BuildConfiguration::Debug, {"CueRuntimeHost.exe", headerSectionAliasHost},
        {"CueGameModule.dll", game}, dependencies, a_assertContext);
    return valid && validNamedImport && validSpacedImport &&
           is_package_error(missing, cue::package::PackageError::RuntimeDependencyViolation) &&
           is_package_error(mixed, cue::package::PackageError::RuntimeDependencyViolation) &&
           is_package_error(hostBoundary, cue::package::PackageError::RuntimeDependencyViolation) &&
           is_package_error(unreachable, cue::package::PackageError::RuntimeDependencyViolation) &&
           is_package_error(forwarder, cue::package::PackageError::RuntimeDependencyViolation) &&
           is_package_error(invalidPe, cue::package::PackageError::InvalidPortableExecutable) &&
           is_package_error(invalidImportThunk, cue::package::PackageError::InvalidPortableExecutable) &&
           is_package_error(mismatchedImportThunkCount, cue::package::PackageError::InvalidPortableExecutable) &&
           is_package_error(invalidImportNameRva, cue::package::PackageError::InvalidPortableExecutable) &&
           is_package_error(unterminatedImport, cue::package::PackageError::InvalidPortableExecutable) &&
           is_package_error(invalidDelayThunk, cue::package::PackageError::InvalidPortableExecutable) &&
           is_package_error(mismatchedDelayThunkCount, cue::package::PackageError::InvalidPortableExecutable) &&
           is_package_error(invalidSectionCount, cue::package::PackageError::InvalidPortableExecutable) &&
           is_package_error(invalidHeaderRange, cue::package::PackageError::InvalidPortableExecutable) &&
           is_package_error(invalidHeaderSectionAlias, cue::package::PackageError::InvalidPortableExecutable);
}

/// @brief Package Root上の存在、Size、SHA-256照合と欠落検出を検証する
[[nodiscard]] bool test_file_verification(const std::filesystem::path &a_testRoot,
                                          const cue::AssertContext &a_assertContext)
{
    std::error_code error;
    std::filesystem::remove_all(a_testRoot, error);
    if (error || !std::filesystem::create_directories(a_testRoot, error) || error)
    {
        return false;
    }
    const cue::package::PackageManifest manifest = make_manifest(a_assertContext);
    write_manifest_files(a_testRoot, manifest);
    auto verified = cue::package::verify_package_manifest_files(a_testRoot.generic_string(), manifest, a_assertContext);
#if defined(_WIN32)
    const std::filesystem::path guardedPath = a_testRoot / "Game/CueGameModule.dll";
    const HANDLE writer = CreateFileW(guardedPath.c_str(), GENERIC_WRITE,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
    require(writer != nullptr && writer != INVALID_HANDLE_VALUE);
    auto writeShared =
        cue::package::verify_package_manifest_files(a_testRoot.generic_string(), manifest, a_assertContext);
    require(CloseHandle(writer) != FALSE);
#endif
    write_file(a_testRoot, "Game/CueGameModule.dll", "y");
    auto mismatched =
        cue::package::verify_package_manifest_files(a_testRoot.generic_string(), manifest, a_assertContext);
    write_file(a_testRoot, "Game/CueGameModule.dll", "x");
    require(std::filesystem::remove(a_testRoot / "Data/CueProject.runtime.json", error) && !error);
    auto missing = cue::package::verify_package_manifest_files(a_testRoot.generic_string(), manifest, a_assertContext);
    std::filesystem::remove_all(a_testRoot, error);
    return verified &&
#if defined(_WIN32)
           is_package_error(writeShared, cue::package::PackageError::PackageFileMissing) &&
#endif
           is_package_error(mismatched, cue::package::PackageError::PackageFileMismatch) &&
           is_package_error(missing, cue::package::PackageError::PackageFileMissing) && !error;
}
} // namespace

/// @brief Package Manifest、Runtime Dependency Inventory、File照合契約を検証する
int main(int a_argumentCount, char **a_arguments)
{
    require(a_argumentCount == 2);
    TestFatalHandler fatalHandler;
    std::vector<std::unique_ptr<cue::LogSink>> sinks;
    cue::Logger logger(fatalHandler, std::move(sinks));
    cue::AssertContext assertContext(logger, fatalHandler);
    const std::filesystem::path testRoot = std::filesystem::path(a_arguments[1]) / "CuePackageManifestTests";
    return test_manifest_wire_contract(assertContext) && test_manifest_validation(assertContext) &&
                   test_runtime_dependency_inventory(assertContext) &&
                   test_runtime_dependency_closure(assertContext) && test_file_verification(testRoot, assertContext)
               ? 0
               : 1;
}
