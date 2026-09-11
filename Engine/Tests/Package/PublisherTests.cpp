#include <Cue/Package/Publisher.h>

#include "PublisherPath.h"

#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Fatal.h>
#include <Cue/Foundation/Log.h>
#include <Cue/IO/Error.h>
#include <Cue/IO/Windows/WindowsFilesystem.h>
#include <Cue/Package/Error.h>
#include <Cue/Project/Generator.h>
#include <Cue/Scene/Identity.h>
#include <Cue/Scene/Instantiation.h>
#include <Cue/Scene/SceneDocument.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <Windows.h>

namespace
{
constexpr std::string_view k_projectId = "41234567-89ab-4cde-8f01-23456789abcd";
constexpr std::string_view k_sceneId = "51234567-89ab-4cde-8f01-23456789abcd";
constexpr std::string_view k_scenePath = "Data/Scenes/51234567-89ab-4cde-8f01-23456789abcd.cueruntime.json";
constexpr cue::BuildWorkspaceCompatibility k_workspaceCompatibility{
    cue::BuildGenerator::VisualStudio2026, cue::BuildArchitecture::X64, {19U, 51U, 0U, 0U}, 2U};

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
void require(bool a_condition, const std::source_location &a_location = std::source_location::current()) noexcept
{
    if (!a_condition)
    {
        std::cerr << "Requirement failed at " << a_location.file_name() << ':' << a_location.line() << '\n';
        std::abort();
    }
}

/// @brief Result成功値を所有値として取得する
template <typename T> [[nodiscard]] T take_value(cue::Result<T> a_result) noexcept
{
    require(a_result.has_value());
    return std::move(*a_result.try_value());
}

/// @brief ASCII Test Fixtureを所有Byte列へ変換する
[[nodiscard]] std::vector<std::byte> bytes(std::string_view a_text)
{
    const std::span<const char> characters(a_text.data(), a_text.size());
    const std::span<const std::byte> raw = std::as_bytes(characters);
    return std::vector<std::byte>(raw.begin(), raw.end());
}

/// @brief 公開済みFileを再生成比較用Byte列として読む
[[nodiscard]] std::vector<std::byte> read_bytes(const std::filesystem::path &a_path)
{
    std::ifstream stream(a_path, std::ios::binary | std::ios::ate);
    require(stream.good());
    const std::streamsize size = stream.tellg();
    require(size >= 0);
    stream.seekg(0, std::ios::beg);
    std::vector<std::byte> result(static_cast<std::size_t>(size));
    if (!result.empty())
    {
        stream.read(reinterpret_cast<char *>(result.data()), size);
        require(stream.good());
    }
    return result;
}

/// @brief Test Artifact Byte列をBinary Fileへ書く
[[nodiscard]] bool write_bytes(const std::filesystem::path &a_path, std::span<const std::byte> a_bytes)
{
    std::ofstream stream(a_path, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char *>(a_bytes.data()), static_cast<std::streamsize>(a_bytes.size()));
    return stream.good();
}

/// @brief Absolute Test PathをProcess manifest非依存のExtended-length形式へ変換する
[[nodiscard]] std::filesystem::path extended_path(const std::filesystem::path &a_path)
{
    std::filesystem::path preferred = a_path;
    preferred.make_preferred();
    const std::wstring value = preferred.native();
    if (value.starts_with(L"\\\\?\\"))
    {
        return a_path;
    }
    if (value.starts_with(L"\\\\"))
    {
        return std::filesystem::path(L"\\\\?\\UNC\\" + value.substr(2U));
    }
    return std::filesystem::path(L"\\\\?\\" + value);
}

/// @brief 5個の必須Roleを持つ最小Package Payloadを構築する
[[nodiscard]] std::vector<cue::package::PackageFilePayload> make_payloads(const cue::AssertContext &a_assertContext)
{
    using cue::package::PackageFilePayload;
    using cue::package::PackageFileRole;
    std::vector<PackageFilePayload> payloads;
    payloads.push_back(take_value(PackageFilePayload::create(PackageFileRole::RuntimeHost, "CueRuntimeHost.exe",
                                                             bytes("runtime-host"), a_assertContext)));
    payloads.push_back(take_value(PackageFilePayload::create(PackageFileRole::GameModule, "Game/CueGameModule.dll",
                                                             bytes("game-module"), a_assertContext)));
    payloads.push_back(take_value(PackageFilePayload::create(
        PackageFileRole::GameModuleMetadata, "Game/CueGameModule.metadata.json", bytes("metadata"), a_assertContext)));
    payloads.push_back(take_value(PackageFilePayload::create(
        PackageFileRole::ProjectRuntimeData, "Data/CueProject.runtime.json", bytes("project-data"), a_assertContext)));
    payloads.push_back(take_value(PackageFilePayload::create(
        PackageFileRole::StartupSceneRuntimeData, std::string(k_scenePath), bytes("scene-data"), a_assertContext)));
    return payloads;
}

/// @brief Payload Entry列から検証済みManifestを構築する
[[nodiscard]] cue::package::PackageManifest make_manifest(std::span<const cue::package::PackageFilePayload> a_payloads,
                                                          const cue::AssertContext &a_assertContext)
{
    std::vector<cue::package::PackageFileEntry> entries;
    entries.reserve(a_payloads.size());
    for (const cue::package::PackageFilePayload &payload : a_payloads)
    {
        entries.push_back(payload.entry());
    }
    return take_value(cue::package::PackageManifest::create(
        std::string(k_projectId), {1U, 0U, 0U}, cue::BuildConfiguration::Debug, std::string(k_sceneId),
        std::string(k_scenePath), std::move(entries), a_assertContext));
}

enum class FailurePoint
{
    None,
    CreateStaging,
    Write,
    ValidateStaging,
    Publish,
    Durability,
    WriteAndRollback,
    BlockPublish,
    InjectUnknownFile
};

/// @brief 実Windows Filesystemの検証完了後かつPublish Authorization確定前でTest Threadを停止する
class BlockingPublishAuthorization final : public cue::StagingPublishAuthorization
{
  public:
    /// @brief 委譲先Authorizationと同期Flagを借用する
    BlockingPublishAuthorization(const cue::StagingPublishAuthorization *a_inner, std::atomic_bool &a_entered,
                                 std::atomic_bool &a_allowed) noexcept
        : m_inner(a_inner), m_entered(&a_entered), m_allowed(&a_allowed)
    {
    }
    /// @brief 借用した同期対象を変更せず解放する
    ~BlockingPublishAuthorization() override = default;

