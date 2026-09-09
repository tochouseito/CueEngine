#include <Cue/Build/Windows/WindowsArtifactPublisher.h>

#include <Cue/Foundation/Assert.h>
#include <Cue/Platform/Windows/WindowsProcess.h>
#include <Cue/Project/Descriptor.h>

#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
constexpr std::size_t k_hashBlockBytes = 64U * 1024U;
constexpr std::size_t k_sha256Bytes = 32U;
constexpr DWORD k_lockRetryMilliseconds = 10U;
constexpr std::string_view k_probeCompletionMarker = "CueGameModuleProbe:v1\n";

/// @brief Windows Artifact処理中の予期しない例外をFatal境界へ渡す
[[noreturn]] void terminate_artifact_exception(const cue::AssertContext &a_assertContext) noexcept
{
    a_assertContext.fatal_handler().terminate("Windows build artifact publication failed unexpectedly");
    std::abort();
}

/// @brief Windows Artifact固有の回復可能Errorを構築する
[[nodiscard]] cue::Error make_error(const cue::AssertContext &a_assertContext, cue::WindowsBuildArtifactError a_code,
                                    std::string_view a_summary) noexcept
{
    cue::ErrorCode code = cue::ErrorCode::create(a_assertContext.fatal_handler(), "Cue.Build.Windows.Artifact",
                                                 static_cast<std::int64_t>(a_code));
    return cue::Error::create(a_assertContext.fatal_handler(), std::move(code), a_summary);
}

/// @brief Platform非依存なPublisher Lock待機Timeoutを構築する
[[nodiscard]] cue::Error make_lock_timeout_error(const cue::AssertContext &a_assertContext,
                                                 std::string_view a_summary) noexcept
{
    cue::ErrorCode code =
        cue::ErrorCode::create(a_assertContext.fatal_handler(), "Cue.Build.Publisher",
                               static_cast<std::int64_t>(cue::BuildArtifactPublisherError::LockWaitTimedOut));
    return cue::Error::create(a_assertContext.fatal_handler(), std::move(code), a_summary);
}

/// @brief Game Module ProbeのTimeoutをServiceが識別できるErrorへ変換する
[[nodiscard]] cue::Error make_module_probe_timeout_error(const cue::AssertContext &a_assertContext) noexcept
{
    cue::ErrorCode code =
        cue::ErrorCode::create(a_assertContext.fatal_handler(), "Cue.Build.Publisher",
                               static_cast<std::int64_t>(cue::BuildArtifactPublisherError::ModuleProbeTimedOut));
    return cue::Error::create(a_assertContext.fatal_handler(), std::move(code), "Game Module ABI probe timed out");
}

/// @brief Win32 Codeを保持するWindows Artifact Errorを構築する
[[nodiscard]] cue::Error make_windows_error(const cue::AssertContext &a_assertContext,
                                            cue::WindowsBuildArtifactError a_code, DWORD a_nativeCode,
                                            std::string_view a_summary) noexcept
{
    cue::ErrorCode code = cue::ErrorCode::create(a_assertContext.fatal_handler(), "Cue.Build.Windows.Artifact",
                                                 static_cast<std::int64_t>(a_code));
    cue::NativeError native =
        cue::NativeError::create(a_assertContext.fatal_handler(), "Win32", static_cast<std::int64_t>(a_nativeCode));
    return cue::Error::create(a_assertContext.fatal_handler(), std::move(code), a_summary, std::move(native));
}

/// @brief NTSTATUSを保持するWindows Artifact Errorを構築する
[[nodiscard]] cue::Error make_nt_error(const cue::AssertContext &a_assertContext, cue::WindowsBuildArtifactError a_code,
                                       NTSTATUS a_nativeCode, std::string_view a_summary) noexcept
{
    cue::ErrorCode code = cue::ErrorCode::create(a_assertContext.fatal_handler(), "Cue.Build.Windows.Artifact",
                                                 static_cast<std::int64_t>(a_code));
    cue::NativeError native =
        cue::NativeError::create(a_assertContext.fatal_handler(), "NTSTATUS", static_cast<std::int64_t>(a_nativeCode));
    return cue::Error::create(a_assertContext.fatal_handler(), std::move(code), a_summary, std::move(native));
}

/// @brief HANDLEを一意所有し全経路でCloseする
class UniqueHandle final
{
  public:
    /// @brief 無効Handleとして構築する
    UniqueHandle() noexcept = default;
    /// @brief Native Handleの一意所有権を取得する
    explicit UniqueHandle(HANDLE a_handle) noexcept : m_handle(a_handle)
    {
    }
    /// @brief Native Handle所有権のCopy構築を禁止する
    UniqueHandle(const UniqueHandle &) = delete;
    /// @brief Native Handle所有権のCopy代入を禁止する
    UniqueHandle &operator=(const UniqueHandle &) = delete;
    /// @brief Native Handleを移動する
    UniqueHandle(UniqueHandle &&a_other) noexcept : m_handle(std::exchange(a_other.m_handle, INVALID_HANDLE_VALUE))
    {
    }
    /// @brief 現在のHandleをCloseしてNative Handleを移動する
    UniqueHandle &operator=(UniqueHandle &&a_other) noexcept
    {
        if (this != &a_other)
        {
            reset();
            m_handle = std::exchange(a_other.m_handle, INVALID_HANDLE_VALUE);
        }
        return *this;
    }
    /// @brief 所有Native HandleをCloseする
    ~UniqueHandle()
    {
        reset();
    }
    /// @brief Native API呼出し用Handleを返す
    [[nodiscard]] HANDLE get() const noexcept
    {
        return m_handle;
    }
    /// @brief 有効なNative Handleを所有しているか返す
    [[nodiscard]] bool is_valid() const noexcept
    {
        return m_handle != nullptr && m_handle != INVALID_HANDLE_VALUE;
    }

  private:
    /// @brief 所有HandleがあればCloseして無効化する
    void reset() noexcept
    {
        if (is_valid())
        {
            CloseHandle(m_handle);
        }
        m_handle = INVALID_HANDLE_VALUE;
    }

    HANDLE m_handle = INVALID_HANDLE_VALUE;
};

/// @brief Offset 0、Length 1のWindows File LockをHandle寿命へ束ねる
class ByteRangeLease
{
  public:
    /// @brief Lock済みHandleの一意所有権を取得する
    explicit ByteRangeLease(UniqueHandle a_handle) noexcept : m_handle(std::move(a_handle))
    {
    }
    /// @brief Byte Range Lock所有権のCopy構築を禁止する
    ByteRangeLease(const ByteRangeLease &) = delete;
    /// @brief Byte Range Lock所有権のCopy代入を禁止する
    ByteRangeLease &operator=(const ByteRangeLease &) = delete;
    /// @brief Byte RangeをUnlockしてHandleをCloseする
    virtual ~ByteRangeLease()
    {
        if (m_handle.is_valid())
        {
            OVERLAPPED overlap{};
            static_cast<void>(UnlockFileEx(m_handle.get(), 0U, 1U, 0U, &overlap));
        }
    }

  private:
    UniqueHandle m_handle;
};

