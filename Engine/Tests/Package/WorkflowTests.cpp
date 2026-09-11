#include <Cue/Package/Workflow.h>

#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Fatal.h>
#include <Cue/Foundation/Log.h>
#include <Cue/IO/Error.h>
#include <Cue/IO/Windows/WindowsFilesystem.h>
#include <Cue/Project/Generator.h>
#include <Cue/Scene/Identity.h>
#include <Cue/Scene/Instantiation.h>
#include <Cue/Scene/SceneDocument.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
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
constexpr std::string_view k_projectId = "00000000-0000-4000-8000-000000000901";
constexpr std::string_view k_sceneId = "10000000-0000-4000-8000-000000000001";
constexpr cue::BuildWorkspaceCompatibility k_workspaceCompatibility{
    cue::BuildGenerator::VisualStudio2026, cue::BuildArchitecture::X64, {19U, 51U, 0U, 0U}, 1U};

/// @brief Test内のFatalを固定Exit Codeへ変換する
class TestFatalHandler final : public cue::FatalHandler
{
  public:
    /// @brief MessageなしFatalを固定Exit Codeへ変換する
    [[noreturn]] void terminate() noexcept override
    {
        std::_Exit(76);
    }

    /// @brief Message付きFatalを固定Exit Codeへ変換する
    [[noreturn]] void terminate(std::string_view) noexcept override
    {
        std::_Exit(76);
    }
};

/// @brief Test所有の二RootをProcess終了時に除去する
class TestDirectory final
{
  public:
    /// @brief Process固有のTemporary Rootを作成する
    TestDirectory()
        : m_path(std::filesystem::temp_directory_path() /
                 (L"CuePackageWorkflowTests-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                  std::to_wstring(GetTickCount64())))
    {
        std::filesystem::create_directories(m_path);
    }

    TestDirectory(const TestDirectory &) = delete;
    TestDirectory &operator=(const TestDirectory &) = delete;

    /// @brief Test所有Rootだけを除去する
    ~TestDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(m_path, error);
    }

    /// @brief Temporary Rootを返す
    [[nodiscard]] const std::filesystem::path &path() const noexcept
    {
        return m_path;
    }

  private:
    std::filesystem::path m_path;
};

enum class RunnerMode : std::uint8_t
{
    Succeed,
    Fail,
    BlockUntilCancelled
};

struct RunnerState final
{
    std::atomic<RunnerMode> mode = RunnerMode::Succeed;
    std::atomic<cue::ChildProcessCancellationMode> lastCancellationMode = cue::ChildProcessCancellationMode::None;
    std::atomic<std::uint32_t> calls = 0U;
    std::atomic<std::size_t> maximumCapturedOutputBytes = 0U;
    std::atomic<bool> active = false;
};

/// @brief BuildまたはRuntimeの成功、失敗、取消待機をProcessなしで再現する
class ControlledRunner final : public cue::ChildProcessRunner
{
  public:
    /// @brief 共有制御状態を借用する
    explicit ControlledRunner(RunnerState &a_state) noexcept : m_state(&a_state)
    {
    }

    /// @brief 指定Modeに応じた所有Process結果を返す
    [[nodiscard]] cue::Result<cue::ChildProcessResult> run(
        const cue::ChildProcessRequest &a_request,
        const cue::ChildProcessCancellation &a_cancellation) noexcept override
    {
        m_state->maximumCapturedOutputBytes.store(a_request.maximum_captured_output_bytes().value_or(0U),
                                                  std::memory_order_release);
        m_state->active.store(true, std::memory_order_release);
        const std::uint32_t call = m_state->calls.fetch_add(1U, std::memory_order_relaxed);
        while (m_state->mode.load(std::memory_order_acquire) == RunnerMode::BlockUntilCancelled &&
               !a_cancellation.is_cancel_requested())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        m_state->active.store(false, std::memory_order_release);
        m_state->lastCancellationMode.store(a_cancellation.cancellation_mode(), std::memory_order_release);
        if (a_cancellation.is_cancel_requested())
        {
            return cue::Result<cue::ChildProcessResult>::success(cue::ChildProcessResult::cancelled({}));
        }
        const std::uint32_t exitCode = m_state->mode.load(std::memory_order_acquire) == RunnerMode::Fail ? 2U : 0U;
        return cue::Result<cue::ChildProcessResult>::success(cue::ChildProcessResult::exited(
            exitCode, {{call, cue::ChildProcessStream::StandardOutput, "process-log"}}));
    }