    /// @brief 検証完了を通知し、Test Threadが許可した後に本来のAuthorizationへ委譲する
    [[nodiscard]] bool try_authorize() const noexcept override
    {
        m_entered->store(true, std::memory_order_release);
        while (!m_allowed->load(std::memory_order_acquire))
        {
            std::this_thread::yield();
        }
        return m_inner == nullptr || m_inner->try_authorize();
    }

  private:
    const cue::StagingPublishAuthorization *m_inner;
    std::atomic_bool *m_entered;
    std::atomic_bool *m_allowed;
};

/// @brief 実Windows Filesystemへ委譲しPackage Stageだけを一度失敗させるTest Double
class FailingFilesystemRoot final : public cue::FilesystemRoot
{
  public:
    /// @brief 委譲先Rootと一度だけ消費するFailure Pointを所有する
    FailingFilesystemRoot(std::unique_ptr<cue::FilesystemRoot> a_inner, FailurePoint a_failure,
                          const cue::AssertContext &a_assertContext, const bool *a_artifactReadLeaseActive = nullptr,
                          bool *a_artifactReadObserved = nullptr, bool *a_artifactReadWithLease = nullptr) noexcept
        : m_inner(std::move(a_inner)), m_failure(a_failure), m_assertContext(&a_assertContext),
          m_artifactReadLeaseActive(a_artifactReadLeaseActive), m_artifactReadObserved(a_artifactReadObserved),
          m_artifactReadWithLease(a_artifactReadWithLease)
    {
    }
    /// @brief 委譲先Rootを解放する
    ~FailingFilesystemRoot() override = default;