/// @brief GameBuildServiceへ渡すWindows Build Workspace Lease
class WindowsBuildWorkspaceLease final : public cue::BuildWorkspaceLease, public ByteRangeLease
{
  public:
    /// @brief Lock済みBuild FileとPlan Keyを所有する
    WindowsBuildWorkspaceLease(UniqueHandle a_handle, std::string a_workspaceKey) noexcept
        : ByteRangeLease(std::move(a_handle)), m_workspaceKey(std::move(a_workspaceKey))
    {
    }
    /// @brief Build Lockを解放する
    ~WindowsBuildWorkspaceLease() override = default;
    /// @brief Leaseが保護するPlan Keyと一致するか返す
    [[nodiscard]] bool matches(std::string_view a_workspaceKey) const noexcept
    {
        return m_workspaceKey == a_workspaceKey;
    }

  private:
    std::string m_workspaceKey;
};

/// @brief UTF-8 PathをWindows Filesystem Pathへ変換する
[[nodiscard]] std::optional<std::filesystem::path> to_path(std::string_view a_path)
{
    try
    {
        return std::filesystem::path(
            std::u8string_view(reinterpret_cast<const char8_t *>(a_path.data()), a_path.size()));
    }
    catch (const std::filesystem::filesystem_error &)
    {
        return std::nullopt;
    }
}

/// @brief Project RootとPlan Rootが同じWindows Pathを表すか比較する
[[nodiscard]] bool same_root(const std::filesystem::path &a_left, std::string_view a_right)
{
    const std::optional<std::filesystem::path> right = to_path(a_right);
    if (!right)
    {
        return false;
    }
    std::error_code leftError;
    std::error_code rightError;
    const std::filesystem::path leftCanonical = std::filesystem::weakly_canonical(a_left, leftError);
    const std::filesystem::path rightCanonical = std::filesystem::weakly_canonical(*right, rightError);
    return !leftError && !rightError && _wcsicmp(leftCanonical.c_str(), rightCanonical.c_str()) == 0;
}

/// @brief Build Configurationを安定したDirectory／Manifest名へ変換する
[[nodiscard]] std::string_view configuration_name(cue::BuildConfiguration a_configuration) noexcept
{
    switch (a_configuration)
    {
    case cue::BuildConfiguration::Debug:
        return "Debug";
    case cue::BuildConfiguration::Development:
        return "Development";
    case cue::BuildConfiguration::Release:
        return "Release";
    }
    return {};
}

/// @brief Filesystem PathをEngine内部のUTF-8表示へ変換する
[[nodiscard]] std::string path_to_utf8(const std::filesystem::path &a_path)
{
    const std::u8string text = a_path.generic_u8string();
    return std::string(reinterpret_cast<const char *>(text.data()), text.size());
}

/// @brief 現在のEngine Processと同じDirectoryを返す
[[nodiscard]] std::optional<std::filesystem::path> current_process_directory()
{
    std::vector<wchar_t> path(32768U, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0U || length >= path.size())
    {
        return std::nullopt;
    }
    return std::filesystem::path(std::wstring_view(path.data(), length)).parent_path();
}

/// @brief Engine VersionをMetadata用major.minor.patchへ変換する
[[nodiscard]] std::string version_text(const cue::EngineVersion &a_version)
{
    return std::to_string(a_version.major) + "." + std::to_string(a_version.minor) + "." +
           std::to_string(a_version.patch);
}

/// @brief Directoryを不足分だけ作成し回復可能なFilesystem Errorへ変換する
[[nodiscard]] cue::Result<void> ensure_directory(const std::filesystem::path &a_directory,
                                                 cue::WindowsBuildArtifactError a_code,
                                                 const cue::AssertContext &a_assertContext) noexcept
{
    std::error_code error;
    if (std::filesystem::create_directories(a_directory, error))
    {
        return cue::Result<void>::success();
    }
    std::error_code statusError;
    if (!error && std::filesystem::is_directory(a_directory, statusError) && !statusError)
    {
        return cue::Result<void>::success();
    }
    const DWORD nativeCode =
        static_cast<DWORD>(error ? error.value() : (statusError ? statusError.value() : ERROR_DIRECTORY));
    return cue::Result<void>::failure(
        make_windows_error(a_assertContext, a_code, nativeCode, "Artifact directory could not be created"));
}