  private:
    RunnerState *m_state;
};

struct PublisherState final
{
    std::atomic<bool> corruptInventory = false;
    std::atomic<bool> invalidPortableExecutable = false;
    std::atomic<bool> oversizedRuntimePeImage = false;
    std::atomic<bool> oversizedRuntimePeInventory = false;
    std::atomic<bool> readerLeaseActive = false;
    std::atomic<bool> artifactReadWithoutLease = false;
    std::atomic<bool> packageWriteWithLease = false;
    std::atomic<std::uint32_t> calls = 0U;
    std::atomic<std::uint32_t> readerCalls = 0U;
    std::atomic<std::uint32_t> artifactReadCalls = 0U;
};

struct RecoveryFilesystemState final
{
    std::atomic<std::uint32_t> writeFailuresRemaining = 0U;
    std::atomic<std::uint32_t> durabilityFailuresRemaining = 0U;
    std::atomic<std::uint32_t> rollbackFailuresRemaining = 0U;
    std::atomic<std::uint32_t> rollbackCalls = 0U;
};

/// @brief Package WriteとRollbackの連続失敗を注入し、保持Tokenの再試行を検証するRoot
class RecoveryFilesystemRoot final : public cue::FilesystemRoot
{
  public:
    /// @brief 委譲先Rootと共有Failure状態を所有する
    RecoveryFilesystemRoot(std::unique_ptr<cue::FilesystemRoot> a_inner, RecoveryFilesystemState &a_state,
                           PublisherState &a_publisherState, const cue::AssertContext &a_assertContext) noexcept
        : m_inner(std::move(a_inner)), m_state(&a_state), m_publisherState(&a_publisherState),
          m_assertContext(&a_assertContext)
    {
    }

    /// @brief 委譲先Rootを解放する
    ~RecoveryFilesystemRoot() override = default;

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

    /// @brief File読取りを委譲する
    [[nodiscard]] cue::Result<std::vector<std::byte>> read_file(const cue::RelativePath &a_path,
                                                                std::size_t a_maxBytes) noexcept override
    {
        if (a_path.text().starts_with("Generated/Artifacts/") &&
            !m_publisherState->readerLeaseActive.load(std::memory_order_acquire))
        {
            m_publisherState->artifactReadWithoutLease.store(true, std::memory_order_release);
        }
        if (a_path.text().starts_with("Generated/Artifacts/"))
        {
            m_publisherState->artifactReadCalls.fetch_add(1U, std::memory_order_relaxed);
        }
        return m_inner->read_file(a_path, a_maxBytes);
    }

    /// @brief Directory作成を委譲する
    [[nodiscard]] cue::Result<void> create_directories(const cue::RelativePath &a_path) noexcept override
    {
        return m_inner->create_directories(a_path);
    }

    /// @brief 指定回数だけPackage Writeを失敗させ、それ以外を委譲する
    [[nodiscard]] cue::Result<void> write_file_atomic(const cue::RelativePath &a_path,
                                                      std::span<const std::byte> a_bytes) noexcept override
    {
        if (a_path.text().starts_with("Generated/Packages/") &&
            m_publisherState->readerLeaseActive.load(std::memory_order_acquire))
        {
            m_publisherState->packageWriteWithLease.store(true, std::memory_order_release);
        }
        if (consume(m_state->writeFailuresRemaining))
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

    /// @brief Staging作成を委譲する
    [[nodiscard]] cue::Result<cue::StagingArea> create_staging_area(
        const cue::RelativePath &a_destination) noexcept override
    {
        return m_inner->create_staging_area(a_destination);
    }

    /// @brief Staging公開を委譲する
    [[nodiscard]] cue::Result<void> publish_staging_area(
        cue::StagingArea &&a_staging, const cue::RelativePath &a_destination,
        const cue::StagingPublishAuthorization *a_authorization = nullptr) noexcept override
    {
        cue::Result<void> published =
            m_inner->publish_staging_area(std::move(a_staging), a_destination, a_authorization);
        if (published && consume(m_state->durabilityFailuresRemaining))
        {
            return cue::Result<void>::failure(cue::make_io_error(*m_assertContext, cue::IoError::DurabilityUnknown,
                                                                 "Published Package durability is unknown"));
        }
        return published;
    }

    /// @brief 呼出回数を記録し、指定回数だけRollbackを失敗させる
    [[nodiscard]] cue::Result<void> rollback_staging_area(cue::StagingArea &&a_staging) noexcept override
    {
        m_state->rollbackCalls.fetch_add(1U, std::memory_order_relaxed);
        if (consume(m_state->rollbackFailuresRemaining))
        {
            return cue::Result<void>::failure(make_failure("Package staging rollback failed"));
        }
        return m_inner->rollback_staging_area(std::move(a_staging));
    }

  private:
    /// @brief 残りFailure回数を一つ消費できたか返す
    [[nodiscard]] static bool consume(std::atomic<std::uint32_t> &a_remaining) noexcept
    {
        std::uint32_t remaining = a_remaining.load(std::memory_order_acquire);
        while (remaining != 0U)
        {
            if (a_remaining.compare_exchange_weak(remaining, remaining - 1U, std::memory_order_acq_rel,
                                                  std::memory_order_acquire))
            {
                return true;
            }
        }
        return false;
    }

    /// @brief Test用Portable IO Errorを構築する
    [[nodiscard]] cue::Error make_failure(std::string_view a_summary) const noexcept
    {
        return cue::make_io_error(*m_assertContext, cue::IoError::IoFailure, a_summary);
    }

    std::unique_ptr<cue::FilesystemRoot> m_inner;
    RecoveryFilesystemState *m_state;
    PublisherState *m_publisherState;
    const cue::AssertContext *m_assertContext;
};

/// @brief 失敗した検証の呼出位置を標準エラーへ出す
[[nodiscard]] bool require(bool a_condition, std::source_location a_location = std::source_location::current()) noexcept
{
    if (!a_condition)
    {
        std::cerr << "Requirement failed at " << a_location.file_name() << ':' << a_location.line() << '\n';
    }
    return a_condition;
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

/// @brief Importを持たない最小x64 PE Test Imageを生成する
[[nodiscard]] std::vector<std::byte> make_test_pe()
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
    write_u32(bytes, optional + 32U, 0x1000U);
    write_u32(bytes, optional + 36U, 0x200U);
    write_u32(bytes, optional + 56U, 0x2000U);
    write_u32(bytes, optional + 60U, 0x200U);
    write_u32(bytes, optional + 108U, 16U);
    constexpr std::size_t section = 0x188U;
    write_u32(bytes, section + 8U, 0x0e00U);
    write_u32(bytes, section + 12U, 0x1000U);
    write_u32(bytes, section + 16U, 0x0e00U);
    write_u32(bytes, section + 20U, 0x200U);
    return bytes;
}

/// @brief Build成功ArtifactをTest Rootへ実体化するPublisher
class MaterializingPublisher final : public cue::BuildArtifactPublisher
{
  public:
    /// @brief Native Resourceを持たないTest Lease
    class Lease final : public cue::BuildWorkspaceLease
    {
      public:
        /// @brief 空Leaseを構築する
        Lease() noexcept = default;
        /// @brief 空Leaseを破棄する
        ~Lease() override = default;
    };