    /// @brief 実FilesystemのStaging検証が完了してAuthorization境界へ到達したか期限付きで待機して返す
    [[nodiscard]] bool wait_until_publish_entered() const noexcept
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!m_publishEntered.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::yield();
        }
        return m_publishEntered.load(std::memory_order_acquire);
    }

    /// @brief 待機中のAuthorizationを実FilesystemのNative Publish境界へ進める
    void allow_publish() noexcept
    {
        m_allowPublish.store(true, std::memory_order_release);
    }

    /// @brief 委譲先Root Identityを返す
    [[nodiscard]] cue::Result<cue::FilesystemIdentity> root_identity() const noexcept override
    {
        return m_inner->root_identity();
    }
    /// @brief Entry照会を委譲する
    [[nodiscard]] cue::Result<cue::EntryType> query_entry(const cue::RelativePath &a_path) noexcept override
    {
        return m_inner->query_entry(a_path);
    }
    /// @brief Validate Failureを注入するかFile読取りを委譲する
    [[nodiscard]] cue::Result<std::vector<std::byte>> read_file(const cue::RelativePath &a_path,
                                                                std::size_t a_maxBytes) noexcept override
    {
        if (m_artifactReadObserved != nullptr && !*m_artifactReadObserved &&
            a_path.text().ends_with("/CueGameProduct.exe"))
        {
            *m_artifactReadObserved = true;
            if (m_artifactReadWithLease != nullptr)
            {
                *m_artifactReadWithLease = m_artifactReadLeaseActive != nullptr && *m_artifactReadLeaseActive;
            }
        }
        if (consume(FailurePoint::ValidateStaging))
        {
            return cue::Result<std::vector<std::byte>>::failure(make_failure("Staged Package validation failed"));
        }
        return m_inner->read_file(a_path, a_maxBytes);
    }
    /// @brief Directory作成を委譲する
    [[nodiscard]] cue::Result<void> create_directories(const cue::RelativePath &a_path) noexcept override
    {
        return m_inner->create_directories(a_path);
    }
    /// @brief Write Failureを注入するかAtomic File書込みを委譲する
    [[nodiscard]] cue::Result<void> write_file_atomic(const cue::RelativePath &a_path,
                                                      std::span<const std::byte> a_bytes) noexcept override
    {
        if (consume(FailurePoint::Write) || consume(FailurePoint::WriteAndRollback))
        {
            return cue::Result<void>::failure(make_failure("Package content write failed"));
        }
        auto written = m_inner->write_file_atomic(a_path, a_bytes);
        if (!written || m_failure != FailurePoint::InjectUnknownFile || m_failureConsumed ||
            !a_path.text().ends_with("/CuePackage.json"))
        {
            return written;
        }
        m_failureConsumed = true;
        constexpr std::string_view manifestFileName = "CuePackage.json";
        std::string unexpectedPath(a_path.text().substr(0U, a_path.text().size() - manifestFileName.size()));
        unexpectedPath.append("Unexpected.dll");
        auto relative = cue::RelativePath::parse(unexpectedPath, *m_assertContext);
        if (!relative)
        {
            return cue::Result<void>::failure(std::move(*relative.try_error()));
        }
        const std::vector<std::byte> unexpectedBytes = bytes("x");
        return m_inner->write_file_atomic(*relative.try_value(), unexpectedBytes);
    }
    /// @brief Recovery Backup書込みを委譲する
    [[nodiscard]] cue::Result<void> write_recovery_backup_atomic(
        const cue::RelativePath &a_destination, std::span<const std::byte> a_bytes,
        const cue::AssertContext &a_assertContext) noexcept override
    {
        return m_inner->write_recovery_backup_atomic(a_destination, a_bytes, a_assertContext);
    }
    /// @brief File Write Lease取得を委譲する
    [[nodiscard]] cue::Result<cue::FileWriteLease> acquire_file_write_lease(
        const cue::RelativePath &a_path) noexcept override
    {
        return m_inner->acquire_file_write_lease(a_path);
    }
    /// @brief Conditional Atomic Writeを委譲する
    [[nodiscard]] cue::Result<void> write_file_atomic_if_unchanged(cue::FileWriteLease &a_lease,
                                                                   const cue::RelativePath &a_path,
                                                                   cue::FileFingerprint a_expected,
                                                                   std::size_t a_maximumExpectedBytes,
                                                                   std::span<const std::byte> a_bytes) noexcept override
    {
        return m_inner->write_file_atomic_if_unchanged(a_lease, a_path, a_expected, a_maximumExpectedBytes, a_bytes);
    }
    /// @brief Regular File削除を委譲する
    [[nodiscard]] cue::Result<void> remove_file(const cue::RelativePath &a_path) noexcept override
    {
        return m_inner->remove_file(a_path);
    }
    /// @brief Staging作成Failureを注入するかOperation所有Staging作成を委譲する
    [[nodiscard]] cue::Result<cue::StagingArea> create_staging_area(
        const cue::RelativePath &a_destination) noexcept override
    {
        if (consume(FailurePoint::CreateStaging))
        {
            return cue::Result<cue::StagingArea>::failure(make_failure("Package staging creation failed"));
        }
        return m_inner->create_staging_area(a_destination);
    }
    /// @brief Publish FailureまたはDurabilityUnknownを注入するかStaging公開を委譲する
    [[nodiscard]] cue::Result<void> publish_staging_area(
        cue::StagingArea &&a_staging, const cue::RelativePath &a_destination,
        const cue::StagingPublishAuthorization *a_authorization = nullptr) noexcept override
    {
        if (m_failure == FailurePoint::BlockPublish)
        {
            BlockingPublishAuthorization blocking(a_authorization, m_publishEntered, m_allowPublish);
            return m_inner->publish_staging_area(std::move(a_staging), a_destination, &blocking);
        }
        if (consume(FailurePoint::Publish))
        {
            return cue::Result<void>::failure(make_failure("Package publish failed"));
        }
        if (consume(FailurePoint::Durability))
        {
            auto published = m_inner->publish_staging_area(std::move(a_staging), a_destination, a_authorization);
            if (!published)
            {
                return published;
            }
            return cue::Result<void>::failure(cue::make_io_error(*m_assertContext, cue::IoError::DurabilityUnknown,
                                                                 "Published Package durability is unknown"));
        }
        return m_inner->publish_staging_area(std::move(a_staging), a_destination, a_authorization);
    }
    /// @brief Rollback Failureを一度注入するかOperation所有Staging削除を委譲する
    [[nodiscard]] cue::Result<void> rollback_staging_area(cue::StagingArea &&a_staging) noexcept override
    {
        if (m_failure == FailurePoint::WriteAndRollback && m_writeRollbackFailureConsumed && !m_rollbackFailureConsumed)
        {
            m_rollbackFailureConsumed = true;
            return cue::Result<void>::failure(make_failure("Package staging rollback failed"));
        }
        return m_inner->rollback_staging_area(std::move(a_staging));
    }

  private:
    /// @brief 指定Failure Pointが未消費なら一度だけtrueを返す
    [[nodiscard]] bool consume(FailurePoint a_failure) noexcept
    {
        if (m_failure != a_failure)
        {
            return false;
        }
        if (a_failure == FailurePoint::WriteAndRollback)
        {
            if (m_writeRollbackFailureConsumed)
            {
                return false;
            }
            m_writeRollbackFailureConsumed = true;
            return true;
        }
        if (m_failureConsumed)
        {
            return false;
        }
        m_failureConsumed = true;
        return true;
    }

    /// @brief Test用Portable IO Errorを構築する
    [[nodiscard]] cue::Error make_failure(std::string_view a_summary) const noexcept
    {
        return cue::make_io_error(*m_assertContext, cue::IoError::IoFailure, a_summary);
    }

    std::unique_ptr<cue::FilesystemRoot> m_inner;
    FailurePoint m_failure;
    const cue::AssertContext *m_assertContext;
    bool m_failureConsumed = false;
    bool m_writeRollbackFailureConsumed = false;
    bool m_rollbackFailureConsumed = false;
    const bool *m_artifactReadLeaseActive;
    bool *m_artifactReadObserved;
    bool *m_artifactReadWithLease;
    std::atomic_bool m_publishEntered = false;
    std::atomic_bool m_allowPublish = false;
};

/// @brief Monolithic PublisherがArtifact Byte取得中だけ保持するTest用Read Leaseを発行する
class TestArtifactReader final : public cue::BuildArtifactReader
{
  public:
    /// @brief Reader所有のLease状態をtrueへ設定するToken
    class Lease final : public cue::BuildArtifactReadLease
    {
      public:
        /// @brief 借用状態を開始する
        explicit Lease(bool &a_active) noexcept : m_active(&a_active)
        {
            *m_active = true;
        }

