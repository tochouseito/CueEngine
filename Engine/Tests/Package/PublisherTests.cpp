#include <Cue/Package/Publisher.h>

#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Fatal.h>
#include <Cue/Foundation/Log.h>
#include <Cue/IO/Error.h>
#include <Cue/IO/Windows/WindowsFilesystem.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace
{
constexpr std::string_view k_projectId = "41234567-89ab-4cde-8f01-23456789abcd";
constexpr std::string_view k_sceneId = "51234567-89ab-4cde-8f01-23456789abcd";
constexpr std::string_view k_scenePath = "Data/Scenes/51234567-89ab-4cde-8f01-23456789abcd.cueruntime.json";

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

/// @brief ASCII Test Fixtureを所有Byte列へ変換する
[[nodiscard]] std::vector<std::byte> bytes(std::string_view a_text)
{
    const std::span<const char> characters(a_text.data(), a_text.size());
    const std::span<const std::byte> raw = std::as_bytes(characters);
    return std::vector<std::byte>(raw.begin(), raw.end());
}

/// @brief 5個の必須Roleを持つ最小Package Payloadを構築する
[[nodiscard]] std::vector<cue::package::PackageFilePayload> make_payloads(
    const cue::AssertContext &a_assertContext)
{
    using cue::package::PackageFilePayload;
    using cue::package::PackageFileRole;
    std::vector<PackageFilePayload> payloads;
    payloads.push_back(take_value(PackageFilePayload::create(
        PackageFileRole::RuntimeHost, "CueRuntimeHost.exe", bytes("runtime-host"), a_assertContext)));
    payloads.push_back(take_value(PackageFilePayload::create(
        PackageFileRole::GameModule, "Game/CueGameModule.dll", bytes("game-module"), a_assertContext)));
    payloads.push_back(take_value(PackageFilePayload::create(PackageFileRole::GameModuleMetadata,
                                                             "Game/CueGameModule.metadata.json", bytes("metadata"),
                                                             a_assertContext)));
    payloads.push_back(take_value(PackageFilePayload::create(PackageFileRole::ProjectRuntimeData,
                                                             "Data/CueProject.runtime.json", bytes("project-data"),
                                                             a_assertContext)));
    payloads.push_back(take_value(PackageFilePayload::create(PackageFileRole::StartupSceneRuntimeData,
                                                             std::string(k_scenePath), bytes("scene-data"),
                                                             a_assertContext)));
    return payloads;
}

/// @brief Payload Entry列から検証済みManifestを構築する
[[nodiscard]] cue::package::PackageManifest make_manifest(
    std::span<const cue::package::PackageFilePayload> a_payloads, const cue::AssertContext &a_assertContext)
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
    BlockPublish
};

/// @brief 実Windows Filesystemへ委譲しPackage Stageだけを一度失敗させるTest Double
class FailingFilesystemRoot final : public cue::FilesystemRoot
{
  public:
    /// @brief 委譲先Rootと一度だけ消費するFailure Pointを所有する
    FailingFilesystemRoot(std::unique_ptr<cue::FilesystemRoot> a_inner, FailurePoint a_failure,
                          const cue::AssertContext &a_assertContext) noexcept
        : m_inner(std::move(a_inner)), m_failure(a_failure), m_assertContext(&a_assertContext)
    {
    }
    /// @brief 委譲先Rootを解放する
    ~FailingFilesystemRoot() override = default;

