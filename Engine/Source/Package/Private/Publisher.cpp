#include <Cue/Package/Publisher.h>

#include "ManifestVerification.h"
#include "PublisherPath.h"
#include "Sha256.h"

#include <Cue/Foundation/Assert.h>
#include <Cue/IO/Error.h>
#if defined(_WIN32)
#include <Cue/IO/Windows/WindowsFilesystem.h>
#endif
#include <Cue/Package/Error.h>

#if defined(_WIN32)
#include <Windows.h>
#endif

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <utility>

namespace
{
#if defined(_WIN32)
/// @brief Windows Directory Handleを単独所有する
class DirectoryHandle final
{
  public:
    /// @brief Handle所有権を取得する
    explicit DirectoryHandle(HANDLE a_handle) noexcept : m_handle(a_handle)
    {
    }
    DirectoryHandle(const DirectoryHandle &) = delete;
    DirectoryHandle &operator=(const DirectoryHandle &) = delete;
    /// @brief 所有Handleを閉じる
    ~DirectoryHandle()
    {
        if (is_valid())
        {
            static_cast<void>(CloseHandle(m_handle));
        }
    }
    /// @brief HandleがWindows Objectを示すか返す
    [[nodiscard]] bool is_valid() const noexcept
    {
        return m_handle != nullptr && m_handle != INVALID_HANDLE_VALUE;
    }
    /// @brief 借用Native Handleを返す
    [[nodiscard]] HANDLE get() const noexcept
    {
        return m_handle;
    }