        /// @brief 借用状態を終了する
        ~Lease() override
        {
            *m_active = false;
        }

        /// @brief Test ReaderへBindingされたProject Identityを返す
        [[nodiscard]] std::string_view project_id() const noexcept override
        {
            return k_projectId;
        }

      private:
        bool *m_active;
    };

    /// @brief 取消済みでなければTest用Read Leaseを返す
    [[nodiscard]] cue::Result<std::optional<std::unique_ptr<cue::BuildArtifactReadLease>>> acquire_current_read_lease(
        const cue::BuildArtifactInventory &, const cue::BuildArtifactReadCancellation &a_cancellation,
        cue::BuildArtifactLockDeadline) noexcept override
    {
        ++calls;
        if (a_cancellation.is_cancel_requested())
        {
            return cue::Result<std::optional<std::unique_ptr<cue::BuildArtifactReadLease>>>::success(std::nullopt);
        }
        std::unique_ptr<cue::BuildArtifactReadLease> lease = std::make_unique<Lease>(active);
        return cue::Result<std::optional<std::unique_ptr<cue::BuildArtifactReadLease>>>::success(
            std::optional<std::unique_ptr<cue::BuildArtifactReadLease>>(std::move(lease)));
    }

    std::uint32_t calls = 0U;
    bool active = false;
};

/// @brief 空Startup Sceneから決定的Runtime Data Publicationを作る
[[nodiscard]] cue::package::MinimalRuntimeDataPublication make_runtime_data(const cue::AssertContext &a_assertContext)
{
    auto projectId = cue::ProjectId::parse(k_projectId, a_assertContext);
    require(projectId.has_value());
    auto descriptor = cue::create_blank_project_descriptor(
        *projectId.try_value(), "Monolithic Publisher Test",
        cue::EngineCompatibility{cue::EngineVersion{1U, 0U, 0U}, cue::EngineVersion{2U, 0U, 0U}}, k_sceneId,
        a_assertContext);
    auto sceneId = cue::scene::SceneAssetId::parse(k_sceneId, a_assertContext);
    require(descriptor.has_value() && sceneId.has_value());
    cue::scene::SceneDocument scene = cue::scene::SceneDocument::create(*sceneId.try_value(), a_assertContext);
    auto snapshot = cue::scene::create_scene_snapshot(scene, a_assertContext);
    require(snapshot.has_value());
    return take_value(
        cue::package::publish_minimal_runtime_data(*descriptor.try_value(), *snapshot.try_value(), a_assertContext));
}

/// @brief Shipping Product用の検証済みArtifact Inventoryを作る
[[nodiscard]] cue::BuildArtifactInventory make_shipping_artifact(const std::filesystem::path &a_projectRoot,
                                                                 std::span<const std::byte> a_executableBytes,
                                                                 cue::ShippingTrustMode a_trustMode,
                                                                 const cue::AssertContext &a_assertContext)
{
    const std::string publisherKey =
        a_trustMode == cue::ShippingTrustMode::PublisherSigned ? std::string(64U, 'a') : std::string();
    auto profile = cue::BuildProfile::create_shipping_product(cue::BuildConfiguration::Release, a_trustMode,
                                                              publisherKey, a_assertContext);
    require(profile.has_value());
    cue::BuildRequest request{a_projectRoot.generic_string(), *profile.try_value(),
                              "61234567-89ab-4cde-8f01-23456789abcd", k_workspaceCompatibility};
    auto plan = cue::create_build_plan(request, a_assertContext);
    require(plan.has_value());
    auto payload = cue::package::PackageFilePayload::create(
        cue::package::PackageFileRole::ApplicationExecutable, "CueGameProduct.exe",
        std::vector<std::byte>(a_executableBytes.begin(), a_executableBytes.end()), a_assertContext);
    require(payload.has_value());
    auto artifact = cue::BuildArtifactInventory::create(
        *plan.try_value(), "71234567-89ab-4cde-8f01-23456789abcd",
        {{"CueGameProduct.exe", payload.try_value()->entry().byte_size(),
          std::string(payload.try_value()->entry().sha256()), cue::BuildArtifactFilePurpose::DistributionPayload},
         {"CueGameProduct.metadata.json", 8U, std::string(64U, 'b'), cue::BuildArtifactFilePurpose::RuntimeMetadata},
         {"CueGameProduct.pdb", 8U, std::string(64U, 'c'), cue::BuildArtifactFilePurpose::DevelopmentSymbol}},
        a_assertContext);
    return take_value(std::move(artifact));
}

/// @brief Test Rootを開くWindows Filesystemを構築する
[[nodiscard]] std::unique_ptr<cue::FilesystemRoot> open_root(const std::filesystem::path &a_root,
                                                             const cue::AssertContext &a_assertContext)
{
    return take_value(cue::create_windows_filesystem_root(a_root.generic_string(), a_assertContext));
}