    /// @brief Artifact Rootと制御状態を借用する
    MaterializingPublisher(PublisherState &a_state, const cue::AssertContext &a_assertContext) noexcept
        : m_state(&a_state), m_assertContext(&a_assertContext)
    {
    }

    /// @brief 取消前なら空のExclusive Leaseを返す
    [[nodiscard]] cue::Result<std::optional<std::unique_ptr<cue::BuildWorkspaceLease>>> acquire_build_lease(
        const cue::BuildPlan &, const cue::ChildProcessCancellation &a_cancellation,
        cue::BuildArtifactLockDeadline) noexcept override
    {
        if (a_cancellation.is_cancel_requested())
        {
            return cue::Result<std::optional<std::unique_ptr<cue::BuildWorkspaceLease>>>::success(std::nullopt);
        }
        return cue::Result<std::optional<std::unique_ptr<cue::BuildWorkspaceLease>>>::success(
            std::optional<std::unique_ptr<cue::BuildWorkspaceLease>>(std::make_unique<Lease>()));
    }

    /// @brief Operation固有Versionへ必須ArtifactとPackage対象外PDBを書きInventoryを返す
    [[nodiscard]] cue::Result<std::optional<cue::BuildArtifactInventory>> publish(
        const cue::BuildPlan &a_plan, const cue::ChildProcessCancellation &a_cancellation,
        std::unique_ptr<cue::BuildWorkspaceLease> a_buildLease, cue::BuildArtifactLockDeadline) noexcept override
    {
        static_cast<void>(a_buildLease);
        if (a_cancellation.is_cancel_requested())
        {
            return cue::Result<std::optional<cue::BuildArtifactInventory>>::success(std::nullopt);
        }
        m_state->calls.fetch_add(1U, std::memory_order_relaxed);
        const std::vector<std::byte> moduleBytes = m_state->invalidPortableExecutable.load(std::memory_order_acquire)
                                                       ? text_bytes("test-game-module")
                                                       : make_test_pe();
        const std::vector<std::byte> pdbBytes = text_bytes("test-debug-symbols");
        const std::vector<std::byte> metadataBytes = text_bytes("{\"schemaVersion\":1}\n");
        const bool hasOversizedInventory = m_state->oversizedRuntimePeInventory.load(std::memory_order_acquire);
        auto modulePayload = cue::package::PackageFilePayload::create(
            cue::package::PackageFileRole::GameModule, "CueGameModule.dll", moduleBytes, *m_assertContext);
        auto metadataPayload =
            cue::package::PackageFilePayload::create(cue::package::PackageFileRole::GameModuleMetadata,
                                                     "CueGameModule.metadata.json", metadataBytes, *m_assertContext);
        if (!modulePayload || !metadataPayload)
        {
            return cue::Result<std::optional<cue::BuildArtifactInventory>>::failure(
                modulePayload ? std::move(*metadataPayload.try_error()) : std::move(*modulePayload.try_error()));
        }
        const std::string artifactId(a_plan.operation_id());
        const std::filesystem::path versionDirectory = std::filesystem::path(a_plan.project_root()) /
                                                       std::filesystem::path(a_plan.artifact_store_directory()) /
                                                       L"Versions" / std::filesystem::path(artifactId);
        std::error_code error;
        std::filesystem::create_directories(versionDirectory, error);
        if (error || !write_file(versionDirectory / L"CueGameModule.dll", moduleBytes) ||
            !write_file(versionDirectory / L"CueGameModule.pdb", pdbBytes) ||
            !write_file(versionDirectory / L"CueGameModule.metadata.json", metadataBytes) ||
            (hasOversizedInventory && (!write_file(versionDirectory / L"RuntimeDependencyA.dll", moduleBytes) ||
                                       !write_file(versionDirectory / L"RuntimeDependencyB.dll", moduleBytes))))
        {
            cue::ErrorCode code = cue::ErrorCode::create(m_assertContext->fatal_handler(), "Cue.Package.Test", 1);
            return cue::Result<std::optional<cue::BuildArtifactInventory>>::failure(cue::Error::create(
                m_assertContext->fatal_handler(), std::move(code), "Artifact materialization failed"));
        }
        std::string moduleHash(modulePayload.try_value()->entry().sha256());
        if (m_state->corruptInventory.load(std::memory_order_acquire))
        {
            moduleHash.assign(64U, 'f');
        }
        const std::uint64_t moduleSize = m_state->oversizedRuntimePeImage.load(std::memory_order_acquire)
                                             ? cue::package::k_maximumRuntimePeImageBytes + 1U
                                             : moduleBytes.size();
        std::vector<cue::BuildArtifactFile> files = {{"CueGameModule.dll", moduleSize, std::move(moduleHash)},
                                                     {"CueGameModule.pdb", pdbBytes.size(), std::string(64U, 'a')},
                                                     {"CueGameModule.metadata.json", metadataBytes.size(),
                                                      std::string(metadataPayload.try_value()->entry().sha256())}};
        if (hasOversizedInventory)
        {
            const std::string dependencyHash(modulePayload.try_value()->entry().sha256());
            files.push_back({"RuntimeDependencyA.dll", cue::package::k_maximumRuntimePeImageBytes, dependencyHash});
            files.push_back({"RuntimeDependencyB.dll", cue::package::k_maximumRuntimePeImageBytes, dependencyHash});
        }
        auto inventory = cue::BuildArtifactInventory::create(a_plan, artifactId, std::move(files), *m_assertContext);
        if (!inventory)
        {
            return cue::Result<std::optional<cue::BuildArtifactInventory>>::failure(std::move(*inventory.try_error()));
        }
        return cue::Result<std::optional<cue::BuildArtifactInventory>>::success(
            std::optional<cue::BuildArtifactInventory>(std::move(*inventory.try_value())));
    }