  private:
    HANDLE m_handle = INVALID_HANDLE_VALUE;
};

/// @brief UTF-8 Path文字列をWindows filesystem Pathへ変換する
[[nodiscard]] std::filesystem::path native_path(std::string_view a_path)
{
    std::u8string value;
    value.reserve(a_path.size());
    for (const char byte : a_path)
    {
        value.push_back(static_cast<char8_t>(byte));
    }
    return std::filesystem::path(std::move(value));
}

/// @brief Absolute Windows PathをProcess manifest非依存のExtended-length形式へ変換する
[[nodiscard]] std::optional<std::wstring> absolute_extended_windows_path(std::string_view a_path)
{
    const std::filesystem::path path = native_path(a_path);
    const std::wstring input = path.native();
    std::wstring absolute;
    if (input.starts_with(L"\\\\?\\"))
    {
        absolute = input;
    }
    else
    {
        if (!path.is_absolute())
        {
            return std::nullopt;
        }
        const DWORD required = GetFullPathNameW(input.c_str(), 0U, nullptr, nullptr);
        if (required == 0U)
        {
            return std::nullopt;
        }
        absolute.resize(required);
        const DWORD written = GetFullPathNameW(input.c_str(), required, absolute.data(), nullptr);
        if (written == 0U || written >= required)
        {
            return std::nullopt;
        }
        absolute.resize(written);
        if (absolute.starts_with(L"\\\\"))
        {
            absolute.erase(0U, 2U);
            absolute.insert(0U, L"\\\\?\\UNC\\");
        }
        else
        {
            absolute.insert(0U, L"\\\\?\\");
        }
    }
    constexpr std::size_t maximumWindowsPathLength = 32767U;
    if (absolute.size() >= maximumWindowsPathLength)
    {
        return std::nullopt;
    }
    return absolute;
}

/// @brief 既存Directoryを開きWindowsが返す最終Native Pathを取得する
[[nodiscard]] std::optional<std::wstring> final_windows_directory_path(std::string_view a_path)
{
    const std::optional<std::wstring> path = absolute_extended_windows_path(a_path);
    if (!path)
    {
        return std::nullopt;
    }
    DirectoryHandle handle(CreateFileW(path->c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                       OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (!handle.is_valid())
    {
        return std::nullopt;
    }
    BY_HANDLE_FILE_INFORMATION information{};
    if (GetFileInformationByHandle(handle.get(), &information) == FALSE ||
        (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0U)
    {
        return std::nullopt;
    }
    const DWORD required = GetFinalPathNameByHandleW(handle.get(), nullptr, 0U, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (required == 0U)
    {
        return std::nullopt;
    }
    std::wstring result(required, L'\0');
    const DWORD written =
        GetFinalPathNameByHandleW(handle.get(), result.data(), required, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (written == 0U || written >= required)
    {
        return std::nullopt;
    }
    result.resize(written);
    return result;
}

/// @brief Windows Path文字列を大小文字非依存のOrdinal比較で照合する
[[nodiscard]] bool equals_windows_path(std::wstring_view a_left, std::wstring_view a_right) noexcept
{
    if (a_left.size() != a_right.size() || a_left.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
    {
        return false;
    }
    return CompareStringOrdinal(a_left.data(), static_cast<int>(a_left.size()), a_right.data(),
                                static_cast<int>(a_right.size()), TRUE) == CSTR_EQUAL;
}

/// @brief Windows Wide PathをUTF-8 slash表現へ変換する
[[nodiscard]] std::string generic_utf8_text(std::wstring_view a_path)
{
    const std::u8string value = std::filesystem::path(a_path).generic_u8string();
    return std::string(reinterpret_cast<const char *>(value.data()), value.size());
}

/// @brief Windowsが解決したProject RootをNative UTF-8表現で返す
[[nodiscard]] std::optional<std::string> canonical_project_root(std::string_view a_projectRoot)
{
    const std::optional<std::wstring> root = final_windows_directory_path(a_projectRoot);
    if (!root)
    {
        return std::nullopt;
    }
    const std::u8string value = std::filesystem::path(*root).u8string();
    return std::string(reinterpret_cast<const char *>(value.data()), value.size());
}
#endif

/// @brief Package Publisher内の予期しない例外をFatal境界へ渡す
[[noreturn]] void terminate_publisher_exception(const cue::AssertContext &a_assertContext) noexcept
{
    a_assertContext.fatal_handler().terminate("Runtime Package publication failed unexpectedly");
    std::abort();
}

/// @brief SHA-256 Digestをlowercase hexadecimalへ変換する
[[nodiscard]] std::string digest_text(const cue::package_private::Sha256Digest &a_digest)
{
    constexpr char digits[] = "0123456789abcdef";
    std::string text;
    text.reserve(a_digest.size() * 2U);
    for (const std::uint8_t value : a_digest)
    {
        text.push_back(digits[(value >> 4U) & 0x0FU]);
        text.push_back(digits[value & 0x0FU]);
    }
    return text;
}

/// @brief Manifestの主要IdentityとInventory規模をReport用所有値へ変換する
[[nodiscard]] cue::package::PackageManifestSummary make_summary(const cue::package::PackageManifest &a_manifest)
{
    std::uint64_t inventoryBytes = 0U;
    for (const cue::package::PackageFileEntry &file : a_manifest.files())
    {
        inventoryBytes += file.byte_size();
    }
    return {std::string(a_manifest.project_id()),
            a_manifest.engine_version(),
            a_manifest.configuration(),
            std::string(a_manifest.startup_scene_asset_id()),
            a_manifest.files().size(),
            inventoryBytes};
}

/// @brief StageとOutcomeに対応する所有Reportを構築する
[[nodiscard]] cue::package::PackagePublishReport make_report(
    cue::package::PackagePublishStage a_stage, cue::package::PackagePublishOutcome a_outcome,
    const cue::RelativePath &a_destination, const cue::package::PackageManifest &a_manifest,
    std::optional<cue::Error> a_error = std::nullopt, std::optional<cue::StagingArea> a_recoveryStaging = std::nullopt)
{
    return {a_stage,
            a_outcome,
            std::string(a_destination.text()),
            make_summary(a_manifest),
            std::move(a_error),
            std::move(a_recoveryStaging)};
}

/// @brief StagingまたはDestination RootとPackage相対Pathを一つの検証済みRelativePathへ結合する
[[nodiscard]] cue::Result<cue::RelativePath> make_package_path(const cue::RelativePath &a_root,
                                                               std::string_view a_suffix,
                                                               const cue::AssertContext &a_assertContext) noexcept
{
    try
    {
        std::string path(a_root.text());
        path.push_back('/');
        path.append(a_suffix);
        return cue::RelativePath::parse(path, a_assertContext);
    }
    catch (...)
    {
        terminate_publisher_exception(a_assertContext);
    }
}

/// @brief Package Fileの親DirectoryがあればRoot配下へ作成する
[[nodiscard]] cue::Result<void> create_parent_directories(cue::FilesystemRoot &a_filesystem,
                                                          const cue::RelativePath &a_root,
                                                          std::string_view a_relativePath,
                                                          const cue::AssertContext &a_assertContext) noexcept
{
    const std::size_t separator = a_relativePath.rfind('/');
    if (separator == std::string_view::npos)
    {
        return cue::Result<void>::success();
    }
    auto parent = make_package_path(a_root, a_relativePath.substr(0U, separator), a_assertContext);
    if (!parent)
    {
        return cue::Result<void>::failure(std::move(*parent.try_error()));
    }
    return a_filesystem.create_directories(*parent.try_value());
}

/// @brief Payload EntryがManifest EntryとByte Identityまで一致するか返す
[[nodiscard]] bool entry_matches(const cue::package::PackageFileEntry &a_expected,
                                 const cue::package::PackageFileEntry &a_actual) noexcept
{
    return a_expected.role() == a_actual.role() && a_expected.relative_path() == a_actual.relative_path() &&
           a_expected.byte_size() == a_actual.byte_size() && a_expected.sha256() == a_actual.sha256();
}

/// @brief Manifestの全Entryが一意なPayloadと一致することを検証する
[[nodiscard]] cue::Result<void> validate_payloads(const cue::package::PackageManifest &a_manifest,
                                                  std::span<const cue::package::PackageFilePayload> a_payloads,
                                                  const cue::AssertContext &a_assertContext) noexcept
{
    if (a_manifest.files().size() != a_payloads.size())
    {
        return cue::Result<void>::failure(
            cue::package::make_package_error(a_assertContext, cue::package::PackageError::PackageFileMismatch,
                                             "Package payload count differs from the Manifest"));
    }
    for (const cue::package::PackageFileEntry &expected : a_manifest.files())
    {
        const std::size_t matches =
            static_cast<std::size_t>(std::count_if(a_payloads.begin(), a_payloads.end(),
                                                   /// @brief 現在のManifest Entryと同じPayloadを数える
                                                   [&](const cue::package::PackageFilePayload &a_payload) noexcept
                                                   { return entry_matches(expected, a_payload.entry()); }));
        if (matches != 1U)
        {
            return cue::Result<void>::failure(
                cue::package::make_package_error(a_assertContext, cue::package::PackageError::PackageFileMismatch,
                                                 "Package payload identity differs from the Manifest"));
        }
    }
    return cue::Result<void>::success();
}

/// @brief 二つのRoot相対Path要素をslash一個で連結する
[[nodiscard]] std::string join_relative(std::string_view a_left, std::string_view a_right);

/// @brief Root配下のManifestと全PayloadをCanonical Byte列へ再検証する
[[nodiscard]] cue::Result<void> verify_package_snapshot(
    cue::FilesystemRoot &a_filesystem, const cue::RelativePath &a_root, const cue::package::PackageManifest &a_manifest,
    std::span<const cue::package::PackageFilePayload> a_payloads, std::string_view a_manifestBytes,
    std::optional<std::string_view> a_filesystemRoot, const cue::AssertContext &a_assertContext) noexcept
{
    auto manifestPath = make_package_path(a_root, "CuePackage.json", a_assertContext);
    if (!manifestPath)
    {
        return cue::Result<void>::failure(std::move(*manifestPath.try_error()));
    }
    auto manifestFile = a_filesystem.read_file(*manifestPath.try_value(), cue::package::k_maximumPackageManifestBytes);
    if (!manifestFile)
    {
        return cue::Result<void>::failure(std::move(*manifestFile.try_error()));
    }
    const std::string_view manifestText(reinterpret_cast<const char *>(manifestFile.try_value()->data()),
                                        manifestFile.try_value()->size());
    if (manifestText != a_manifestBytes)
    {
        return cue::Result<void>::failure(
            cue::package::make_package_error(a_assertContext, cue::package::PackageError::PackageFileMismatch,
                                             "Published Package Manifest bytes differ from the staged snapshot"));
    }
    auto reparsed = cue::package::parse_package_manifest(manifestText, a_assertContext);
    if (!reparsed)
    {
        return cue::Result<void>::failure(std::move(*reparsed.try_error()));
    }
    auto canonical = cue::package::serialize_package_manifest(*reparsed.try_value(), a_assertContext);
    if (!canonical || *canonical.try_value() != a_manifestBytes)
    {
        return cue::Result<void>::failure(
            canonical
                ? cue::package::make_package_error(a_assertContext, cue::package::PackageError::InvalidPackageManifest,
                                                   "Published Package Manifest is not canonical")
                : std::move(*canonical.try_error()));
    }

    for (const cue::package::PackageFilePayload &payload : a_payloads)
    {
        auto filePath = make_package_path(a_root, payload.entry().relative_path(), a_assertContext);
        if (!filePath)
        {
            return cue::Result<void>::failure(std::move(*filePath.try_error()));
        }
        auto bytes = a_filesystem.read_file(*filePath.try_value(), payload.bytes().size());
        if (!bytes)
        {
            return cue::Result<void>::failure(std::move(*bytes.try_error()));
        }
        if (bytes.try_value()->size() != payload.bytes().size() ||
            !std::equal(bytes.try_value()->begin(), bytes.try_value()->end(), payload.bytes().begin()))
        {
            return cue::Result<void>::failure(
                cue::package::make_package_error(a_assertContext, cue::package::PackageError::PackageFileMismatch,
                                                 "Package file bytes differ from the immutable payload snapshot"));
        }
    }
    if (a_manifest.schema_version() == cue::package::k_monolithicPackageManifestSchemaVersion)
    {
        if (!a_filesystemRoot)
        {
            return cue::Result<void>::failure(cue::package::make_package_error(
                a_assertContext, cue::package::PackageError::InvalidPackagePath,
                "Monolithic Package verification requires its native filesystem root"));
        }
        const std::string nativeRoot = join_relative(*a_filesystemRoot, a_root.text());
        auto inventory = cue::package_private::verify_monolithic_package_tree(nativeRoot, a_manifest, a_assertContext);
        if (!inventory)
        {
            return inventory;
        }
    }
    return cue::Result<void>::success();
}

/// @brief Primary Errorを維持してStagingをRollbackし、失敗時はRecovery所有権を返す
[[nodiscard]] std::optional<cue::StagingArea> rollback_staging(cue::FilesystemRoot &a_filesystem,
                                                               cue::StagingArea &a_staging, cue::Error &a_primary,
                                                               const cue::AssertContext &a_assertContext) noexcept
{
    auto rollback = a_filesystem.rollback_staging_area(std::move(a_staging));
    if (rollback)
    {
        return std::nullopt;
    }
    a_primary.append_secondary_diagnostics(a_assertContext, *rollback.try_error(), "Package staging rollback failed",
                                           "Rollback");
    return std::optional<cue::StagingArea>(std::move(a_staging));
}

/// @brief IO Errorが公開済みでRollback不能なDurabilityUnknownか返す
[[nodiscard]] bool is_durability_unknown(const cue::Error &a_error) noexcept
{
    return a_error.root_code().domain() == "Cue.IO" &&
           a_error.root_code().value() == static_cast<std::int64_t>(cue::IoError::DurabilityUnknown);
}

/// @brief ASCIIまたはUTF-8文字列を同一Byte列へ変換して所有する
[[nodiscard]] std::vector<std::byte> copy_bytes(std::string_view a_text)
{
    const std::span<const char> characters(a_text.data(), a_text.size());
    const std::span<const std::byte> raw = std::as_bytes(characters);
    return std::vector<std::byte>(raw.begin(), raw.end());
}

/// @brief 二つのRoot相対Path要素をslash一個で連結する
[[nodiscard]] std::string join_relative(std::string_view a_left, std::string_view a_right)
{
    std::string path(a_left);
    if (!path.empty() && path.back() != '/' && path.back() != '\\')
    {
        path.push_back('/');
    }
    path.append(a_right);
    return path;
}

} // namespace

namespace cue::package_private
{
Result<RelativePath> make_project_relative_path(std::string_view a_projectRoot, std::string_view a_absoluteLocator,
                                                const AssertContext &a_assertContext) noexcept
{
    try
    {
#if defined(_WIN32)
        const std::optional<std::wstring> root = final_windows_directory_path(a_projectRoot);
        const std::optional<std::wstring> locator = final_windows_directory_path(a_absoluteLocator);
        if (!root || !locator || locator->size() <= root->size() ||
            !equals_windows_path(*root, std::wstring_view(*locator).substr(0U, root->size())))
        {
            return Result<RelativePath>::failure(
                package::make_package_error(a_assertContext, package::PackageError::InvalidPackagePath,
                                            "Shipping Product artifact directory is outside the package project root"));
        }
        std::size_t relativeOffset = root->size();
        if (root->back() != L'\\' && root->back() != L'/')
        {
            if ((*locator)[relativeOffset] != L'\\' && (*locator)[relativeOffset] != L'/')
            {
                return Result<RelativePath>::failure(package::make_package_error(
                    a_assertContext, package::PackageError::InvalidPackagePath,
                    "Shipping Product artifact directory is outside the package project root"));
            }
            ++relativeOffset;
        }
        return RelativePath::parse(generic_utf8_text(std::wstring_view(*locator).substr(relativeOffset)),
                                   a_assertContext);
#else
        return Result<RelativePath>::failure(
            package::make_package_error(a_assertContext, package::PackageError::InvalidPackagePath,
                                        "Shipping Product artifact locator conversion requires Windows x64"));
#endif
    }
    catch (...)
    {
        terminate_publisher_exception(a_assertContext);
    }
}
} // namespace cue::package_private

namespace cue::package
{
bool PackagePublishReport::succeeded() const noexcept
{
    return outcome == PackagePublishOutcome::Committed && !error.has_value();
}

PackageFilePayload::PackageFilePayload(PackageFileEntry a_entry, std::vector<std::byte> a_bytes) noexcept
    : m_entry(std::move(a_entry)), m_bytes(std::move(a_bytes))
{
}

Result<PackageFilePayload> PackageFilePayload::create(PackageFileRole a_role, std::string a_relativePath,
                                                      std::vector<std::byte> a_bytes,
                                                      const AssertContext &a_assertContext) noexcept
{
    try
    {
        const std::string hash = digest_text(package_private::compute_sha256(a_bytes));
        auto entry = PackageFileEntry::create(a_role, std::move(a_relativePath), a_bytes.size(), hash, a_assertContext);
        if (!entry)
        {
            return Result<PackageFilePayload>::failure(std::move(*entry.try_error()));
        }
        return Result<PackageFilePayload>::success(
            PackageFilePayload(std::move(*entry.try_value()), std::move(a_bytes)));
    }
    catch (...)
    {
        terminate_publisher_exception(a_assertContext);
    }
}

const PackageFileEntry &PackageFilePayload::entry() const noexcept
{
    return m_entry;
}

std::span<const std::byte> PackageFilePayload::bytes() const noexcept
{
    return m_bytes;
}

void PackageCancellation::request_cancel() noexcept
{
    State expected = State::Active;
    static_cast<void>(m_state.compare_exchange_strong(expected, State::CancelRequested, std::memory_order_acq_rel,
                                                      std::memory_order_acquire));
}

bool PackageCancellation::is_cancel_requested() const noexcept
{
    return m_state.load(std::memory_order_acquire) == State::CancelRequested;
}

bool PackageCancellation::try_authorize() const noexcept
{
    State expected = State::Active;
    return m_state.compare_exchange_strong(expected, State::PublishStarted, std::memory_order_acq_rel,
                                           std::memory_order_acquire);
}

namespace
{
/// @brief 検証済みPayloadを任意のPackage Schema用Native Root情報付きでAtomic公開する
[[nodiscard]] PackagePublishReport publish_runtime_package_impl(
    FilesystemRoot &a_filesystem, const RelativePath &a_destination, const PackageManifest &a_manifest,
    std::span<const PackageFilePayload> a_payloads, std::optional<std::string_view> a_filesystemRoot,
    const PackageCancellation &a_cancellation, const AssertContext &a_assertContext) noexcept
{
    try
    {
        if (a_manifest.schema_version() == k_monolithicPackageManifestSchemaVersion && !a_filesystemRoot)
        {
            return make_report(PackagePublishStage::ValidateInput, PackagePublishOutcome::NotPublished, a_destination,
                               a_manifest,
                               make_package_error(a_assertContext, PackageError::InvalidPackagePath,
                                                  "Monolithic Package publication requires its native root"));
        }
        auto payloadsValid = validate_payloads(a_manifest, a_payloads, a_assertContext);
        if (!payloadsValid)
        {
            return make_report(PackagePublishStage::ValidateInput, PackagePublishOutcome::NotPublished, a_destination,
                               a_manifest, std::move(*payloadsValid.try_error()));
        }
        auto manifest = serialize_package_manifest(a_manifest, a_assertContext);
        if (!manifest)
        {
            return make_report(PackagePublishStage::ValidateInput, PackagePublishOutcome::NotPublished, a_destination,
                               a_manifest, std::move(*manifest.try_error()));
        }
        auto destinationType = a_filesystem.query_entry(a_destination);
        if (!destinationType)
        {
            return make_report(PackagePublishStage::ValidateInput, PackagePublishOutcome::NotPublished, a_destination,
                               a_manifest, std::move(*destinationType.try_error()));
        }
        if (*destinationType.try_value() != EntryType::Missing)
        {
            return make_report(
                PackagePublishStage::ValidateInput, PackagePublishOutcome::NotPublished, a_destination, a_manifest,
                make_io_error(a_assertContext, IoError::AlreadyExists, "Package destination already exists"));
        }
        if (a_cancellation.is_cancel_requested())
        {
            return make_report(PackagePublishStage::ValidateInput, PackagePublishOutcome::NotPublished, a_destination,
                               a_manifest,
                               make_package_error(a_assertContext, PackageError::PackageCancelled,
                                                  "Package publication was cancelled"));
        }

        auto stagingResult = a_filesystem.create_staging_area(a_destination);
        if (!stagingResult)
        {
            return make_report(PackagePublishStage::CreateStaging, PackagePublishOutcome::NotPublished, a_destination,
                               a_manifest, std::move(*stagingResult.try_error()));
        }
        StagingArea staging = std::move(*stagingResult.try_value());

        for (const PackageFilePayload &payload : a_payloads)
        {
            if (a_cancellation.is_cancel_requested())
            {
                Error primary = make_package_error(a_assertContext, PackageError::PackageCancelled,
                                                   "Package publication was cancelled");
                auto recovery = rollback_staging(a_filesystem, staging, primary, a_assertContext);
                return make_report(PackagePublishStage::WriteContent, PackagePublishOutcome::NotPublished,
                                   a_destination, a_manifest, std::move(primary), std::move(recovery));
            }
            auto parent = create_parent_directories(a_filesystem, staging.path(), payload.entry().relative_path(),
                                                    a_assertContext);
            auto path = make_package_path(staging.path(), payload.entry().relative_path(), a_assertContext);
            if (!parent || !path)
            {
                Error primary = parent ? std::move(*path.try_error()) : std::move(*parent.try_error());
                auto recovery = rollback_staging(a_filesystem, staging, primary, a_assertContext);
                return make_report(PackagePublishStage::WriteContent, PackagePublishOutcome::NotPublished,
                                   a_destination, a_manifest, std::move(primary), std::move(recovery));
            }
            auto written = a_filesystem.write_file_atomic(*path.try_value(), payload.bytes());
            if (!written)
            {
                Error primary = std::move(*written.try_error());
                auto recovery = rollback_staging(a_filesystem, staging, primary, a_assertContext);
                return make_report(PackagePublishStage::WriteContent, PackagePublishOutcome::NotPublished,
                                   a_destination, a_manifest, std::move(primary), std::move(recovery));
            }
        }

        auto manifestPath = make_package_path(staging.path(), "CuePackage.json", a_assertContext);
        const std::span<const char> manifestCharacters(manifest.try_value()->data(), manifest.try_value()->size());
        auto manifestWritten =
            manifestPath ? a_filesystem.write_file_atomic(*manifestPath.try_value(), std::as_bytes(manifestCharacters))
                         : Result<void>::failure(std::move(*manifestPath.try_error()));
        if (!manifestWritten)
        {
            Error primary = std::move(*manifestWritten.try_error());
            auto recovery = rollback_staging(a_filesystem, staging, primary, a_assertContext);
            return make_report(PackagePublishStage::WriteManifest, PackagePublishOutcome::NotPublished, a_destination,
                               a_manifest, std::move(primary), std::move(recovery));
        }

        auto staged = verify_package_snapshot(a_filesystem, staging.path(), a_manifest, a_payloads,
                                              *manifest.try_value(), a_filesystemRoot, a_assertContext);
        if (!staged || a_cancellation.is_cancel_requested())
        {
            Error primary = staged ? make_package_error(a_assertContext, PackageError::PackageCancelled,
                                                        "Package publication was cancelled")
                                   : std::move(*staged.try_error());
            auto recovery = rollback_staging(a_filesystem, staging, primary, a_assertContext);
            return make_report(PackagePublishStage::ValidateStaging, PackagePublishOutcome::NotPublished, a_destination,
                               a_manifest, std::move(primary), std::move(recovery));
        }

        auto published = a_filesystem.publish_staging_area(std::move(staging), a_destination, &a_cancellation);
        if (!published)
        {
            if (a_cancellation.is_cancel_requested())
            {
                Error primary = make_package_error(a_assertContext, PackageError::PackageCancelled,
                                                   "Package publication was cancelled");
                auto recovery = rollback_staging(a_filesystem, staging, primary, a_assertContext);
                return make_report(PackagePublishStage::Publish, PackagePublishOutcome::NotPublished, a_destination,
                                   a_manifest, std::move(primary), std::move(recovery));
            }
            Error primary = std::move(*published.try_error());
            if (is_durability_unknown(primary))
            {
                auto visible = verify_package_snapshot(a_filesystem, a_destination, a_manifest, a_payloads,
                                                       *manifest.try_value(), a_filesystemRoot, a_assertContext);
                if (!visible)
                {
                    primary.append_secondary_diagnostics(a_assertContext, *visible.try_error(),
                                                         "Visible Package revalidation failed", "Revalidation");
                }
                return make_report(PackagePublishStage::Publish, PackagePublishOutcome::PublishedButDurabilityUnknown,
                                   a_destination, a_manifest, std::move(primary));
            }
            auto recovery = rollback_staging(a_filesystem, staging, primary, a_assertContext);
            return make_report(PackagePublishStage::Publish, PackagePublishOutcome::NotPublished, a_destination,
                               a_manifest, std::move(primary), std::move(recovery));
        }

        auto finalSnapshot = verify_package_snapshot(a_filesystem, a_destination, a_manifest, a_payloads,
                                                     *manifest.try_value(), a_filesystemRoot, a_assertContext);
        if (!finalSnapshot)
        {
            return make_report(PackagePublishStage::ValidatePublished,
                               PackagePublishOutcome::PublishedButDurabilityUnknown, a_destination, a_manifest,
                               std::move(*finalSnapshot.try_error()));
        }
        return make_report(PackagePublishStage::Completed, PackagePublishOutcome::Committed, a_destination, a_manifest);
    }
    catch (...)
    {
        terminate_publisher_exception(a_assertContext);
    }
}
} // namespace

PackagePublishReport publish_runtime_package(FilesystemRoot &a_filesystem, const RelativePath &a_destination,
                                             const PackageManifest &a_manifest,
                                             std::span<const PackageFilePayload> a_payloads,
                                             const PackageCancellation &a_cancellation,
                                             const AssertContext &a_assertContext) noexcept
{
    return publish_runtime_package_impl(a_filesystem, a_destination, a_manifest, a_payloads, std::nullopt,
                                        a_cancellation, a_assertContext);
}

Result<PackagePublishReport> publish_monolithic_runtime_package(
    FilesystemRoot &a_projectFilesystem, std::string_view a_projectRoot, BuildArtifactReader &a_artifactReader,
    const BuildArtifactInventory &a_artifact, EngineVersion a_engineVersion,
    const MinimalRuntimeDataPublication &a_runtimeData, const RelativePath &a_destination,
    const PackageCancellation &a_cancellation, const AssertContext &a_assertContext) noexcept
{
    try
    {
        std::optional<std::string> canonicalRoot;
#if defined(_WIN32)
        canonicalRoot = canonical_project_root(a_projectRoot);
        if (!canonicalRoot)
        {
            return Result<PackagePublishReport>::failure(
                make_package_error(a_assertContext, PackageError::InvalidPackagePath,
                                   "Monolithic Package Project Root could not be resolved"));
        }
        auto nativeRoot = create_windows_filesystem_root(*canonicalRoot, a_assertContext);
        auto projectRootIdentity = a_projectFilesystem.root_identity();
        auto nativeRootIdentity = nativeRoot ? nativeRoot.try_value()->get()->root_identity()
                                             : Result<FilesystemIdentity>::failure(std::move(*nativeRoot.try_error()));
        if (!projectRootIdentity || !nativeRootIdentity)
        {
            return Result<PackagePublishReport>::failure(projectRootIdentity
                                                             ? std::move(*nativeRootIdentity.try_error())
                                                             : std::move(*projectRootIdentity.try_error()));
        }
        if (*projectRootIdentity.try_value() != *nativeRootIdentity.try_value())
        {
            return Result<PackagePublishReport>::failure(
                make_package_error(a_assertContext, PackageError::InvalidPackagePath,
                                   "Monolithic Package native root identity differs from the project filesystem"));
        }
#else
        return Result<PackagePublishReport>::failure(
            make_package_error(a_assertContext, PackageError::InvalidPackagePath,
                               "Monolithic Package publication requires the Windows x64 filesystem boundary"));
#endif
        const BuildProfile &profile = a_artifact.profile();
        const std::optional<ShippingTrustMode> trustMode = profile.minimum_trust_mode();
        if (profile.target() != BuildTarget::ShippingProduct ||
            profile.configuration() != BuildConfiguration::Release || !trustMode ||
            *trustMode != ShippingTrustMode::UnsignedLocal || !profile.publisher_key_id().empty())
        {
            return Result<PackagePublishReport>::failure(make_package_error(
                a_assertContext, PackageError::InvalidPackageManifest,
                "Monolithic Package publication requires an unsigned Release Shipping Product artifact"));
        }

        const BuildArtifactFile *executable = nullptr;
        std::size_t distributionPayloadCount = 0U;
        for (const BuildArtifactFile &file : a_artifact.files())
        {
            if (file.purpose == BuildArtifactFilePurpose::DistributionPayload)
            {
                ++distributionPayloadCount;
            }
            if (file.relativePath == "CueGameProduct.exe")
            {
                executable = &file;
            }
        }
        if (distributionPayloadCount != 1U || executable == nullptr ||
            executable->purpose != BuildArtifactFilePurpose::DistributionPayload || executable->byteSize == 0U ||
            executable->byteSize > k_maximumMonolithicExecutableBytes ||
            executable->byteSize > std::numeric_limits<std::size_t>::max())
        {
            return Result<PackagePublishReport>::failure(make_package_error(
                a_assertContext, PackageError::PackageManifestResourceLimitExceeded,
                "Shipping Product executable inventory is invalid or exceeds the Monolithic Package limit"));
        }

        auto acquired = a_artifactReader.acquire_current_read_lease(a_artifact, a_cancellation, std::nullopt);
        if (!acquired)
        {
            return Result<PackagePublishReport>::failure(std::move(*acquired.try_error()));
        }
        if (!acquired.try_value()->has_value())
        {
            return Result<PackagePublishReport>::failure(make_package_error(
                a_assertContext, PackageError::PackageCancelled,
                "Monolithic Package publication was cancelled while waiting for the artifact read lease"));
        }
        std::unique_ptr<BuildArtifactReadLease> artifactReadLease = std::move(**acquired.try_value());
        if (artifactReadLease->project_id() != a_runtimeData.project_id())
        {
            return Result<PackagePublishReport>::failure(
                make_package_error(a_assertContext, PackageError::PackageFileMismatch,
                                   "Shipping Product artifact and Runtime Data Project identities differ"));
        }

        auto artifactDirectory = package_private::make_project_relative_path(
            *canonicalRoot, a_artifact.version_directory(), a_assertContext);
        if (!artifactDirectory)
        {
            return Result<PackagePublishReport>::failure(std::move(*artifactDirectory.try_error()));
        }
        auto executablePath = RelativePath::parse(
            join_relative(artifactDirectory.try_value()->text(), executable->relativePath), a_assertContext);
        if (!executablePath)
        {
            return Result<PackagePublishReport>::failure(std::move(*executablePath.try_error()));
        }

        auto executableBytes = a_projectFilesystem.read_file(
            *executablePath.try_value(), static_cast<std::size_t>(k_maximumMonolithicExecutableBytes));
        if (!executableBytes)
        {
            return Result<PackagePublishReport>::failure(std::move(*executableBytes.try_error()));
        }
        auto executablePayload =
            PackageFilePayload::create(PackageFileRole::ApplicationExecutable, "CueGameProduct.exe",
                                       std::move(*executableBytes.try_value()), a_assertContext);
        if (!executablePayload)
        {
            return Result<PackagePublishReport>::failure(std::move(*executablePayload.try_error()));
        }
        if (executablePayload.try_value()->entry().byte_size() != executable->byteSize ||
            executablePayload.try_value()->entry().sha256() != executable->contentHash)
        {
            return Result<PackagePublishReport>::failure(
                make_package_error(a_assertContext, PackageError::PackageFileMismatch,
                                   "Shipping Product executable bytes differ from the published artifact inventory"));
        }
        artifactReadLease.reset();

        if (a_cancellation.is_cancel_requested())
        {
            return Result<PackagePublishReport>::failure(make_package_error(
                a_assertContext, PackageError::PackageCancelled, "Monolithic Package publication was cancelled"));
        }
        const RuntimeDataFile &projectData = a_runtimeData.project_data();
        const RuntimeDataFile &sceneData = a_runtimeData.startup_scene_data();
        if (projectData.byte_size() > k_maximumMonolithicProjectDataBytes ||
            sceneData.byte_size() > k_maximumMonolithicSceneDataBytes)
        {
            return Result<PackagePublishReport>::failure(
                make_package_error(a_assertContext, PackageError::PackageManifestResourceLimitExceeded,
                                   "Monolithic Package Runtime Data exceeds its role limit"));
        }
        auto projectPayload =
            PackageFilePayload::create(PackageFileRole::ProjectRuntimeData, std::string(projectData.relative_path()),
                                       copy_bytes(projectData.bytes()), a_assertContext);
        auto scenePayload =
            PackageFilePayload::create(PackageFileRole::StartupSceneRuntimeData, std::string(sceneData.relative_path()),
                                       copy_bytes(sceneData.bytes()), a_assertContext);
        if (!projectPayload || !scenePayload)
        {
            return Result<PackagePublishReport>::failure(projectPayload ? std::move(*scenePayload.try_error())
                                                                        : std::move(*projectPayload.try_error()));
        }
        if (projectPayload.try_value()->entry().byte_size() != projectData.byte_size() ||
            projectPayload.try_value()->entry().sha256() != projectData.sha256() ||
            scenePayload.try_value()->entry().byte_size() != sceneData.byte_size() ||
            scenePayload.try_value()->entry().sha256() != sceneData.sha256())
        {
            return Result<PackagePublishReport>::failure(
                make_package_error(a_assertContext, PackageError::PackageFileMismatch,
                                   "Runtime Data bytes differ from the publication snapshot"));
        }

        std::vector<PackageFilePayload> payloads;
        payloads.reserve(3U);
        payloads.push_back(std::move(*executablePayload.try_value()));
        payloads.push_back(std::move(*projectPayload.try_value()));
        payloads.push_back(std::move(*scenePayload.try_value()));
        std::vector<PackageFileEntry> entries;
        entries.reserve(payloads.size());
        for (const PackageFilePayload &payload : payloads)
        {
            entries.push_back(payload.entry());
        }
        auto manifest = PackageManifest::create_monolithic(
            std::string(a_runtimeData.project_id()), a_engineVersion, BuildConfiguration::Release,
            std::string(a_runtimeData.startup_scene_asset_id()),
            std::string(a_runtimeData.startup_scene_data().relative_path()), ShippingTrustMode::UnsignedLocal,
            std::nullopt, std::nullopt, std::move(entries), a_assertContext);
        if (!manifest)
        {
            return Result<PackagePublishReport>::failure(std::move(*manifest.try_error()));
        }
        return Result<PackagePublishReport>::success(
            publish_runtime_package_impl(a_projectFilesystem, a_destination, *manifest.try_value(), payloads,
                                         *canonicalRoot, a_cancellation, a_assertContext));
    }
    catch (...)
    {
        terminate_publisher_exception(a_assertContext);
    }
}
} // namespace cue::package