/// @brief 正常公開、既存Destination拒否、取消を実Filesystemで検証する
void test_publish_contract(const std::filesystem::path &a_root, const cue::package::PackageManifest &a_manifest,
                           std::span<const cue::package::PackageFilePayload> a_payloads,
                           const cue::AssertContext &a_assertContext)
{
    std::unique_ptr<cue::FilesystemRoot> filesystem = open_root(a_root, a_assertContext);
    cue::RelativePath destination = take_value(cue::RelativePath::parse("GamePackage", a_assertContext));
    cue::package::PackageCancellation cancellation;
    auto published = cue::package::publish_runtime_package(*filesystem, destination, a_manifest, a_payloads,
                                                           cancellation, a_assertContext);
    require(published.succeeded());
    require(published.stage == cue::package::PackagePublishStage::Completed);
    require(published.outcome == cue::package::PackagePublishOutcome::Committed);
    require(!published.error.has_value() && !published.recoveryStaging.has_value());
    require(std::filesystem::is_regular_file(a_root / "GamePackage" / "CuePackage.json"));
    require(std::filesystem::is_regular_file(a_root / "GamePackage" / "Game" / "CueGameModule.dll"));

    auto collided = cue::package::publish_runtime_package(*filesystem, destination, a_manifest, a_payloads,
                                                          cancellation, a_assertContext);
    require(!collided.succeeded() && collided.outcome == cue::package::PackagePublishOutcome::NotPublished);
    require(collided.error.has_value() && collided.error->root_code().domain() == "Cue.IO" &&
            collided.error->root_code().value() == static_cast<std::int64_t>(cue::IoError::AlreadyExists));

    cue::RelativePath cancelledDestination = take_value(cue::RelativePath::parse("CancelledPackage", a_assertContext));
    cue::package::PackageCancellation cancelled;
    cancelled.request_cancel();
    auto cancelledReport = cue::package::publish_runtime_package(*filesystem, cancelledDestination, a_manifest,
                                                                 a_payloads, cancelled, a_assertContext);
    require(cancelledReport.outcome == cue::package::PackagePublishOutcome::NotPublished);
    require(!std::filesystem::exists(a_root / "CancelledPackage"));
}

/// @brief 同じManifestとPayloadから公開したPackageの全ContentがByte一致することを検証する
void test_reproducible_publication(const std::filesystem::path &a_root, const cue::package::PackageManifest &a_manifest,
                                   std::span<const cue::package::PackageFilePayload> a_payloads,
                                   const cue::AssertContext &a_assertContext)
{
    std::unique_ptr<cue::FilesystemRoot> filesystem = open_root(a_root, a_assertContext);
    cue::RelativePath firstDestination = take_value(cue::RelativePath::parse("ReproduciblePackageA", a_assertContext));
    cue::RelativePath secondDestination = take_value(cue::RelativePath::parse("ReproduciblePackageB", a_assertContext));
    cue::package::PackageCancellation firstCancellation;
    cue::package::PackageCancellation secondCancellation;
    const cue::package::PackagePublishReport first = cue::package::publish_runtime_package(
        *filesystem, firstDestination, a_manifest, a_payloads, firstCancellation, a_assertContext);
    const cue::package::PackagePublishReport second = cue::package::publish_runtime_package(
        *filesystem, secondDestination, a_manifest, a_payloads, secondCancellation, a_assertContext);
    require(first.succeeded() && second.succeeded());

    const std::filesystem::path firstRoot = a_root / "ReproduciblePackageA";
    const std::filesystem::path secondRoot = a_root / "ReproduciblePackageB";
    std::size_t firstFileCount = 0U;
    for (const std::filesystem::directory_entry &entry : std::filesystem::recursive_directory_iterator(firstRoot))
    {
        if (!entry.is_regular_file())
        {
            continue;
        }
        ++firstFileCount;
        const std::filesystem::path relative = std::filesystem::relative(entry.path(), firstRoot);
        const std::filesystem::path counterpart = secondRoot / relative;
        require(std::filesystem::is_regular_file(counterpart));
        require(read_bytes(entry.path()) == read_bytes(counterpart));
    }
    std::size_t secondFileCount = 0U;
    for (const std::filesystem::directory_entry &entry : std::filesystem::recursive_directory_iterator(secondRoot))
    {
        secondFileCount += entry.is_regular_file() ? 1U : 0U;
    }
    require(firstFileCount == a_payloads.size() + 1U && secondFileCount == firstFileCount);
}

/// @brief Publish前Failure、Rollback Recovery、Publish後DurabilityUnknownを検証する
void test_failure_injection(const std::filesystem::path &a_root, const cue::package::PackageManifest &a_manifest,
                            std::span<const cue::package::PackageFilePayload> a_payloads,
                            const cue::AssertContext &a_assertContext)
{
    constexpr std::array failures = {FailurePoint::CreateStaging, FailurePoint::Write, FailurePoint::ValidateStaging,
                                     FailurePoint::Publish};
    std::size_t index = 0U;
    for (const FailurePoint failure : failures)
    {
        FailingFilesystemRoot filesystem(open_root(a_root, a_assertContext), failure, a_assertContext);
        const std::string name = "FailedPackage" + std::to_string(index++);
        cue::RelativePath destination = take_value(cue::RelativePath::parse(name, a_assertContext));
        cue::package::PackageCancellation cancellation;
        auto report = cue::package::publish_runtime_package(filesystem, destination, a_manifest, a_payloads,
                                                            cancellation, a_assertContext);
        require(report.outcome == cue::package::PackagePublishOutcome::NotPublished);
        require(report.error.has_value() && !report.recoveryStaging.has_value());
        require(!std::filesystem::exists(a_root / name));
    }

    FailingFilesystemRoot recoveryFilesystem(open_root(a_root, a_assertContext), FailurePoint::WriteAndRollback,
                                             a_assertContext);
    cue::RelativePath recoveryDestination = take_value(cue::RelativePath::parse("RecoveryPackage", a_assertContext));
    cue::package::PackageCancellation recoveryCancellation;
    auto recovery = cue::package::publish_runtime_package(recoveryFilesystem, recoveryDestination, a_manifest,
                                                          a_payloads, recoveryCancellation, a_assertContext);
    require(recovery.outcome == cue::package::PackagePublishOutcome::NotPublished && recovery.error.has_value() &&
            recovery.recoveryStaging.has_value());
    const std::string stagingPath(recovery.recoveryStaging->path().text());
    require(recoveryFilesystem.rollback_staging_area(std::move(*recovery.recoveryStaging)).has_value());
    require(!std::filesystem::exists(a_root / stagingPath));

    FailingFilesystemRoot durabilityFilesystem(open_root(a_root, a_assertContext), FailurePoint::Durability,
                                               a_assertContext);
    cue::RelativePath durabilityDestination = take_value(cue::RelativePath::parse("UnknownPackage", a_assertContext));
    cue::package::PackageCancellation durabilityCancellation;
    auto durability = cue::package::publish_runtime_package(durabilityFilesystem, durabilityDestination, a_manifest,
                                                            a_payloads, durabilityCancellation, a_assertContext);
    require(durability.outcome == cue::package::PackagePublishOutcome::PublishedButDurabilityUnknown);
    require(!durability.succeeded() && durability.error.has_value() && !durability.recoveryStaging.has_value());
    require(std::filesystem::is_regular_file(a_root / "UnknownPackage" / "CuePackage.json"));
}