  private:
    /// @brief ASCII Test入力を所有Byte列へ変換する
    [[nodiscard]] static std::vector<std::byte> text_bytes(std::string_view a_text)
    {
        const std::span<const char> characters(a_text.data(), a_text.size());
        const std::span<const std::byte> bytes = std::as_bytes(characters);
        return {bytes.begin(), bytes.end()};
    }

    /// @brief Test Artifact Byte列をBinary Fileへ書く
    [[nodiscard]] static bool write_file(const std::filesystem::path &a_path,
                                         std::span<const std::byte> a_bytes) noexcept
    {
        std::ofstream stream(a_path, std::ios::binary | std::ios::trunc);
        stream.write(reinterpret_cast<const char *>(a_bytes.data()), static_cast<std::streamsize>(a_bytes.size()));
        return stream.good();
    }

    PublisherState *m_state;
    const cue::AssertContext *m_assertContext;
};

/// @brief WorkflowがArtifact読込中だけ保持するTest用Shared Read Leaseを発行するReader
class MaterializingArtifactReader final : public cue::BuildArtifactReader
{
  public:
    /// @brief Leaseの取得中状態を共有Test Stateへ反映するToken
    class Lease final : public cue::BuildArtifactReadLease
    {
      public:
        /// @brief Shared Read Lease取得を記録する
        explicit Lease(PublisherState &a_state) noexcept : m_state(&a_state)
        {
            m_state->readerLeaseActive.store(true, std::memory_order_release);
        }

        /// @brief Shared Read Lease解放を記録する
        ~Lease() override
        {
            m_state->readerLeaseActive.store(false, std::memory_order_release);
        }

      private:
        PublisherState *m_state;
    };