/// @brief Lock Fileを作成して取消可能なExclusive Byte Range Lockを取得する
[[nodiscard]] cue::Result<std::optional<UniqueHandle>> acquire_exclusive_lock(
    const std::filesystem::path &a_path, const cue::ChildProcessCancellation &a_cancellation,
    cue::BuildArtifactLockDeadline a_deadline, cue::WindowsBuildArtifactError a_code,
    const cue::AssertContext &a_assertContext) noexcept
{
    cue::Result<void> parent = ensure_directory(a_path.parent_path(), a_code, a_assertContext);
    if (!parent)
    {
        return cue::Result<std::optional<UniqueHandle>>::failure(std::move(*parent.try_error()));
    }
    UniqueHandle handle(CreateFileW(a_path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!handle.is_valid())
    {
        return cue::Result<std::optional<UniqueHandle>>::failure(
            make_windows_error(a_assertContext, a_code, GetLastError(), "Artifact lock file could not be opened"));
    }
    while (!a_cancellation.is_cancel_requested())
    {
        if (a_deadline && std::chrono::steady_clock::now() >= *a_deadline)
        {
            return cue::Result<std::optional<UniqueHandle>>::failure(
                make_lock_timeout_error(a_assertContext, "Artifact byte-range lock wait timed out"));
        }
        OVERLAPPED overlap{};
        if (LockFileEx(handle.get(), LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0U, 1U, 0U, &overlap) !=
            FALSE)
        {
            return cue::Result<std::optional<UniqueHandle>>::success(std::optional<UniqueHandle>(std::move(handle)));
        }
        const DWORD code = GetLastError();
        if (code != ERROR_LOCK_VIOLATION && code != ERROR_IO_PENDING)
        {
            return cue::Result<std::optional<UniqueHandle>>::failure(
                make_windows_error(a_assertContext, a_code, code, "Artifact byte-range lock could not be acquired"));
        }
        DWORD retryMilliseconds = k_lockRetryMilliseconds;
        if (a_deadline)
        {
            const auto now = std::chrono::steady_clock::now();
            if (now >= *a_deadline)
            {
                return cue::Result<std::optional<UniqueHandle>>::failure(
                    make_lock_timeout_error(a_assertContext, "Artifact byte-range lock wait timed out"));
            }
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(*a_deadline - now);
            retryMilliseconds = static_cast<DWORD>(
                std::clamp<std::int64_t>(remaining.count(), 1, static_cast<std::int64_t>(k_lockRetryMilliseconds)));
        }
        Sleep(retryMilliseconds);
    }
    return cue::Result<std::optional<UniqueHandle>>::success(std::nullopt);
}

/// @brief Regular FileをSHA-256でStreaming HashしSizeとDigestを返す
[[nodiscard]] cue::Result<cue::BuildArtifactFile> hash_file(const std::filesystem::path &a_path,
                                                            std::string a_relativePath,
                                                            const cue::AssertContext &a_assertContext) noexcept
{
    std::error_code statusError;
    const std::filesystem::file_status status = std::filesystem::symlink_status(a_path, statusError);
    if (statusError || !std::filesystem::is_regular_file(status))
    {
        return cue::Result<cue::BuildArtifactFile>::failure(make_error(
            a_assertContext, cue::WindowsBuildArtifactError::CandidateInvalid, "Artifact entry is not a regular file"));
    }
    std::error_code sizeError;
    const std::uintmax_t byteSize = std::filesystem::file_size(a_path, sizeError);
    if (sizeError || byteSize > 9007199254740991ULL)
    {
        return cue::Result<cue::BuildArtifactFile>::failure(make_error(
            a_assertContext, cue::WindowsBuildArtifactError::CandidateInvalid, "Artifact entry size is invalid"));
    }

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    NTSTATUS statusCode = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0U);
    if (statusCode < 0)
    {
        return cue::Result<cue::BuildArtifactFile>::failure(make_nt_error(
            a_assertContext, cue::WindowsBuildArtifactError::CandidateInvalid, statusCode, "SHA-256 provider failed"));
    }
    /// @brief BCrypt Algorithm Providerを全経路でCloseする
    const auto closeAlgorithm = [](BCRYPT_ALG_HANDLE *a_algorithm) noexcept
    {
        if (*a_algorithm != nullptr)
        {
            BCryptCloseAlgorithmProvider(*a_algorithm, 0U);
        }
    };
    std::unique_ptr<BCRYPT_ALG_HANDLE, decltype(closeAlgorithm)> algorithmOwner(&algorithm, closeAlgorithm);

    DWORD objectBytes = 0U;
    DWORD copied = 0U;
    statusCode = BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectBytes),
                                   sizeof(objectBytes), &copied, 0U);
    if (statusCode < 0 || copied != sizeof(objectBytes))
    {
        return cue::Result<cue::BuildArtifactFile>::failure(
            make_nt_error(a_assertContext, cue::WindowsBuildArtifactError::CandidateInvalid, statusCode,
                          "SHA-256 state query failed"));
    }
    std::vector<std::uint8_t> object(objectBytes);
    BCRYPT_HASH_HANDLE hash = nullptr;
    statusCode = BCryptCreateHash(algorithm, &hash, object.data(), objectBytes, nullptr, 0U, 0U);
    if (statusCode < 0)
    {
        return cue::Result<cue::BuildArtifactFile>::failure(
            make_nt_error(a_assertContext, cue::WindowsBuildArtifactError::CandidateInvalid, statusCode,
                          "SHA-256 state creation failed"));
    }
    /// @brief BCrypt Hash Stateを全経路で破棄する
    const auto destroyHash = [](BCRYPT_HASH_HANDLE *a_hash) noexcept
    {
        if (*a_hash != nullptr)
        {
            BCryptDestroyHash(*a_hash);
        }
    };
    std::unique_ptr<BCRYPT_HASH_HANDLE, decltype(destroyHash)> hashOwner(&hash, destroyHash);

    std::ifstream input(a_path, std::ios::binary);
    if (!input)
    {
        return cue::Result<cue::BuildArtifactFile>::failure(make_error(
            a_assertContext, cue::WindowsBuildArtifactError::CandidateInvalid, "Artifact entry could not be opened"));
    }
    std::array<std::uint8_t, k_hashBlockBytes> block{};
    while (input)
    {
        input.read(reinterpret_cast<char *>(block.data()), static_cast<std::streamsize>(block.size()));
        const std::streamsize count = input.gcount();
        if (count > 0)
        {
            statusCode = BCryptHashData(hash, block.data(), static_cast<ULONG>(count), 0U);
            if (statusCode < 0)
            {
                return cue::Result<cue::BuildArtifactFile>::failure(
                    make_nt_error(a_assertContext, cue::WindowsBuildArtifactError::CandidateInvalid, statusCode,
                                  "SHA-256 update failed"));
            }
        }
    }
    if (!input.eof())
    {
        return cue::Result<cue::BuildArtifactFile>::failure(make_error(
            a_assertContext, cue::WindowsBuildArtifactError::CandidateInvalid, "Artifact entry could not be read"));
    }
    std::array<std::uint8_t, k_sha256Bytes> digest{};
    statusCode = BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0U);
    if (statusCode < 0)
    {
        return cue::Result<cue::BuildArtifactFile>::failure(
            make_nt_error(a_assertContext, cue::WindowsBuildArtifactError::CandidateInvalid, statusCode,
                          "SHA-256 finalization failed"));
    }
    constexpr std::string_view digits = "0123456789abcdef";
    std::string hashText;
    hashText.reserve(digest.size() * 2U);
    for (const std::uint8_t value : digest)
    {
        hashText.push_back(digits[value >> 4U]);
        hashText.push_back(digits[value & 0x0fU]);
    }
    return cue::Result<cue::BuildArtifactFile>::success(
        {std::move(a_relativePath), static_cast<std::uint64_t>(byteSize), std::move(hashText)});
}

/// @brief 可視なCurrentが参照するVersion全FileをInventoryのSizeとHashへ再照合する
[[nodiscard]] cue::Result<void> verify_inventory_files(const std::filesystem::path &a_version,
                                                       const cue::BuildArtifactInventory &a_inventory,
                                                       const cue::AssertContext &a_assertContext) noexcept
{
    for (const cue::BuildArtifactFile &expected : a_inventory.files())
    {
        const std::optional<std::filesystem::path> relative = to_path(expected.relativePath);
        if (!relative)
        {
            return cue::Result<void>::failure(
                make_error(a_assertContext, cue::WindowsBuildArtifactError::CurrentManifestFailed,
                           "Visible Current artifact contains an invalid inventory path"));
        }
        cue::Result<cue::BuildArtifactFile> actual =
            hash_file(a_version / *relative, expected.relativePath, a_assertContext);
        if (!actual)
        {
            return cue::Result<void>::failure(std::move(*actual.try_error()));
        }
        if (actual.try_value()->byteSize != expected.byteSize ||
            actual.try_value()->contentHash != expected.contentHash)
        {
            return cue::Result<void>::failure(make_error(a_assertContext,
                                                         cue::WindowsBuildArtifactError::CurrentManifestFailed,
                                                         "Visible Current artifact differs from its inventory"));
        }
    }
    return cue::Result<void>::success();
}

/// @brief Current更新Errorが可視化後の耐久性不明を表すか判定する
[[nodiscard]] bool is_current_durability_unknown(const cue::Error &a_error) noexcept
{
    const cue::ErrorCode &root = a_error.root_code();
    return root.domain() == "Cue.Build.Windows.Artifact" &&
           root.value() == static_cast<std::int64_t>(cue::WindowsBuildArtifactError::CurrentManifestDurabilityUnknown);
}