/// @brief Filesystem検証中のCancelがNative Publish直前で受理されることを検証する
void test_cancellation_publish_boundary(const std::filesystem::path &a_root,
                                        const cue::package::PackageManifest &a_manifest,
                                        std::span<const cue::package::PackageFilePayload> a_payloads,
                                        const cue::AssertContext &a_assertContext)
{
    FailingFilesystemRoot filesystem(open_root(a_root, a_assertContext), FailurePoint::BlockPublish, a_assertContext);
    cue::RelativePath destination = take_value(cue::RelativePath::parse("PublishBoundaryPackage", a_assertContext));
    cue::package::PackageCancellation cancellation;
    std::optional<cue::package::PackagePublishReport> report;
    std::thread publishThread(
        /// @brief Package公開を別Threadで進め、Publish状態遷移後にFilesystem境界で待機する
        [&]() noexcept
        {
            report.emplace(cue::package::publish_runtime_package(filesystem, destination, a_manifest, a_payloads,
                                                                 cancellation, a_assertContext));
        });

    const bool publishEntered = filesystem.wait_until_publish_entered();
    cancellation.request_cancel();
    filesystem.allow_publish();
    publishThread.join();

    require(publishEntered && report.has_value() && !report->succeeded());
    require(report->outcome == cue::package::PackagePublishOutcome::NotPublished && report->error.has_value());
    require(report->error->root_code().domain() == "Cue.Package" &&
            report->error->root_code().value() ==
                static_cast<std::int64_t>(cue::package::PackageError::PackageCancelled));
    require(!std::filesystem::exists(a_root / "PublishBoundaryPackage"));
}

