#include <Cue/Package/Publisher.h>

#include "Sha256.h"

#include <Cue/Foundation/Assert.h>
#include <Cue/IO/Error.h>
#include <Cue/Package/Error.h>

#include <algorithm>
#include <cstdlib>
#include <string>
#include <utility>

namespace
{
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
    return {std::string(a_manifest.project_id()), a_manifest.engine_version(), a_manifest.configuration(),
            std::string(a_manifest.startup_scene_asset_id()), a_manifest.files().size(), inventoryBytes};
}

/// @brief StageとOutcomeに対応する所有Reportを構築する
[[nodiscard]] cue::package::PackagePublishReport make_report(
    cue::package::PackagePublishStage a_stage, cue::package::PackagePublishOutcome a_outcome,
    const cue::RelativePath &a_destination, const cue::package::PackageManifest &a_manifest,
    std::optional<cue::Error> a_error = std::nullopt,
    std::optional<cue::StagingArea> a_recoveryStaging = std::nullopt)
{
    return {a_stage, a_outcome, std::string(a_destination.text()), make_summary(a_manifest), std::move(a_error),
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
        return cue::Result<void>::failure(cue::package::make_package_error(
            a_assertContext, cue::package::PackageError::PackageFileMismatch,
            "Package payload count differs from the Manifest"));
    }
    for (const cue::package::PackageFileEntry &expected : a_manifest.files())
    {
        const std::size_t matches = static_cast<std::size_t>(std::count_if(
            a_payloads.begin(), a_payloads.end(),
            /// @brief 現在のManifest Entryと同じPayloadを数える
            [&](const cue::package::PackageFilePayload &a_payload) noexcept
            { return entry_matches(expected, a_payload.entry()); }));
        if (matches != 1U)
        {
            return cue::Result<void>::failure(cue::package::make_package_error(
                a_assertContext, cue::package::PackageError::PackageFileMismatch,
                "Package payload identity differs from the Manifest"));
        }
    }
    return cue::Result<void>::success();
}

/// @brief Root配下のManifestと全PayloadをCanonical Byte列へ再検証する
[[nodiscard]] cue::Result<void> verify_package_snapshot(
    cue::FilesystemRoot &a_filesystem, const cue::RelativePath &a_root,
    std::span<const cue::package::PackageFilePayload> a_payloads, std::string_view a_manifestBytes,
    const cue::AssertContext &a_assertContext) noexcept
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
        return cue::Result<void>::failure(cue::package::make_package_error(
            a_assertContext, cue::package::PackageError::PackageFileMismatch,
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
        return cue::Result<void>::failure(canonical ? cue::package::make_package_error(
                                                          a_assertContext,
                                                          cue::package::PackageError::InvalidPackageManifest,
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
            return cue::Result<void>::failure(cue::package::make_package_error(
                a_assertContext, cue::package::PackageError::PackageFileMismatch,
                "Package file bytes differ from the immutable payload snapshot"));
        }
    }
    return cue::Result<void>::success();
}

/// @brief Primary Errorを維持してStagingをRollbackし、失敗時はRecovery所有権を返す
[[nodiscard]] std::optional<cue::StagingArea> rollback_staging(cue::FilesystemRoot &a_filesystem,
                                                               cue::StagingArea &a_staging,
                                                               cue::Error &a_primary,
                                                               const cue::AssertContext &a_assertContext) noexcept
{
    auto rollback = a_filesystem.rollback_staging_area(std::move(a_staging));
    if (rollback)
    {
        return std::nullopt;
    }
    a_primary.append_secondary_diagnostics(a_assertContext, *rollback.try_error(),
                                           "Package staging rollback failed", "Rollback");
    return std::optional<cue::StagingArea>(std::move(a_staging));
}

/// @brief IO Errorが公開済みでRollback不能なDurabilityUnknownか返す
[[nodiscard]] bool is_durability_unknown(const cue::Error &a_error) noexcept
{
    return a_error.root_code().domain() == "Cue.IO" &&
           a_error.root_code().value() == static_cast<std::int64_t>(cue::IoError::DurabilityUnknown);
}
} // namespace

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

