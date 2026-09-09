#include <Cue/Package/Error.h>
#include <Cue/Package/Manifest.h>

#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Fatal.h>
#include <Cue/Foundation/Log.h>

#if defined(_WIN32)
#include <Windows.h>
#endif

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
    return is_package_error(unsupported, cue::package::PackageError::UnsupportedPackageManifestVersion) &&
           is_package_error(unknown, cue::package::PackageError::InvalidPackageManifest) &&
           is_package_error(invalidRole, cue::package::PackageError::InvalidPackageManifest) &&
           is_package_error(overLimit, cue::package::PackageError::PackageManifestResourceLimitExceeded);
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
           is_package_error(zeroSize, cue::package::PackageError::PackageManifestResourceLimitExceeded) &&
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

    return inventory && inventory.try_value()->size() == 2U &&
           inventory.try_value()->front().relative_path() == "Runtime/A.dll" &&
           inventory.try_value()->back().relative_path() == "Runtime/Z.dll" &&
           is_package_error(configurationMix, cue::package::PackageError::InvalidPackageManifest) &&
           is_package_error(duplicateAlias, cue::package::PackageError::InvalidPackageManifest) &&
           is_package_error(role, cue::package::PackageError::InvalidPackageManifest) &&
           is_package_error(overLimit, cue::package::PackageError::PackageManifestResourceLimitExceeded);
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
                   test_runtime_dependency_inventory(assertContext) && test_file_verification(testRoot, assertContext)
               ? 0
               : 1;
}