/// @brief Shipping Artifactからv2 Packageだけを公開し、除外File、移設、失敗保全を検証する
void test_monolithic_publication(const cue::AssertContext &a_assertContext)
{
    const std::filesystem::path projectRoot =
        std::filesystem::temp_directory_path() /
        (L"CueMonolithicPublisherTests-" + std::to_wstring(GetCurrentProcessId()));
    std::error_code error;
    std::filesystem::remove_all(projectRoot, error);
    require(!error && std::filesystem::create_directories(projectRoot));
    const std::vector<std::byte> executableBytes = bytes("shipping-product-executable");
    cue::BuildArtifactInventory artifact =
        make_shipping_artifact(projectRoot, executableBytes, cue::ShippingTrustMode::UnsignedLocal, a_assertContext);
    const std::filesystem::path artifactDirectory(artifact.version_directory());
    require(std::filesystem::create_directories(artifactDirectory));
    require(write_bytes(artifactDirectory / "CueGameProduct.exe", executableBytes));
    const std::filesystem::path driveRoot = projectRoot.root_path();
    auto driveRelative = cue::package_private::make_project_relative_path(
        driveRoot.generic_string(), projectRoot.generic_string(), a_assertContext);
    require(driveRelative.has_value() && !driveRelative.try_value()->text().empty());

    TestArtifactReader reader;
    bool artifactReadObserved = false;
    bool artifactReadWithLease = false;
    std::unique_ptr<FailingFilesystemRoot> filesystem = std::make_unique<FailingFilesystemRoot>(
        open_root(projectRoot, a_assertContext), FailurePoint::None, a_assertContext, &reader.active,
        &artifactReadObserved, &artifactReadWithLease);
    const cue::package::MinimalRuntimeDataPublication runtimeData = make_runtime_data(a_assertContext);
    const cue::RelativePath destination = take_value(cue::RelativePath::parse("MonolithicPackage", a_assertContext));
    cue::package::PackageCancellation cancellation;
    std::string projectRootWithTrailingBackslash = projectRoot.generic_string();
    projectRootWithTrailingBackslash[0U] =
        projectRootWithTrailingBackslash[0U] >= 'A' && projectRootWithTrailingBackslash[0U] <= 'Z'
            ? static_cast<char>(projectRootWithTrailingBackslash[0U] - 'A' + 'a')
            : static_cast<char>(projectRootWithTrailingBackslash[0U] - 'a' + 'A');
    projectRootWithTrailingBackslash.push_back('\\');
    auto result = cue::package::publish_monolithic_runtime_package(*filesystem, projectRootWithTrailingBackslash,
                                                                   reader, artifact, {1U, 0U, 0U}, runtimeData,
                                                                   destination, cancellation, a_assertContext);
    if (!result)
    {
        std::cerr << "Monolithic publication failed: " << result.try_error()->summary() << '\n';
    }
    else if (result.try_value()->error)
    {
        std::cerr << "Monolithic publication report failed: " << result.try_value()->error->summary() << '\n';
    }
    require(result.has_value() && result.try_value()->succeeded() && reader.calls == 1U && !reader.active &&
            artifactReadObserved && artifactReadWithLease);

    const std::filesystem::path packageRoot = projectRoot / "MonolithicPackage";
    const std::filesystem::path manifestPath = packageRoot / "CuePackage.json";
    const std::vector<std::byte> manifestBytes = read_bytes(manifestPath);
    const std::string_view manifestText(reinterpret_cast<const char *>(manifestBytes.data()), manifestBytes.size());
    auto manifest = cue::package::parse_package_manifest(manifestText, a_assertContext);
    require(manifest.has_value() &&
            manifest.try_value()->schema_version() == cue::package::k_monolithicPackageManifestSchemaVersion &&
            manifest.try_value()->execution_model() == cue::package::PackageExecutionModel::Monolithic &&
            manifest.try_value()->files().size() == 3U);
    require(std::filesystem::is_regular_file(packageRoot / "CueGameProduct.exe"));
    require(!std::filesystem::exists(packageRoot / "CueGameProduct.metadata.json") &&
            !std::filesystem::exists(packageRoot / "CueGameProduct.pdb") &&
            !std::filesystem::exists(packageRoot / "CueGameModule.dll"));
    std::size_t fileCount = 0U;
    for (const std::filesystem::directory_entry &entry : std::filesystem::recursive_directory_iterator(packageRoot))
    {
        fileCount += entry.is_regular_file() ? 1U : 0U;
    }
    require(fileCount == 4U);

    const std::filesystem::path differentRoot = projectRoot / "DifferentNativeRoot";
    require(std::filesystem::create_directory(differentRoot));
    const cue::RelativePath differentRootDestination =
        take_value(cue::RelativePath::parse("DifferentRootPackage", a_assertContext));
    auto differentRootResult = cue::package::publish_monolithic_runtime_package(
        *filesystem, differentRoot.generic_string(), reader, artifact, {1U, 0U, 0U}, runtimeData,
        differentRootDestination, cancellation, a_assertContext);
    require(!differentRootResult.has_value() && reader.calls == 1U &&
            differentRootResult.try_error()->root_code().domain() == "Cue.Package" &&
            differentRootResult.try_error()->root_code().value() ==
                static_cast<std::int64_t>(cue::package::PackageError::InvalidPackagePath));

    auto otherProjectId = cue::ProjectId::parse("81234567-89ab-4cde-8f01-23456789abcd", a_assertContext);
    require(otherProjectId.has_value());
    auto otherDescriptor = cue::create_blank_project_descriptor(
        *otherProjectId.try_value(), "Other Project",
        cue::EngineCompatibility{cue::EngineVersion{1U, 0U, 0U}, cue::EngineVersion{2U, 0U, 0U}}, k_sceneId,
        a_assertContext);
    auto sceneId = cue::scene::SceneAssetId::parse(k_sceneId, a_assertContext);
    require(otherDescriptor.has_value() && sceneId.has_value());
    cue::scene::SceneDocument otherScene = cue::scene::SceneDocument::create(*sceneId.try_value(), a_assertContext);
    auto otherSnapshot = cue::scene::create_scene_snapshot(otherScene, a_assertContext);
    require(otherSnapshot.has_value());
    auto otherRuntimeData = cue::package::publish_minimal_runtime_data(*otherDescriptor.try_value(),
                                                                       *otherSnapshot.try_value(), a_assertContext);
    require(otherRuntimeData.has_value());
    const cue::RelativePath otherProjectDestination =
        take_value(cue::RelativePath::parse("OtherProjectPackage", a_assertContext));
    auto otherProjectResult = cue::package::publish_monolithic_runtime_package(
        *filesystem, projectRoot.generic_string(), reader, artifact, {1U, 0U, 0U}, *otherRuntimeData.try_value(),
        otherProjectDestination, cancellation, a_assertContext);
    require(!otherProjectResult.has_value() && reader.calls == 2U && !reader.active &&
            otherProjectResult.try_error()->root_code().domain() == "Cue.Package" &&
            otherProjectResult.try_error()->root_code().value() ==
                static_cast<std::int64_t>(cue::package::PackageError::PackageFileMismatch));

    const std::filesystem::path relocatedRoot = projectRoot / "RelocatedMonolithicPackage";
    std::filesystem::rename(packageRoot, relocatedRoot, error);
    require(!error && cue::package::verify_package_manifest_files(relocatedRoot.generic_string(), *manifest.try_value(),
                                                                  a_assertContext));

    TestArtifactReader injectionReader;
    std::unique_ptr<FailingFilesystemRoot> injectionFilesystem = std::make_unique<FailingFilesystemRoot>(
        open_root(projectRoot, a_assertContext), FailurePoint::InjectUnknownFile, a_assertContext);
    const cue::RelativePath injectionDestination =
        take_value(cue::RelativePath::parse("InjectedPackage", a_assertContext));
    auto injectionResult = cue::package::publish_monolithic_runtime_package(
        *injectionFilesystem, projectRoot.generic_string(), injectionReader, artifact, {1U, 0U, 0U}, runtimeData,
        injectionDestination, cancellation, a_assertContext);
    require(injectionResult.has_value() && !injectionResult.try_value()->succeeded() &&
            injectionResult.try_value()->stage == cue::package::PackagePublishStage::ValidateStaging &&
            !std::filesystem::exists(projectRoot / "InjectedPackage"));
    injectionFilesystem.reset();

    cue::BuildArtifactInventory signedArtifact =
        make_shipping_artifact(projectRoot, executableBytes, cue::ShippingTrustMode::PublisherSigned, a_assertContext);
    const cue::RelativePath signedDestination = take_value(cue::RelativePath::parse("SignedPackage", a_assertContext));
    auto signedResult = cue::package::publish_monolithic_runtime_package(
        *filesystem, projectRoot.generic_string(), reader, signedArtifact, {1U, 0U, 0U}, runtimeData, signedDestination,
        cancellation, a_assertContext);
    require(!signedResult.has_value() && reader.calls == 2U && !std::filesystem::exists(projectRoot / "SignedPackage"));

    require(write_bytes(artifactDirectory / "CueGameProduct.exe", bytes("corrupted-executable")));
    const cue::RelativePath corruptDestination =
        take_value(cue::RelativePath::parse("CorruptPackage", a_assertContext));
    auto corruptResult = cue::package::publish_monolithic_runtime_package(
        *filesystem, projectRoot.generic_string(), reader, artifact, {1U, 0U, 0U}, runtimeData, corruptDestination,
        cancellation, a_assertContext);
    require(!corruptResult.has_value() && reader.calls == 3U && !reader.active &&
            !std::filesystem::exists(projectRoot / "CorruptPackage"));

    require(write_bytes(artifactDirectory / "CueGameProduct.exe", executableBytes));
    cue::package::PackageCancellation cancelled;
    cancelled.request_cancel();
    const cue::RelativePath cancelledDestination =
        take_value(cue::RelativePath::parse("CancelledMonolithicPackage", a_assertContext));
    auto cancelledResult = cue::package::publish_monolithic_runtime_package(
        *filesystem, projectRoot.generic_string(), reader, artifact, {1U, 0U, 0U}, runtimeData, cancelledDestination,
        cancelled, a_assertContext);
    require(!cancelledResult.has_value() && reader.calls == 4U && !reader.active &&
            !std::filesystem::exists(projectRoot / "CancelledMonolithicPackage"));
    filesystem.reset();
    std::filesystem::remove_all(projectRoot, error);
    require(!error);
}