/// @brief Byte列を新規Fileへ書きFlushする
[[nodiscard]] cue::Result<void> write_new_file(const std::filesystem::path &a_path, std::string_view a_bytes,
                                               cue::WindowsBuildArtifactError a_code,
                                               const cue::AssertContext &a_assertContext) noexcept
{
    std::optional<cue::Error> writeError;
    {
        UniqueHandle file(
            CreateFileW(a_path.c_str(), GENERIC_WRITE, 0U, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!file.is_valid())
        {
            return cue::Result<void>::failure(
                make_windows_error(a_assertContext, a_code, GetLastError(), "Artifact file could not be created"));
        }
        std::size_t offset = 0U;
        while (offset < a_bytes.size())
        {
            const DWORD request = static_cast<DWORD>(std::min<std::size_t>(a_bytes.size() - offset, MAXDWORD));
            DWORD written = 0U;
            const BOOL succeeded = WriteFile(file.get(), a_bytes.data() + offset, request, &written, nullptr);
            if (succeeded == FALSE || written == 0U)
            {
                const DWORD code = succeeded == FALSE ? GetLastError() : ERROR_WRITE_FAULT;
                writeError.emplace(make_windows_error(a_assertContext, a_code, code, "Artifact file write failed"));
                break;
            }
            offset += written;
        }
        if (!writeError.has_value() && FlushFileBuffers(file.get()) == FALSE)
        {
            writeError.emplace(
                make_windows_error(a_assertContext, a_code, GetLastError(), "Artifact file flush failed"));
        }
    }
    if (!writeError.has_value())
    {
        return cue::Result<void>::success();
    }
    if (DeleteFileW(a_path.c_str()) == FALSE)
    {
        const DWORD cleanupCode = GetLastError();
        if (cleanupCode != ERROR_FILE_NOT_FOUND && cleanupCode != ERROR_PATH_NOT_FOUND)
        {
            cue::Error cleanupError =
                make_windows_error(a_assertContext, a_code, cleanupCode, "Incomplete artifact file rollback failed");
            writeError->append_secondary_diagnostics(a_assertContext, cleanupError,
                                                     "Incomplete artifact file could not be removed", "Rollback");
        }
    }
    return cue::Result<void>::failure(std::move(*writeError));
}

/// @brief Build出力を新規Candidate FileへCopyし、File内容をFlushしてから返す
[[nodiscard]] cue::Result<void> copy_new_file_durable(const std::filesystem::path &a_source,
                                                      const std::filesystem::path &a_destination,
                                                      const cue::AssertContext &a_assertContext) noexcept
{
    UniqueHandle source(CreateFileW(a_source.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!source.is_valid())
    {
        return cue::Result<void>::failure(
            make_windows_error(a_assertContext, cue::WindowsBuildArtifactError::CandidateInvalid, GetLastError(),
                               "Build artifact source could not be opened"));
    }
    UniqueHandle destination(CreateFileW(a_destination.c_str(), GENERIC_WRITE, 0U, nullptr, CREATE_NEW,
                                         FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!destination.is_valid())
    {
        return cue::Result<void>::failure(
            make_windows_error(a_assertContext, cue::WindowsBuildArtifactError::CandidateInvalid, GetLastError(),
                               "Build artifact candidate could not be created"));
    }

    std::array<std::byte, 64U * 1024U> buffer{};
    for (;;)
    {
        DWORD read = 0U;
        if (ReadFile(source.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) == FALSE)
        {
            return cue::Result<void>::failure(make_windows_error(a_assertContext,
                                                                 cue::WindowsBuildArtifactError::CandidateInvalid,
                                                                 GetLastError(), "Build artifact source read failed"));
        }
        if (read == 0U)
        {
            break;
        }
        DWORD offset = 0U;
        while (offset < read)
        {
            DWORD written = 0U;
            const BOOL succeeded =
                WriteFile(destination.get(), buffer.data() + offset, read - offset, &written, nullptr);
            if (succeeded == FALSE || written == 0U)
            {
                const DWORD code = succeeded == FALSE ? GetLastError() : ERROR_WRITE_FAULT;
                return cue::Result<void>::failure(make_windows_error(a_assertContext,
                                                                     cue::WindowsBuildArtifactError::CandidateInvalid,
                                                                     code, "Build artifact candidate write failed"));
            }
            offset += written;
        }
    }
    if (FlushFileBuffers(destination.get()) == FALSE)
    {
        return cue::Result<void>::failure(make_windows_error(a_assertContext,
                                                             cue::WindowsBuildArtifactError::CandidateInvalid,
                                                             GetLastError(), "Build artifact candidate flush failed"));
    }
    return cue::Result<void>::success();
}

/// @brief 別ProcessでのGame Module検証完了種別
enum class ModuleProbeStatus : std::uint8_t
{
    Valid,
    Cancelled
};

/// @brief 絶対DeadlineをChild Process Runner用の残り時間へ変換する
[[nodiscard]] std::optional<std::chrono::milliseconds> remaining_timeout(
    cue::BuildArtifactLockDeadline a_deadline) noexcept
{
    if (!a_deadline)
    {
        return std::nullopt;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now >= *a_deadline)
    {
        return std::chrono::milliseconds(0);
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(*a_deadline - now);
    return std::max(remaining, std::chrono::milliseconds(1));
}

/// @brief 前回Probeの完了Markerを除去し新しい検証との混同を防ぐ
[[nodiscard]] cue::Result<void> remove_probe_marker(const std::filesystem::path &a_path,
                                                    const cue::AssertContext &a_assertContext) noexcept
{
    std::error_code error;
    static_cast<void>(std::filesystem::remove(a_path, error));
    if (error)
    {
        return cue::Result<void>::failure(make_windows_error(
            a_assertContext, cue::WindowsBuildArtifactError::ModuleContractMismatch,
            static_cast<DWORD>(error.value()), "Game Module ABI probe completion marker could not be removed"));
    }
    return cue::Result<void>::success();
}

/// @brief Probeが全検証後に作成した固定内容のRegular Fileだけを完了通知として認める
[[nodiscard]] bool probe_marker_matches(const std::filesystem::path &a_path) noexcept
{
    std::error_code statusError;
    const std::filesystem::file_status status = std::filesystem::symlink_status(a_path, statusError);
    if (statusError || !std::filesystem::is_regular_file(status))
    {
        return false;
    }
    std::error_code sizeError;
    if (std::filesystem::file_size(a_path, sizeError) != k_probeCompletionMarker.size() || sizeError)
    {
        return false;
    }
    std::ifstream input(a_path, std::ios::binary);
    std::array<char, k_probeCompletionMarker.size()> bytes{};
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return input && std::string_view(bytes.data(), bytes.size()) == k_probeCompletionMarker;
}

/// @brief Candidate DLLの公開ABIを取消／Timeout可能な別Processで検証する
[[nodiscard]] cue::Result<ModuleProbeStatus> validate_module(const std::filesystem::path &a_path,
                                                             const cue::BuildPlan &a_plan, std::string_view a_projectId,
                                                             const std::filesystem::path &a_markerDirectory,
                                                             std::string_view a_probeExecutable,
                                                             cue::ChildProcessRunner &a_processRunner,
                                                             const cue::ChildProcessCancellation &a_cancellation,
                                                             cue::BuildArtifactLockDeadline a_deadline,
                                                             const cue::AssertContext &a_assertContext) noexcept
{
    const std::optional<std::chrono::milliseconds> timeout = remaining_timeout(a_deadline);
    if (timeout && timeout->count() <= 0)
    {
        return cue::Result<ModuleProbeStatus>::failure(make_module_probe_timeout_error(a_assertContext));
    }
    const std::filesystem::path marker = a_markerDirectory / ".probe-complete";
    cue::Result<void> staleMarkerRemoved = remove_probe_marker(marker, a_assertContext);
    if (!staleMarkerRemoved)
    {
        return cue::Result<ModuleProbeStatus>::failure(std::move(*staleMarkerRemoved.try_error()));
    }
    /// @brief Probe失敗へMarker Cleanup診断を追加して返す
    const auto failProbe = [&](cue::Error a_error) -> cue::Result<ModuleProbeStatus>
    {
        cue::Result<void> markerRemoved = remove_probe_marker(marker, a_assertContext);
        if (!markerRemoved)
        {
            a_error.append_secondary_diagnostics(a_assertContext, *markerRemoved.try_error(),
                                                 "Probe completion marker cleanup failed", "Probe cleanup");
        }
        return cue::Result<ModuleProbeStatus>::failure(std::move(a_error));
    };
    cue::ChildProcessRequest request(std::string(a_probeExecutable),
                                     {path_to_utf8(a_path),
                                      std::string(configuration_name(a_plan.profile().configuration())),
                                      std::string(a_projectId), path_to_utf8(marker)},
                                     path_to_utf8(a_path.parent_path()), {}, timeout, 0U);
    auto process = a_processRunner.run(request, a_cancellation);
    if (!process)
    {
        return failProbe(std::move(*process.try_error()));
    }
    switch (process.try_value()->outcome())
    {
    case cue::ChildProcessOutcome::Cancelled:
    {
        cue::Result<void> markerRemoved = remove_probe_marker(marker, a_assertContext);
        if (!markerRemoved)
        {
            return cue::Result<ModuleProbeStatus>::failure(std::move(*markerRemoved.try_error()));
        }
        return cue::Result<ModuleProbeStatus>::success(ModuleProbeStatus::Cancelled);
    }
    case cue::ChildProcessOutcome::TimedOut:
        return failProbe(make_module_probe_timeout_error(a_assertContext));
    case cue::ChildProcessOutcome::Exited:
        break;
    }
    if (!process.try_value()->exit_code() || *process.try_value()->exit_code() != 0U)
    {
        std::string summary("Game Module ABI probe rejected the candidate");
        if (process.try_value()->exit_code())
        {
            summary.append(" with exit code ");
            summary.append(std::to_string(*process.try_value()->exit_code()));
        }
        return failProbe(
            make_error(a_assertContext, cue::WindowsBuildArtifactError::ModuleContractMismatch, summary));
    }
    const bool isComplete = probe_marker_matches(marker);
    cue::Result<void> markerRemoved = remove_probe_marker(marker, a_assertContext);
    if (!isComplete)
    {
        cue::Error error = make_error(a_assertContext, cue::WindowsBuildArtifactError::ModuleContractMismatch,
                                      "Game Module ABI probe exited without a valid completion marker");
        if (!markerRemoved)
        {
            error.append_secondary_diagnostics(a_assertContext, *markerRemoved.try_error(),
                                               "Probe completion marker cleanup failed", "Probe cleanup");
        }
        return cue::Result<ModuleProbeStatus>::failure(std::move(error));
    }
    if (!markerRemoved)
    {
        return cue::Result<ModuleProbeStatus>::failure(std::move(*markerRemoved.try_error()));
    }
    return cue::Result<ModuleProbeStatus>::success(ModuleProbeStatus::Valid);
}

/// @brief Metadata v1を決定的なUTF-8 JSONへ直列化する
[[nodiscard]] std::string serialize_metadata(std::string_view a_artifactId, std::string_view a_projectId,
                                             const cue::EngineCompatibility &a_compatibility,
                                             cue::BuildConfiguration a_configuration,
                                             const cue::BuildToolVersion &a_toolsetVersion)
{
    const std::uint64_t compilerVersion =
        static_cast<std::uint64_t>(a_toolsetVersion.major) * 100U + static_cast<std::uint64_t>(a_toolsetVersion.minor);
    const std::uint64_t fullVersion = compilerVersion * 100000U + static_cast<std::uint64_t>(a_toolsetVersion.patch);
    std::string output;
    output.reserve(768U);
    output.append("{\n    \"schemaVersion\": 1,\n    \"artifactId\": \"");
    output.append(a_artifactId);
    output.append("\",\n    \"projectId\": \"");
    output.append(a_projectId);
    output.append("\",\n    \"engineCompatibility\": {\n        \"minimum\": \"");
    output.append(version_text(a_compatibility.minimum));
    output.append("\",\n        \"maximumExclusive\": ");
    if (a_compatibility.maximumExclusive)
    {
        output.push_back('"');
        output.append(version_text(*a_compatibility.maximumExclusive));
        output.push_back('"');
    }
    else
    {
        output.append("null");
    }
    output.append("\n    },\n    \"abiVersion\": 1,\n    \"configuration\": \"");
    output.append(configuration_name(a_configuration));
    output.append("\",\n    \"architecture\": \"x64\",\n    \"compilerFamily\": \"msvc\",\n"
                  "    \"msvcToolset\": {\n        \"compilerVersion\": ");
    output.append(std::to_string(compilerVersion));
    output.append(",\n        \"fullVersion\": ");
    output.append(std::to_string(fullVersion));
    output.append(",\n        \"build\": ");
    output.append(std::to_string(a_toolsetVersion.build));
    output.append("\n    },\n    \"runtimeLibrary\": \"");
    output.append(a_configuration == cue::BuildConfiguration::Debug ? "DebugDll" : "Dll");
    output.append("\",\n    \"iteratorDebugLevel\": ");
    output.append(a_configuration == cue::BuildConfiguration::Debug ? "2" : "0");
    output.append(",\n    \"moduleFile\": \"CueGameModule.dll\",\n"
                  "    \"entrySymbol\": \"cue_game_module_query\"\n}\n");
    return output;
}

/// @brief Current Manifest v1をArtifact Inventoryから決定的に直列化する
[[nodiscard]] std::string serialize_current(const cue::BuildArtifactInventory &a_inventory)
{
    std::string output;
    output.reserve(512U);
    output.append("{\n    \"schemaVersion\": 1,\n    \"artifactId\": \"");
    output.append(a_inventory.artifact_id());
    output.append("\",\n    \"configuration\": \"");
    output.append(configuration_name(a_inventory.configuration()));
    output.append("\",\n    \"files\": [\n");
    for (std::size_t index = 0U; index < a_inventory.files().size(); ++index)
    {
        const cue::BuildArtifactFile &file = a_inventory.files()[index];
        output.append("        {\n            \"path\": \"");
        output.append(file.relativePath);
        output.append("\",\n            \"sizeBytes\": ");
        output.append(std::to_string(file.byteSize));
        output.append(",\n            \"hashAlgorithm\": \"sha256\",\n            \"contentHash\": \"");
        output.append(file.contentHash);
        output.append("\"\n        }");
        output.append(index + 1U == a_inventory.files().size() ? "\n" : ",\n");
    }
    output.append("    ]\n}\n");
    return output;
}

/// @brief Current.jsonをSibling Temporary FileからAtomic Replaceする
[[nodiscard]] cue::Result<void> publish_current(const std::filesystem::path &a_store, std::string_view a_operationId,
                                                std::string_view a_content,
                                                const cue::AssertContext &a_assertContext) noexcept
{
    const std::filesystem::path temporary = a_store / (".Current-" + std::string(a_operationId) + ".tmp");
    const std::filesystem::path current = a_store / "Current.json";
    cue::Result<void> written =
        write_new_file(temporary, a_content, cue::WindowsBuildArtifactError::CurrentManifestFailed, a_assertContext);
    if (!written)
    {
        return written;
    }
    if (MoveFileExW(temporary.c_str(), current.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE)
    {
        const DWORD code = GetLastError();
        std::ifstream visible(current, std::ios::binary);
        const std::string visibleBytes{std::istreambuf_iterator<char>(visible), std::istreambuf_iterator<char>()};
        const bool newManifestVisible = visible.is_open() && !visible.bad() && visibleBytes == a_content;
        std::optional<cue::Error> cleanupError;
        if (DeleteFileW(temporary.c_str()) == FALSE)
        {
            const DWORD cleanupCode = GetLastError();
            if (cleanupCode != ERROR_FILE_NOT_FOUND && cleanupCode != ERROR_PATH_NOT_FOUND)
            {
                cleanupError.emplace(
                    make_windows_error(a_assertContext, cue::WindowsBuildArtifactError::CurrentManifestFailed,
                                       cleanupCode, "Current artifact temporary manifest rollback failed"));
            }
        }
        cue::Error publicationError =
            newManifestVisible
                ? make_windows_error(a_assertContext, cue::WindowsBuildArtifactError::CurrentManifestDurabilityUnknown,
                                     code, "Current artifact manifest is visible but durability is unknown")
                : make_windows_error(a_assertContext, cue::WindowsBuildArtifactError::CurrentManifestFailed, code,
                                     "Current artifact manifest was not published");
        if (cleanupError)
        {
            publicationError.append_secondary_diagnostics(
                a_assertContext, *cleanupError, "Current temporary manifest could not be removed", "Rollback");
        }
        return cue::Result<void>::failure(std::move(publicationError));
    }
    return cue::Result<void>::success();
}

/// @brief Windows上でBuild CandidateとArtifact Storeを公開する
class WindowsBuildArtifactPublisher final : public cue::BuildArtifactPublisher
{
  public:
    /// @brief 検証済みProject契約をPublisher全寿命へ所有する
    WindowsBuildArtifactPublisher(std::filesystem::path a_projectRoot, std::string a_projectId,
                                  cue::EngineCompatibility a_compatibility, std::string a_probeExecutable,
                                  std::unique_ptr<cue::ChildProcessRunner> a_processRunner,
                                  const cue::AssertContext &a_assertContext) noexcept
        : m_projectRoot(std::move(a_projectRoot)), m_projectId(std::move(a_projectId)),
          m_compatibility(std::move(a_compatibility)), m_probeExecutable(std::move(a_probeExecutable)),
          m_processRunner(std::move(a_processRunner)), m_assertContext(&a_assertContext)
    {
    }
    /// @brief 所有Project契約を解放する
    ~WindowsBuildArtifactPublisher() override = default;

    /// @brief Plan固有Binary TreeのExclusive Byte Range Lockを取消可能に取得する
    [[nodiscard]] cue::Result<std::optional<std::unique_ptr<cue::BuildWorkspaceLease>>> acquire_build_lease(
        const cue::BuildPlan &a_plan, const cue::ChildProcessCancellation &a_cancellation,
        cue::BuildArtifactLockDeadline a_deadline) noexcept override
    {
        try
        {
            if (!m_isAvailable)
            {
                return cue::Result<std::optional<std::unique_ptr<cue::BuildWorkspaceLease>>>::failure(
                    make_error(*m_assertContext, cue::WindowsBuildArtifactError::PublisherUnavailable,
                               "Artifact Publisher is unavailable after an unknown Current selection"));
            }
            if (!same_root(m_projectRoot, a_plan.project_root()))
            {
                return cue::Result<std::optional<std::unique_ptr<cue::BuildWorkspaceLease>>>::failure(
                    make_error(*m_assertContext, cue::WindowsBuildArtifactError::InvalidSettings,
                               "Build Plan belongs to another Project Root"));
            }
            const std::filesystem::path lock =
                m_projectRoot / "Generated" / "Build" / "Locks" / (std::string(a_plan.workspace_key()) + ".lock");
            auto acquired =
                acquire_exclusive_lock(lock, a_cancellation, a_deadline,
                                       cue::WindowsBuildArtifactError::WorkspaceLockFailed, *m_assertContext);
            if (!acquired)
            {
                return cue::Result<std::optional<std::unique_ptr<cue::BuildWorkspaceLease>>>::failure(
                    std::move(*acquired.try_error()));
            }
            if (!acquired.try_value()->has_value())
            {
                return cue::Result<std::optional<std::unique_ptr<cue::BuildWorkspaceLease>>>::success(std::nullopt);
            }
            std::unique_ptr<cue::BuildWorkspaceLease> lease = std::make_unique<WindowsBuildWorkspaceLease>(
                std::move(**acquired.try_value()), std::string(a_plan.workspace_key()));
            return cue::Result<std::optional<std::unique_ptr<cue::BuildWorkspaceLease>>>::success(
                std::optional<std::unique_ptr<cue::BuildWorkspaceLease>>(std::move(lease)));
        }
        catch (...)
        {
            terminate_artifact_exception(*m_assertContext);
        }
    }

    /// @brief Build出力をCandidateへ確定してから不変VersionとCurrent Manifestを公開する
    [[nodiscard]] cue::Result<std::optional<cue::BuildArtifactInventory>> publish(
        const cue::BuildPlan &a_plan, const cue::ChildProcessCancellation &a_cancellation,
        std::unique_ptr<cue::BuildWorkspaceLease> a_buildLease,
        cue::BuildArtifactLockDeadline a_deadline) noexcept override
    {
        try
        {
            if (!m_isAvailable)
            {
                return cue::Result<std::optional<cue::BuildArtifactInventory>>::failure(
                    make_error(*m_assertContext, cue::WindowsBuildArtifactError::PublisherUnavailable,
                               "Artifact Publisher is unavailable after an unknown Current selection"));
            }
            auto *windowsLease = dynamic_cast<WindowsBuildWorkspaceLease *>(a_buildLease.get());
            if (windowsLease == nullptr || !windowsLease->matches(a_plan.workspace_key()) ||
                !same_root(m_projectRoot, a_plan.project_root()))
            {
                return cue::Result<std::optional<cue::BuildArtifactInventory>>::failure(
                    make_error(*m_assertContext, cue::WindowsBuildArtifactError::InvalidSettings,
                               "Build Workspace Lease does not match the Build Plan"));
            }
            const std::string configuration(configuration_name(a_plan.profile().configuration()));
            const std::optional<std::filesystem::path> binary = to_path(a_plan.binary_directory());
            const std::optional<std::filesystem::path> candidatePath = to_path(a_plan.candidate_directory());
            const std::optional<std::filesystem::path> storePath = to_path(a_plan.artifact_store_directory());
            if (!binary || !candidatePath || !storePath)
            {
                return cue::Result<std::optional<cue::BuildArtifactInventory>>::failure(
                    make_error(*m_assertContext, cue::WindowsBuildArtifactError::InvalidSettings,
                               "Build Plan path could not be converted"));
            }
            const std::filesystem::path source = *binary / "bin" / configuration / "CueGameModule.dll";
            const std::filesystem::path sourcePdb = *binary / "bin" / configuration / "CueGameModule.pdb";
            const std::filesystem::path candidate = *candidatePath;
            std::error_code filesystemError;
            const std::filesystem::file_status sourceStatus = std::filesystem::symlink_status(source, filesystemError);
            if (filesystemError || !std::filesystem::is_regular_file(sourceStatus))
            {
                return cue::Result<std::optional<cue::BuildArtifactInventory>>::failure(make_windows_error(
                    *m_assertContext, cue::WindowsBuildArtifactError::CandidateInvalid,
                    filesystemError ? static_cast<DWORD>(filesystemError.value()) : ERROR_FILE_INVALID,
                    "Game Module build output is not a regular file"));
            }
            const bool requiresPdb = a_plan.profile().configuration() != cue::BuildConfiguration::Release;
            const bool hasPdb = std::filesystem::exists(sourcePdb, filesystemError);
            if (filesystemError || (requiresPdb && !hasPdb))
            {
                return cue::Result<std::optional<cue::BuildArtifactInventory>>::failure(make_windows_error(
                    *m_assertContext, cue::WindowsBuildArtifactError::CandidateInvalid,
                    filesystemError ? static_cast<DWORD>(filesystemError.value()) : ERROR_FILE_NOT_FOUND,
                    "Required Game Module PDB build output is unavailable"));
            }
            if (hasPdb)
            {
                const std::filesystem::file_status pdbStatus =
                    std::filesystem::symlink_status(sourcePdb, filesystemError);
                if (filesystemError || !std::filesystem::is_regular_file(pdbStatus))
                {
                    return cue::Result<std::optional<cue::BuildArtifactInventory>>::failure(make_windows_error(
                        *m_assertContext, cue::WindowsBuildArtifactError::CandidateInvalid,
                        filesystemError ? static_cast<DWORD>(filesystemError.value()) : ERROR_FILE_INVALID,
                        "Game Module PDB build output is not a regular file"));
                }
            }
            if (std::filesystem::exists(candidate, filesystemError) || filesystemError ||
                !std::filesystem::create_directories(candidate, filesystemError) || filesystemError)
            {
                return cue::Result<std::optional<cue::BuildArtifactInventory>>::failure(make_windows_error(
                    *m_assertContext, cue::WindowsBuildArtifactError::CandidateInvalid,
                    filesystemError ? static_cast<DWORD>(filesystemError.value()) : ERROR_ALREADY_EXISTS,
                    "Operation candidate directory is unavailable"));
            }
            using PublishResult = cue::Result<std::optional<cue::BuildArtifactInventory>>;
            /// @brief Primary Errorを保持したまま未公開CandidateをRollbackする
            const auto failCandidate = [&](cue::Error a_error) -> PublishResult
            {
                std::error_code cleanupCode;
                static_cast<void>(std::filesystem::remove_all(candidate, cleanupCode));
                if (cleanupCode)
                {
                    cue::Error cleanupError = make_windows_error(
                        *m_assertContext, cue::WindowsBuildArtifactError::CandidateInvalid,
                        static_cast<DWORD>(cleanupCode.value()), "Unpublished Candidate rollback failed");
                    a_error.append_secondary_diagnostics(*m_assertContext, cleanupError,
                                                         "Unpublished Candidate could not be removed", "Rollback");
                }
                return PublishResult::failure(std::move(a_error));
            };
            /// @brief 取消前に未公開CandidateをRollbackしCleanup失敗だけをErrorとして返す
            const auto cancelCandidate = [&]() -> PublishResult
            {
                std::error_code cleanupCode;
                static_cast<void>(std::filesystem::remove_all(candidate, cleanupCode));
                if (cleanupCode)
                {
                    return PublishResult::failure(make_windows_error(
                        *m_assertContext, cue::WindowsBuildArtifactError::CandidateInvalid,
                        static_cast<DWORD>(cleanupCode.value()), "Cancelled Candidate rollback failed"));
                }
                return PublishResult::success(std::nullopt);
            };
            const std::filesystem::path candidateModule = candidate / "CueGameModule.dll";
            cue::Result<void> moduleCopied = copy_new_file_durable(source, candidateModule, *m_assertContext);
            if (!moduleCopied)
            {
                return failCandidate(std::move(*moduleCopied.try_error()));
            }
            if (hasPdb)
            {
                cue::Result<void> pdbCopied =
                    copy_new_file_durable(sourcePdb, candidate / "CueGameModule.pdb", *m_assertContext);
                if (!pdbCopied)
                {
                    return failCandidate(std::move(*pdbCopied.try_error()));
                }
            }
            cue::Result<ModuleProbeStatus> validated =
                validate_module(candidateModule, a_plan, m_projectId, candidate, m_probeExecutable, *m_processRunner,
                                a_cancellation, a_deadline, *m_assertContext);
            if (!validated)
            {
                return failCandidate(std::move(*validated.try_error()));
            }
            if (*validated.try_value() == ModuleProbeStatus::Cancelled)
            {
                return cancelCandidate();
            }
            const std::string metadata =
                serialize_metadata(a_plan.operation_id(), m_projectId, m_compatibility,
                                   a_plan.profile().configuration(), a_plan.workspace_compatibility().toolsetVersion);
            cue::Result<void> metadataWritten =
                write_new_file(candidate / "CueGameModule.metadata.json", metadata,
                               cue::WindowsBuildArtifactError::CandidateInvalid, *m_assertContext);
            if (!metadataWritten)
            {
                return failCandidate(std::move(*metadataWritten.try_error()));
            }
            auto candidateModuleHash = hash_file(candidateModule, "CueGameModule.dll", *m_assertContext);
            std::optional<cue::BuildArtifactFile> candidatePdbHash;
            if (hasPdb)
            {
                auto hashed = hash_file(candidate / "CueGameModule.pdb", "CueGameModule.pdb", *m_assertContext);
                if (!hashed)
                {
                    return failCandidate(std::move(*hashed.try_error()));
                }
                candidatePdbHash.emplace(std::move(*hashed.try_value()));
            }
            auto candidateMetadataHash =
                hash_file(candidate / "CueGameModule.metadata.json", "CueGameModule.metadata.json", *m_assertContext);
            if (!candidateModuleHash)
            {
                return failCandidate(std::move(*candidateModuleHash.try_error()));
            }
            if (!candidateMetadataHash)
            {
                return failCandidate(std::move(*candidateMetadataHash.try_error()));
            }
            a_buildLease.reset();
            if (a_cancellation.is_cancel_requested())
            {
                return cancelCandidate();
            }

            const std::filesystem::path store = *storePath;
            cue::Result<void> storeCreated = ensure_directory(
                store / "Versions", cue::WindowsBuildArtifactError::ArtifactLockFailed, *m_assertContext);
            if (!storeCreated)
            {
                return failCandidate(std::move(*storeCreated.try_error()));
            }
            auto artifactLock =
                acquire_exclusive_lock(store / "Access.lock", a_cancellation, a_deadline,
                                       cue::WindowsBuildArtifactError::ArtifactLockFailed, *m_assertContext);
            if (!artifactLock)
            {
                return failCandidate(std::move(*artifactLock.try_error()));
            }
            if (!artifactLock.try_value()->has_value())
            {
                return cancelCandidate();
            }
            ByteRangeLease mutationLease(std::move(**artifactLock.try_value()));
            const std::filesystem::path version = store / "Versions" / std::string(a_plan.operation_id());
            if (std::filesystem::exists(version, filesystemError) || filesystemError)
            {
                return failCandidate(make_error(*m_assertContext, cue::WindowsBuildArtifactError::ArtifactAlreadyExists,
                                                "Artifact Version already exists"));
            }
            if (a_cancellation.is_cancel_requested())
            {
                return cancelCandidate();
            }
            if (MoveFileExW(candidate.c_str(), version.c_str(), MOVEFILE_WRITE_THROUGH) == FALSE)
            {
                const DWORD code = GetLastError();
                const DWORD versionAttributes = GetFileAttributesW(version.c_str());
                const DWORD candidateAttributes = GetFileAttributesW(candidate.c_str());
                const DWORD candidateCode =
                    candidateAttributes == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_SUCCESS;
                if (versionAttributes != INVALID_FILE_ATTRIBUTES &&
                    (versionAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U &&
                    candidateAttributes == INVALID_FILE_ATTRIBUTES &&
                    (candidateCode == ERROR_FILE_NOT_FOUND || candidateCode == ERROR_PATH_NOT_FOUND))
                {
                    return cue::Result<std::optional<cue::BuildArtifactInventory>>::failure(make_windows_error(
                        *m_assertContext, cue::WindowsBuildArtifactError::ArtifactVersionDurabilityUnknown, code,
                        "Artifact Version is visible but directory publication durability is unknown"));
                }
                return failCandidate(make_windows_error(*m_assertContext,
                                                        cue::WindowsBuildArtifactError::CandidateInvalid, code,
                                                        "Artifact Version could not be published"));
            }
            auto versionModuleHash = hash_file(version / "CueGameModule.dll", "CueGameModule.dll", *m_assertContext);
            std::optional<cue::BuildArtifactFile> versionPdbHash;
            if (hasPdb)
            {
                auto hashed = hash_file(version / "CueGameModule.pdb", "CueGameModule.pdb", *m_assertContext);
                if (!hashed)
                {
                    return cue::Result<std::optional<cue::BuildArtifactInventory>>::failure(
                        std::move(*hashed.try_error()));
                }
                versionPdbHash.emplace(std::move(*hashed.try_value()));
            }
            auto versionMetadataHash =
                hash_file(version / "CueGameModule.metadata.json", "CueGameModule.metadata.json", *m_assertContext);
            if (!versionModuleHash)
            {
                return cue::Result<std::optional<cue::BuildArtifactInventory>>::failure(
                    std::move(*versionModuleHash.try_error()));
            }
            if (!versionMetadataHash)
            {
                return cue::Result<std::optional<cue::BuildArtifactInventory>>::failure(
                    std::move(*versionMetadataHash.try_error()));
            }
            if (versionModuleHash.try_value()->contentHash != candidateModuleHash.try_value()->contentHash ||
                (candidatePdbHash.has_value() &&
                 (!versionPdbHash.has_value() || versionPdbHash->contentHash != candidatePdbHash->contentHash)) ||
                versionMetadataHash.try_value()->contentHash != candidateMetadataHash.try_value()->contentHash)
            {
                return cue::Result<std::optional<cue::BuildArtifactInventory>>::failure(
                    make_error(*m_assertContext, cue::WindowsBuildArtifactError::CandidateInvalid,
                               "Published Artifact differs from the Candidate snapshot"));
            }
            std::vector<cue::BuildArtifactFile> files;
            files.push_back(std::move(*versionModuleHash.try_value()));
            if (versionPdbHash.has_value())
            {
                files.push_back(std::move(*versionPdbHash));
            }
            files.push_back(std::move(*versionMetadataHash.try_value()));
            auto inventory = cue::BuildArtifactInventory::create(a_plan, std::string(a_plan.operation_id()),
                                                                 std::move(files), *m_assertContext);
            if (!inventory)
            {
                return cue::Result<std::optional<cue::BuildArtifactInventory>>::failure(
                    std::move(*inventory.try_error()));
            }
            const std::string currentContent = serialize_current(*inventory.try_value());
            cue::Result<void> current = publish_current(store, a_plan.operation_id(), currentContent, *m_assertContext);
            if (!current)
            {
                cue::Error publicationError = std::move(*current.try_error());
                if (is_current_durability_unknown(publicationError))
                {
                    cue::Result<void> inventoryVerified =
                        verify_inventory_files(version, *inventory.try_value(), *m_assertContext);
                    cue::ChildProcessCancellation validationCancellation;
                    cue::Result<ModuleProbeStatus> moduleVerified =
                        validate_module(version / "CueGameModule.dll", a_plan, m_projectId, store, m_probeExecutable,
                                        *m_processRunner, validationCancellation, a_deadline, *m_assertContext);
                    if (inventoryVerified && moduleVerified && *moduleVerified.try_value() == ModuleProbeStatus::Valid)
                    {
                        std::string context("Visible Current artifact ");
                        context.append(inventory.try_value()->artifact_id());
                        context.append(" at ");
                        context.append(inventory.try_value()->version_directory());
                        context.append(" matched canonical schema, inventory, size, hash, and module contract; "
                                       "durability remains unknown");
                        publicationError.add_context(m_assertContext->fatal_handler(), context);
                    }
                    else
                    {
                        m_isAvailable = false;
                        publicationError.add_context(
                            m_assertContext->fatal_handler(),
                            "Visible Current matched the requested manifest bytes but its referenced artifact could "
                            "not be fully revalidated; Current selection is unknown");
                        if (!inventoryVerified)
                        {
                            publicationError.append_secondary_diagnostics(
                                *m_assertContext, *inventoryVerified.try_error(),
                                "Visible Current inventory revalidation failed", "Inventory validation");
                        }
                        if (!moduleVerified)
                        {
                            publicationError.append_secondary_diagnostics(*m_assertContext, *moduleVerified.try_error(),
                                                                          "Visible Current module revalidation failed",
                                                                          "Module validation");
                        }
                    }
                }
                return cue::Result<std::optional<cue::BuildArtifactInventory>>::failure(std::move(publicationError));
            }
            return cue::Result<std::optional<cue::BuildArtifactInventory>>::success(
                std::optional<cue::BuildArtifactInventory>(std::move(*inventory.try_value())));
        }
        catch (...)
        {
            terminate_artifact_exception(*m_assertContext);
        }
    }

  private:
    std::filesystem::path m_projectRoot;
    std::string m_projectId;
    cue::EngineCompatibility m_compatibility;
    std::string m_probeExecutable;
    std::unique_ptr<cue::ChildProcessRunner> m_processRunner;
    const cue::AssertContext *m_assertContext;
    bool m_isAvailable = true;
};
} // namespace

namespace cue
{
Result<std::unique_ptr<BuildArtifactPublisher>> create_windows_build_artifact_publisher(
    std::string a_projectRoot, const ProjectDescriptor &a_descriptor, const AssertContext &a_assertContext) noexcept
{
    try
    {
        const std::optional<std::filesystem::path> root = to_path(a_projectRoot);
        std::error_code error;
        if (!root || !root->is_absolute() || !std::filesystem::is_directory(*root, error) || error)
        {
            return Result<std::unique_ptr<BuildArtifactPublisher>>::failure(make_error(
                a_assertContext, WindowsBuildArtifactError::InvalidSettings, "Artifact Project Root is invalid"));
        }
        const EngineCompatibility &compatibility = a_descriptor.engine_compatibility();
        const std::optional<std::filesystem::path> processDirectory = current_process_directory();
        if (!processDirectory)
        {
            return Result<std::unique_ptr<BuildArtifactPublisher>>::failure(
                make_windows_error(a_assertContext, WindowsBuildArtifactError::InvalidSettings, GetLastError(),
                                   "Engine process path could not be resolved"));
        }
        auto processRunner = create_windows_child_process_runner(a_assertContext);
        if (!processRunner)
        {
            return Result<std::unique_ptr<BuildArtifactPublisher>>::failure(std::move(*processRunner.try_error()));
        }
        const std::string probeExecutable = path_to_utf8(*processDirectory / L"CueGameModuleProbe.exe");
        return Result<std::unique_ptr<BuildArtifactPublisher>>::success(std::make_unique<WindowsBuildArtifactPublisher>(
            *root, std::string(a_descriptor.project_id().text()), compatibility, probeExecutable,
            std::move(*processRunner.try_value()), a_assertContext));
    }
    catch (...)
    {
        terminate_artifact_exception(a_assertContext);
    }
}
} // namespace cue