    /// @brief Publish開始がFilesystem境界へ到達したか期限付きで待機して返す
    [[nodiscard]] bool wait_until_publish_entered() const noexcept
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!m_publishEntered.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::yield();
        }
        return m_publishEntered.load(std::memory_order_acquire);
    }

    /// @brief 待機中のPublishを実Filesystemへ進める
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
        return m_inner->write_file_atomic(a_path, a_bytes);
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
    [[nodiscard]] cue::Result<void> write_file_atomic_if_unchanged(
        cue::FileWriteLease &a_lease, const cue::RelativePath &a_path, cue::FileFingerprint a_expected,
        std::size_t a_maximumExpectedBytes, std::span<const std::byte> a_bytes) noexcept override
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
    [[nodiscard]] cue::Result<void> publish_staging_area(cue::StagingArea &&a_staging,
                                                         const cue::RelativePath &a_destination) noexcept override
    {
        if (m_failure == FailurePoint::BlockPublish)
        {
            m_publishEntered.store(true, std::memory_order_release);
            while (!m_allowPublish.load(std::memory_order_acquire))
            {
                std::this_thread::yield();
            }
        }
        if (consume(FailurePoint::Publish))
        {
            return cue::Result<void>::failure(make_failure("Package publish failed"));
        }
        if (consume(FailurePoint::Durability))
        {
            auto published = m_inner->publish_staging_area(std::move(a_staging), a_destination);
            if (!published)
            {
                return published;
            }
            return cue::Result<void>::failure(cue::make_io_error(
                *m_assertContext, cue::IoError::DurabilityUnknown, "Published Package durability is unknown"));
        }
        return m_inner->publish_staging_area(std::move(a_staging), a_destination);
    }
    /// @brief Rollback Failureを一度注入するかOperation所有Staging削除を委譲する
    [[nodiscard]] cue::Result<void> rollback_staging_area(cue::StagingArea &&a_staging) noexcept override
    {
        if (m_failure == FailurePoint::WriteAndRollback && m_writeRollbackFailureConsumed &&
            !m_rollbackFailureConsumed)
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
    std::atomic_bool m_publishEntered = false;
    std::atomic_bool m_allowPublish = false;
};

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

    cue::RelativePath cancelledDestination =
        take_value(cue::RelativePath::parse("CancelledPackage", a_assertContext));
    cue::package::PackageCancellation cancelled;
    cancelled.request_cancel();
    auto cancelledReport = cue::package::publish_runtime_package(*filesystem, cancelledDestination, a_manifest,
                                                                 a_payloads, cancelled, a_assertContext);
    require(cancelledReport.outcome == cue::package::PackagePublishOutcome::NotPublished);
    require(!std::filesystem::exists(a_root / "CancelledPackage"));
}

/// @brief Publish前Failure、Rollback Recovery、Publish後DurabilityUnknownを検証する
void test_failure_injection(const std::filesystem::path &a_root,
                            const cue::package::PackageManifest &a_manifest,
                            std::span<const cue::package::PackageFilePayload> a_payloads,
                            const cue::AssertContext &a_assertContext)
{
    constexpr std::array failures = {FailurePoint::CreateStaging, FailurePoint::Write,
                                     FailurePoint::ValidateStaging, FailurePoint::Publish};
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
    cue::RelativePath recoveryDestination =
        take_value(cue::RelativePath::parse("RecoveryPackage", a_assertContext));
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
    cue::RelativePath durabilityDestination =
        take_value(cue::RelativePath::parse("UnknownPackage", a_assertContext));
    cue::package::PackageCancellation durabilityCancellation;
    auto durability = cue::package::publish_runtime_package(durabilityFilesystem, durabilityDestination, a_manifest,
                                                            a_payloads, durabilityCancellation, a_assertContext);
    require(durability.outcome == cue::package::PackagePublishOutcome::PublishedButDurabilityUnknown);
    require(!durability.succeeded() && durability.error.has_value() && !durability.recoveryStaging.has_value());
    require(std::filesystem::is_regular_file(a_root / "UnknownPackage" / "CuePackage.json"));
}

/// @brief Publish開始確定後のCancelがCommit済み境界を巻き戻さないことを検証する
void test_cancellation_publish_boundary(const std::filesystem::path &a_root,
                                        const cue::package::PackageManifest &a_manifest,
                                        std::span<const cue::package::PackageFilePayload> a_payloads,
                                        const cue::AssertContext &a_assertContext)
{
    FailingFilesystemRoot filesystem(open_root(a_root, a_assertContext), FailurePoint::BlockPublish,
                                     a_assertContext);
    cue::RelativePath destination =
        take_value(cue::RelativePath::parse("PublishBoundaryPackage", a_assertContext));
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

    require(publishEntered && report.has_value() && report->succeeded());
    require(std::filesystem::is_regular_file(a_root / "PublishBoundaryPackage" / "CuePackage.json"));
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
    test_failure_injection(root, manifest, payloads, assertContext);
    test_cancellation_publish_boundary(root, manifest, payloads, assertContext);

    std::filesystem::remove_all(root, error);
    require(!error);
    return 0;
}