    /// @brief 共有Test Stateを借用する
    explicit MaterializingArtifactReader(PublisherState &a_state) noexcept : m_state(&a_state)
    {
    }

    /// @brief 取消前なら読込範囲を可視化する空Leaseを返す
    [[nodiscard]] cue::Result<std::optional<std::unique_ptr<cue::BuildArtifactReadLease>>> acquire_current_read_lease(
        const cue::BuildArtifactInventory &, const cue::BuildArtifactReadCancellation &a_cancellation,
        cue::BuildArtifactLockDeadline) noexcept override
    {
        m_state->readerCalls.fetch_add(1U, std::memory_order_relaxed);
        if (a_cancellation.is_cancel_requested())
        {
            return cue::Result<std::optional<std::unique_ptr<cue::BuildArtifactReadLease>>>::success(std::nullopt);
        }
        std::unique_ptr<cue::BuildArtifactReadLease> lease = std::make_unique<Lease>(*m_state);
        return cue::Result<std::optional<std::unique_ptr<cue::BuildArtifactReadLease>>>::success(
            std::optional<std::unique_ptr<cue::BuildArtifactReadLease>>(std::move(lease)));
    }

  private:
    PublisherState *m_state;
};

/// @brief Processを起動しないBuild Runner設定を返す
[[nodiscard]] cue::CMakeRunnerSettings make_settings()
{
    return {"C:/Tools/cmake.exe", "C:/CueEngine", {}, std::chrono::seconds(5), std::chrono::seconds(5),
            "14.51.36231"};
}

/// @brief Debug Game Module用Build Requestを作る
[[nodiscard]] cue::BuildRequest make_request(std::string a_projectRoot, std::string a_operationId,
                                             const cue::AssertContext &a_assertContext)
{
    auto profile =
        cue::BuildProfile::create(cue::BuildConfiguration::Debug, cue::BuildTarget::GameModule, a_assertContext);
    return {std::move(a_projectRoot), *profile.try_value(), std::move(a_operationId), k_workspaceCompatibility};
}

/// @brief 空Sceneから決定的Runtime Data Publicationを作る
[[nodiscard]] cue::Result<cue::package::MinimalRuntimeDataPublication> make_runtime_data(
    const cue::AssertContext &a_assertContext) noexcept
{
    auto projectId = cue::ProjectId::parse(k_projectId, a_assertContext);
    auto descriptor =
        projectId ? cue::create_blank_project_descriptor(
                        *projectId.try_value(), "Workflow Test",
                        cue::EngineCompatibility{cue::EngineVersion{1U, 0U, 0U}, cue::EngineVersion{2U, 0U, 0U}},
                        k_sceneId, a_assertContext)
                  : cue::Result<cue::ProjectDescriptor>::failure(std::move(*projectId.try_error()));
    auto sceneId = cue::scene::SceneAssetId::parse(k_sceneId, a_assertContext);
    if (!descriptor || !sceneId)
    {
        return cue::Result<cue::package::MinimalRuntimeDataPublication>::failure(
            descriptor ? std::move(*sceneId.try_error()) : std::move(*descriptor.try_error()));
    }
    cue::scene::SceneDocument scene = cue::scene::SceneDocument::create(*sceneId.try_value(), a_assertContext);
    auto snapshot = cue::scene::create_scene_snapshot(scene, a_assertContext);
    return snapshot
               ? cue::package::publish_minimal_runtime_data(*descriptor.try_value(), *snapshot.try_value(),
                                                            a_assertContext)
               : cue::Result<cue::package::MinimalRuntimeDataPublication>::failure(std::move(*snapshot.try_error()));
}

/// @brief RunnerがProcess実行中になるまで上限付きで待つ
[[nodiscard]] bool wait_until_active(const RunnerState &a_state) noexcept
{
    for (std::uint32_t attempt = 0U; attempt < 1000U; ++attempt)
    {
        if (a_state.active.load(std::memory_order_acquire))
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

/// @brief 指定回数目のProcessが終了しBuild Serviceへ結果を渡すまで上限付きで待つ
[[nodiscard]] bool wait_until_completed_call(const RunnerState &a_state, std::uint32_t a_minimumCalls) noexcept
{
    for (std::uint32_t attempt = 0U; attempt < 1000U; ++attempt)
    {
        if (a_state.calls.load(std::memory_order_acquire) >= a_minimumCalls &&
            !a_state.active.load(std::memory_order_acquire))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

/// @brief Build・Package・Runの成功、失敗保全、停止をHeadlessで検証する
[[nodiscard]] bool test_workflow(const cue::AssertContext &a_assertContext)
{
    TestDirectory directory;
    const std::filesystem::path projectRoot = directory.path() / L"Project";
    const std::filesystem::path engineRoot = directory.path() / L"Engine";
    const std::filesystem::path hostPath = engineRoot / L"bin" / L"Debug" / L"CueRuntimeHost.exe";
    std::filesystem::create_directories(projectRoot);
    std::filesystem::create_directories(hostPath.parent_path());
    {
        const std::vector<std::byte> hostBytes = make_test_pe();
        std::ofstream host(hostPath, std::ios::binary);
        host.write(reinterpret_cast<const char *>(hostBytes.data()), static_cast<std::streamsize>(hostBytes.size()));
    }

    RunnerState buildRunner;
    RunnerState runRunner;
    PublisherState publisher;
    RecoveryFilesystemState recoveryFilesystem;
    auto build = cue::GameBuildService::create(make_settings(), std::make_unique<ControlledRunner>(buildRunner),
                                               std::make_unique<MaterializingPublisher>(publisher, a_assertContext),
                                               a_assertContext);
    auto projectFilesystem = cue::create_windows_filesystem_root(projectRoot.generic_string(), a_assertContext);
    auto engineFilesystem = cue::create_windows_filesystem_root(engineRoot.generic_string(), a_assertContext);
    if (!require(build && projectFilesystem && engineFilesystem))
    {
        return false;
    }
    std::unique_ptr<cue::GameBuildService> buildService = std::move(*build.try_value());
    auto guardedProjectFilesystem = std::make_unique<RecoveryFilesystemRoot>(
        std::move(*projectFilesystem.try_value()), recoveryFilesystem, publisher, a_assertContext);
    auto workflow = cue::package::GamePackageWorkflowService::create(
        std::move(buildService), std::make_unique<MaterializingArtifactReader>(publisher),
        std::move(guardedProjectFilesystem), std::move(*engineFilesystem.try_value()),
        std::make_unique<ControlledRunner>(runRunner), projectRoot.generic_string(), {}, a_assertContext);
    auto runtimeData = make_runtime_data(a_assertContext);
    if (!require(workflow && runtimeData))
    {
        return false;
    }
    std::unique_ptr<cue::package::GamePackageWorkflowService> service = std::move(*workflow.try_value());
    constexpr std::string_view firstOperation = "01234567-89ab-4cde-8f01-23456789abcd";
    if (!require(
            service->start(make_request(projectRoot.generic_string(), std::string(firstOperation), a_assertContext),
                           cue::CMakeConfigureMode::Required, {1U, 0U, 0U}, std::string(k_projectId),
                           *runtimeData.try_value()) &&
            service->wait_for_package()))
    {
        return false;
    }
    cue::package::PackageWorkflowSnapshot first = service->snapshot();
    if (first.state != cue::package::PackageWorkflowState::PackageReady)
    {
        std::cerr << "Initial package state=" << static_cast<int>(first.state) << " message=" << first.message << '\n';
    }
    if (!require(first.state == cue::package::PackageWorkflowState::PackageReady && first.package &&
                 first.latestSuccessfulPackage && first.package->operationId == firstOperation &&
                 publisher.readerCalls.load(std::memory_order_acquire) == 1U &&
                 !publisher.readerLeaseActive.load(std::memory_order_acquire) &&
                 !publisher.artifactReadWithoutLease.load(std::memory_order_acquire) &&
                 !publisher.packageWriteWithLease.load(std::memory_order_acquire) &&
                 std::filesystem::exists(projectRoot / std::filesystem::path(first.package->destination) /
                                         L"CuePackage.json") &&
                 !std::filesystem::exists(projectRoot / std::filesystem::path(first.package->destination) / L"Runtime" /
                                          L"CueGameModule.pdb")))
    {
        return false;
    }
    const std::string firstDestination = first.package->destination;

    const std::uint32_t buildCallsBeforeCancel = buildRunner.calls.load(std::memory_order_acquire);
    constexpr std::string_view cancelledOperation = "09234567-89ab-4cde-8f01-23456789abcd";
    if (!require(
            service->start(make_request(projectRoot.generic_string(), std::string(cancelledOperation), a_assertContext),
                           cue::CMakeConfigureMode::Required, {1U, 0U, 0U}, std::string(k_projectId),
                           *runtimeData.try_value()) &&
            wait_until_completed_call(buildRunner, buildCallsBeforeCancel + 1U) && service->request_cancel() &&
            service->wait_for_package()))
    {
        return false;
    }
    const cue::package::PackageWorkflowSnapshot transitionCancelled = service->snapshot();
    if (!require(transitionCancelled.state == cue::package::PackageWorkflowState::Cancelled &&
                 !transitionCancelled.package && transitionCancelled.latestSuccessfulPackage &&
                 transitionCancelled.latestSuccessfulPackage->destination == firstDestination &&
                 !std::filesystem::exists(projectRoot / L"Generated" / L"Packages" / L"Debug" /
                                          std::filesystem::path(cancelledOperation))))
    {
        return false;
    }
    const std::uint32_t publisherCallsAfterTransitionCancel = publisher.calls.load(std::memory_order_acquire);

    buildRunner.mode.store(RunnerMode::Fail, std::memory_order_release);
    if (!require(service->start(make_request(projectRoot.generic_string(), "11234567-89ab-4cde-8f01-23456789abcd",
                                             a_assertContext),
                                cue::CMakeConfigureMode::Required, {1U, 0U, 0U}, std::string(k_projectId),
                                *runtimeData.try_value()) &&
                 service->wait_for_package()))
    {
        return false;
    }
    cue::package::PackageWorkflowSnapshot buildFailed = service->snapshot();
    if (!require(buildFailed.state == cue::package::PackageWorkflowState::Failed && !buildFailed.package &&
                 buildFailed.latestSuccessfulPackage &&
                 buildFailed.latestSuccessfulPackage->destination == firstDestination &&
                 publisher.calls.load(std::memory_order_acquire) == publisherCallsAfterTransitionCancel))
    {
        return false;
    }

    buildRunner.mode.store(RunnerMode::Succeed, std::memory_order_release);
    publisher.invalidPortableExecutable.store(true, std::memory_order_release);
    if (!require(service->retry("16234567-89ab-4cde-8f01-23456789abcd", {1U, 0U, 0U}, std::string(k_projectId),
                                *runtimeData.try_value()) &&
                 service->wait_for_package()))
    {
        return false;
    }
    cue::package::PackageWorkflowSnapshot invalidPe = service->snapshot();
    if (!require(invalidPe.state == cue::package::PackageWorkflowState::Failed && !invalidPe.package &&
                 invalidPe.latestSuccessfulPackage &&
                 invalidPe.latestSuccessfulPackage->destination == firstDestination))
    {
        return false;
    }

    publisher.invalidPortableExecutable.store(false, std::memory_order_release);
    const std::uint32_t artifactReadsBeforeResourceLimits = publisher.artifactReadCalls.load(std::memory_order_acquire);
    publisher.oversizedRuntimePeImage.store(true, std::memory_order_release);
    if (!require(service->retry("17234567-89ab-4cde-8f01-23456789abcd", {1U, 0U, 0U}, std::string(k_projectId),
                                *runtimeData.try_value()) &&
                 service->wait_for_package()))
    {
        return false;
    }
    if (!require(service->snapshot().state == cue::package::PackageWorkflowState::Failed &&
                 publisher.artifactReadCalls.load(std::memory_order_acquire) == artifactReadsBeforeResourceLimits))
    {
        return false;
    }
    publisher.oversizedRuntimePeImage.store(false, std::memory_order_release);
    publisher.oversizedRuntimePeInventory.store(true, std::memory_order_release);
    if (!require(service->retry("18234567-89ab-4cde-8f01-23456789abcd", {1U, 0U, 0U}, std::string(k_projectId),
                                *runtimeData.try_value()) &&
                 service->wait_for_package()))
    {
        return false;
    }
    if (!require(service->snapshot().state == cue::package::PackageWorkflowState::Failed &&
                 publisher.artifactReadCalls.load(std::memory_order_acquire) == artifactReadsBeforeResourceLimits))
    {
        return false;
    }
    publisher.oversizedRuntimePeInventory.store(false, std::memory_order_release);
    publisher.corruptInventory.store(true, std::memory_order_release);
    if (!require(service->retry("21234567-89ab-4cde-8f01-23456789abcd", {1U, 0U, 0U}, std::string(k_projectId),
                                *runtimeData.try_value()) &&
                 service->wait_for_package()))
    {
        return false;
    }
    cue::package::PackageWorkflowSnapshot packageFailed = service->snapshot();
    if (!require(packageFailed.state == cue::package::PackageWorkflowState::Failed && !packageFailed.package &&
                 packageFailed.latestSuccessfulPackage &&
                 packageFailed.latestSuccessfulPackage->destination == firstDestination))
    {
        return false;
    }

    publisher.corruptInventory.store(false, std::memory_order_release);
    recoveryFilesystem.durabilityFailuresRemaining.store(1U, std::memory_order_release);
    constexpr std::string_view durabilityOperation = "23234567-89ab-4cde-8f01-23456789abcd";
    if (!require(service->retry(std::string(durabilityOperation), {1U, 0U, 0U}, std::string(k_projectId),
                                *runtimeData.try_value()) &&
                 service->wait_for_package()))
    {
        return false;
    }
    const cue::package::PackageWorkflowSnapshot durabilityUnknown = service->snapshot();
    if (!require(durabilityUnknown.state == cue::package::PackageWorkflowState::Failed && !durabilityUnknown.package &&
                 durabilityUnknown.publicationDiagnostic &&
                 durabilityUnknown.publicationDiagnostic->stage == cue::package::PackagePublishStage::Publish &&
                 durabilityUnknown.publicationDiagnostic->outcome ==
                     cue::package::PackagePublishOutcome::PublishedButDurabilityUnknown &&
                 durabilityUnknown.publicationDiagnostic->destination ==
                     std::string("Generated/Packages/Debug/") + std::string(durabilityOperation) &&
                 durabilityUnknown.publicationDiagnostic->manifest.projectId == k_projectId &&
                 durabilityUnknown.publicationDiagnostic->manifest.fileCount != 0U &&
                 durabilityUnknown.latestSuccessfulPackage &&
                 durabilityUnknown.latestSuccessfulPackage->destination == firstDestination &&
                 std::filesystem::exists(projectRoot /
                                         std::filesystem::path(durabilityUnknown.publicationDiagnostic->destination) /
                                         L"CuePackage.json")))
    {
        return false;
    }

    recoveryFilesystem.writeFailuresRemaining.store(1U, std::memory_order_release);
    recoveryFilesystem.rollbackFailuresRemaining.store(2U, std::memory_order_release);
    if (!require(service->retry("26234567-89ab-4cde-8f01-23456789abcd", {1U, 0U, 0U}, std::string(k_projectId),
                                *runtimeData.try_value()) &&
                 service->wait_for_package()))
    {
        return false;
    }
    const cue::package::PackageWorkflowSnapshot recoveryFailed = service->snapshot();
    if (!require(recoveryFailed.state == cue::package::PackageWorkflowState::Failed &&
                 recoveryFailed.recoveryStagingLocator &&
                 std::filesystem::exists(projectRoot / std::filesystem::path(*recoveryFailed.recoveryStagingLocator)) &&
                 recoveryFilesystem.rollbackCalls.load(std::memory_order_acquire) == 2U))
    {
        return false;
    }
    const std::string recoveryStaging = *recoveryFailed.recoveryStagingLocator;

    if (!require(service->retry("31234567-89ab-4cde-8f01-23456789abcd", {1U, 0U, 0U}, std::string(k_projectId),
                                *runtimeData.try_value()) &&
                 service->wait_for_package() && service->run(cue::package::PackageRunMode::SmokeTest) &&
                 service->wait_for_run_completion()))
    {
        return false;
    }
    const cue::package::PackageWorkflowSnapshot runSucceeded = service->snapshot();
    if (!require(runSucceeded.state == cue::package::PackageWorkflowState::RunSucceeded &&
                 !runSucceeded.recoveryStagingLocator &&
                 !std::filesystem::exists(projectRoot / std::filesystem::path(recoveryStaging)) &&
                 recoveryFilesystem.rollbackCalls.load(std::memory_order_acquire) == 3U &&
                 runSucceeded.runOutput.size() == 1U &&
                 runRunner.maximumCapturedOutputBytes.load(std::memory_order_acquire) == 4U * 1024U * 1024U))
    {
        return false;
    }

    runRunner.mode.store(RunnerMode::Fail, std::memory_order_release);
    if (!require(service->run(cue::package::PackageRunMode::SmokeTest) && service->wait_for_run_completion()))
    {
        return false;
    }
    const cue::package::PackageWorkflowSnapshot runFailed = service->snapshot();
    runRunner.mode.store(RunnerMode::Succeed, std::memory_order_release);
    if (!require(runFailed.state == cue::package::PackageWorkflowState::Failed && runFailed.package &&
                 service->run(cue::package::PackageRunMode::SmokeTest) && service->wait_for_run_completion() &&
                 service->snapshot().state == cue::package::PackageWorkflowState::RunSucceeded))
    {
        return false;
    }

    runRunner.mode.store(RunnerMode::BlockUntilCancelled, std::memory_order_release);
    if (!require(service->run(cue::package::PackageRunMode::Interactive) && wait_until_active(runRunner) &&
                 service->stop() && service->wait_for_run_completion()))
    {
        return false;
    }
    if (!require(service->snapshot().state == cue::package::PackageWorkflowState::PackageReady &&
                 !runRunner.active.load(std::memory_order_acquire) &&
                 runRunner.lastCancellationMode.load(std::memory_order_acquire) ==
                     cue::ChildProcessCancellationMode::Graceful))
    {
        return false;
    }

    if (!require(service->run(cue::package::PackageRunMode::Interactive) && wait_until_active(runRunner)))
    {
        return false;
    }
    service.reset();
    return require(!runRunner.active.load(std::memory_order_acquire) &&
                   runRunner.lastCancellationMode.load(std::memory_order_acquire) ==
                       cue::ChildProcessCancellationMode::Immediate);
}
} // namespace

/// @brief Build・Package・Run CoordinatorをUIなしで検証する
int main()
{
    TestFatalHandler fatalHandler;
    std::vector<std::unique_ptr<cue::LogSink>> sinks;
    cue::Logger logger(fatalHandler, std::move(sinks));
    cue::AssertContext assertContext(logger, fatalHandler);
    return test_workflow(assertContext) ? 0 : 1;
}