/// @brief 260文字を超えるProject RootとArtifact LocatorからMonolithic Packageを公開できることを検証する
void test_long_monolithic_project_root(const cue::AssertContext &a_assertContext)
{
    std::filesystem::path projectRoot = std::filesystem::temp_directory_path() /
                                        (L"CueMonolithicLongPathTests-" + std::to_wstring(GetCurrentProcessId()));
    constexpr std::wstring_view segment = L"segment-0123456789abcdef-0123456789abcdef-0123456789abcdef";
    while (projectRoot.native().size() <= 300U)
    {
        projectRoot /= segment;
    }
    const std::filesystem::path extendedProjectRoot = extended_path(projectRoot);
    std::error_code error;
    std::filesystem::remove_all(extendedProjectRoot, error);
    error.clear();
    require(std::filesystem::create_directories(extendedProjectRoot, error) && !error);

    const std::vector<std::byte> executableBytes = bytes("long-path-shipping-product");
    cue::BuildArtifactInventory artifact =
        make_shipping_artifact(projectRoot, executableBytes, cue::ShippingTrustMode::UnsignedLocal, a_assertContext);
    const std::filesystem::path artifactDirectory(artifact.version_directory());
    require(std::filesystem::create_directories(extended_path(artifactDirectory), error) && !error);
    require(write_bytes(extended_path(artifactDirectory / "CueGameProduct.exe"), executableBytes));

    TestArtifactReader reader;
    std::unique_ptr<cue::FilesystemRoot> filesystem = open_root(projectRoot, a_assertContext);
    const cue::package::MinimalRuntimeDataPublication runtimeData = make_runtime_data(a_assertContext);
    const cue::RelativePath destination = take_value(cue::RelativePath::parse("LongPathPackage", a_assertContext));
    cue::package::PackageCancellation cancellation;
    auto result = cue::package::publish_monolithic_runtime_package(*filesystem, projectRoot.generic_string(), reader,
                                                                   artifact, {1U, 0U, 0U}, runtimeData, destination,
                                                                   cancellation, a_assertContext);
    require(result.has_value() && result.try_value()->succeeded() && reader.calls == 1U && !reader.active);
    require(std::filesystem::is_regular_file(extended_path(projectRoot / "LongPathPackage" / "CueGameProduct.exe")));

    filesystem.reset();
    std::filesystem::remove_all(extendedProjectRoot, error);
    require(!error);
}
} // namespace

/// @brief Package Staging、Atomic Publish、Rollback、Failure Recoveryを検証する
int main(int a_argumentCount, char **a_arguments)
{
    require(a_argumentCount == 2);
    TestFatalHandler fatalHandler;
    std::vector<std::unique_ptr<cue::LogSink>> sinks;
    cue::Logger logger(fatalHandler, std::move(sinks));
    cue::AssertContext assertContext(logger, fatalHandler);

    const std::filesystem::path root =
        std::filesystem::absolute(std::filesystem::path(a_arguments[1]) / "CuePackagePublisherTests");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    require(!error && std::filesystem::create_directories(root));

    const std::vector<cue::package::PackageFilePayload> payloads = make_payloads(assertContext);
    const cue::package::PackageManifest manifest = make_manifest(payloads, assertContext);
    test_publish_contract(root, manifest, payloads, assertContext);
    test_reproducible_publication(root, manifest, payloads, assertContext);
    test_failure_injection(root, manifest, payloads, assertContext);
    test_cancellation_publish_boundary(root, manifest, payloads, assertContext);
    test_monolithic_publication(assertContext);
    test_long_monolithic_project_root(assertContext);

    std::filesystem::remove_all(root, error);
    require(!error);
    return 0;
}