bool PackageCancellation::try_begin_publish() const noexcept
{
    State expected = State::Active;
    return m_state.compare_exchange_strong(expected, State::PublishStarted, std::memory_order_acq_rel,
                                           std::memory_order_acquire);
}

PackagePublishReport publish_runtime_package(FilesystemRoot &a_filesystem, const RelativePath &a_destination,
                                             const PackageManifest &a_manifest,
                                             std::span<const PackageFilePayload> a_payloads,
                                             const PackageCancellation &a_cancellation,
                                             const AssertContext &a_assertContext) noexcept
{
    try
    {
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
            return make_report(PackagePublishStage::ValidateInput, PackagePublishOutcome::NotPublished, a_destination,
                               a_manifest,
                               make_io_error(a_assertContext, IoError::AlreadyExists,
                                             "Package destination already exists"));
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
        auto manifestWritten = manifestPath ? a_filesystem.write_file_atomic(
                                                  *manifestPath.try_value(), std::as_bytes(manifestCharacters))
                                            : Result<void>::failure(std::move(*manifestPath.try_error()));
        if (!manifestWritten)
        {
            Error primary = std::move(*manifestWritten.try_error());
            auto recovery = rollback_staging(a_filesystem, staging, primary, a_assertContext);
            return make_report(PackagePublishStage::WriteManifest, PackagePublishOutcome::NotPublished, a_destination,
                               a_manifest, std::move(primary), std::move(recovery));
        }

        auto staged = verify_package_snapshot(a_filesystem, staging.path(), a_payloads, *manifest.try_value(),
                                              a_assertContext);
        if (!staged || a_cancellation.is_cancel_requested())
        {
            Error primary = staged ? make_package_error(a_assertContext, PackageError::PackageCancelled,
                                                        "Package publication was cancelled")
                                   : std::move(*staged.try_error());
            auto recovery = rollback_staging(a_filesystem, staging, primary, a_assertContext);
            return make_report(PackagePublishStage::ValidateStaging, PackagePublishOutcome::NotPublished,
                               a_destination, a_manifest, std::move(primary), std::move(recovery));
        }

        if (!a_cancellation.try_begin_publish())
        {
            Error primary = make_package_error(a_assertContext, PackageError::PackageCancelled,
                                               "Package publication was cancelled");
            auto recovery = rollback_staging(a_filesystem, staging, primary, a_assertContext);
            return make_report(PackagePublishStage::ValidateStaging, PackagePublishOutcome::NotPublished,
                               a_destination, a_manifest, std::move(primary), std::move(recovery));
        }

        auto published = a_filesystem.publish_staging_area(std::move(staging), a_destination);
        if (!published)
        {
            Error primary = std::move(*published.try_error());
            if (is_durability_unknown(primary))
            {
                auto visible = verify_package_snapshot(a_filesystem, a_destination, a_payloads, *manifest.try_value(),
                                                       a_assertContext);
                if (!visible)
                {
                    primary.append_secondary_diagnostics(a_assertContext, *visible.try_error(),
                                                         "Visible Package revalidation failed", "Revalidation");
                }
                return make_report(PackagePublishStage::Publish,
                                   PackagePublishOutcome::PublishedButDurabilityUnknown, a_destination, a_manifest,
                                   std::move(primary));
            }
            auto recovery = rollback_staging(a_filesystem, staging, primary, a_assertContext);
            return make_report(PackagePublishStage::Publish, PackagePublishOutcome::NotPublished, a_destination,
                               a_manifest, std::move(primary), std::move(recovery));
        }

        auto finalSnapshot = verify_package_snapshot(a_filesystem, a_destination, a_payloads, *manifest.try_value(),
                                                     a_assertContext);
        if (!finalSnapshot)
        {
            return make_report(PackagePublishStage::ValidatePublished,
                               PackagePublishOutcome::PublishedButDurabilityUnknown, a_destination, a_manifest,
                               std::move(*finalSnapshot.try_error()));
        }
        return make_report(PackagePublishStage::Completed, PackagePublishOutcome::Committed, a_destination,
                           a_manifest);
    }
    catch (...)
    {
        terminate_publisher_exception(a_assertContext);
    }
}
} // namespace cue::package
