#include <Cue/Build/Windows/WindowsArtifactPublisher.h>

#include <Cue/Foundation/Assert.h>
#include <Cue/GameModule/GameModuleAbi.h>
#include <Cue/Project/Descriptor.h>

#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <bit>
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

/// @brief HMODULEを一意所有し全経路でUnloadする
class UniqueModule final
{
  public:
    /// @brief Native Moduleの一意所有権を取得する
    explicit UniqueModule(HMODULE a_module) noexcept : m_module(a_module)
    {
    }
    /// @brief Module所有権のCopy構築を禁止する
    UniqueModule(const UniqueModule &) = delete;
    /// @brief Module所有権のCopy代入を禁止する
    UniqueModule &operator=(const UniqueModule &) = delete;
    /// @brief 所有ModuleをUnloadする
    ~UniqueModule()
    {
        if (m_module != nullptr)
        {
            FreeLibrary(m_module);
        }
    }
    /// @brief Native API呼出し用Moduleを返す
    [[nodiscard]] HMODULE get() const noexcept
    {
        return m_module;
    }

  private:
    HMODULE m_module = nullptr;
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

/// @brief Project IDの16 byte値をModule ABI比較用に復元する
[[nodiscard]] std::array<std::uint8_t, 16U> project_id_bytes(std::string_view a_projectId) noexcept
{
    /// @brief UUID Hex文字を4-bit値へ変換する
    const auto nibble = [](char a_value) noexcept -> std::uint8_t
    {
        return a_value <= '9' ? static_cast<std::uint8_t>(a_value - '0')
                              : static_cast<std::uint8_t>(a_value - 'a' + 10);
    };
    std::array<std::uint8_t, 16U> bytes{};
    std::size_t output = 0U;
    for (std::size_t index = 0U; index < a_projectId.size();)
    {
        if (a_projectId[index] == '-')
        {
            ++index;
            continue;
        }
        bytes[output++] =
            static_cast<std::uint8_t>((nibble(a_projectId[index]) << 4U) | nibble(a_projectId[index + 1U]));
        index += 2U;
    }
    return bytes;
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
    cue::WindowsBuildArtifactError a_code, const cue::AssertContext &a_assertContext) noexcept
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
        Sleep(k_lockRetryMilliseconds);
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

/// @brief Byte列を新規Fileへ書きFlushする
[[nodiscard]] cue::Result<void> write_new_file(const std::filesystem::path &a_path, std::string_view a_bytes,
                                               cue::WindowsBuildArtifactError a_code,
                                               const cue::AssertContext &a_assertContext) noexcept
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
            return cue::Result<void>::failure(
                make_windows_error(a_assertContext, a_code, code, "Artifact file write failed"));
        }
        offset += written;
    }
    if (FlushFileBuffers(file.get()) == FALSE)
    {
        return cue::Result<void>::failure(
            make_windows_error(a_assertContext, a_code, GetLastError(), "Artifact file flush failed"));
    }
    return cue::Result<void>::success();
}

/// @brief Candidate DLLの公開ABIがBuild PlanとProject契約に一致するか検証する
[[nodiscard]] cue::Result<void> validate_module(const std::filesystem::path &a_path, const cue::BuildPlan &a_plan,
                                                std::span<const std::uint8_t, 16U> a_projectId,
                                                const cue::AssertContext &a_assertContext) noexcept
{
    UniqueModule module(
        LoadLibraryExW(a_path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS));
    if (module.get() == nullptr)
    {
        return cue::Result<void>::failure(make_windows_error(a_assertContext,
                                                             cue::WindowsBuildArtifactError::ModuleContractMismatch,
                                                             GetLastError(), "Game Module could not be loaded"));
    }
    using Query = CueGameModuleResult(CUE_GAME_MODULE_CALL *)(uint32_t, CueGameModuleQueryOutputV1 *,
                                                              CueGameModuleDiagnosticV1 *) noexcept;
    const FARPROC address = GetProcAddress(module.get(), "cue_game_module_query");
    if (address == nullptr)
    {
        return cue::Result<void>::failure(
            make_windows_error(a_assertContext, cue::WindowsBuildArtifactError::ModuleContractMismatch,
                               ERROR_PROC_NOT_FOUND, "Game Module entry symbol is missing"));
    }
    const Query query = std::bit_cast<Query>(address);
    CueGameModuleQueryOutputV1 output{sizeof(output), CUE_GAME_MODULE_STRUCTURE_VERSION_1, nullptr, {0U, 0U}};
    CueGameModuleDiagnosticV1 diagnostic{sizeof(diagnostic),
                                         CUE_GAME_MODULE_STRUCTURE_VERSION_1,
                                         0U,
                                         0U,
                                         {sizeof(CueGameUtf8ViewV1), CUE_GAME_MODULE_STRUCTURE_VERSION_1, nullptr, 0U}};
    const CueGameModuleResult result = query(CUE_GAME_MODULE_ABI_VERSION_1, &output, &diagnostic);
    if (result != CUE_GAME_MODULE_RESULT_SUCCESS || output.api == nullptr)
    {
        return cue::Result<void>::failure(make_error(a_assertContext,
                                                     cue::WindowsBuildArtifactError::ModuleContractMismatch,
                                                     "Game Module query rejected the current ABI"));
    }
    const CueGameModuleApiV1 &api = *output.api;
    const std::uint32_t expectedConfiguration =
        a_plan.profile().configuration() == cue::BuildConfiguration::Debug
            ? CUE_GAME_MODULE_CONFIGURATION_DEBUG
            : (a_plan.profile().configuration() == cue::BuildConfiguration::Development
                   ? CUE_GAME_MODULE_CONFIGURATION_DEVELOPMENT
                   : CUE_GAME_MODULE_CONFIGURATION_RELEASE);
    if (api.structSize != sizeof(CueGameModuleApiV1) || api.version != CUE_GAME_MODULE_STRUCTURE_VERSION_1 ||
        api.abiVersion != CUE_GAME_MODULE_ABI_VERSION_1 || api.configuration != expectedConfiguration ||
        api.architecture != CUE_GAME_MODULE_ARCHITECTURE_X64 || api.projectId.structSize != sizeof(CueGameUuidV1) ||
        api.projectId.version != CUE_GAME_MODULE_STRUCTURE_VERSION_1 ||
        !std::equal(a_projectId.begin(), a_projectId.end(), api.projectId.bytes))
    {
        return cue::Result<void>::failure(make_error(a_assertContext,
                                                     cue::WindowsBuildArtifactError::ModuleContractMismatch,
                                                     "Game Module identity does not match the Build Plan"));
    }
    return cue::Result<void>::success();
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
        static_cast<void>(DeleteFileW(temporary.c_str()));
        return cue::Result<void>::failure(
            make_windows_error(a_assertContext,
                               newManifestVisible ? cue::WindowsBuildArtifactError::CurrentManifestDurabilityUnknown
                                                  : cue::WindowsBuildArtifactError::CurrentManifestFailed,
                               code,
                               newManifestVisible ? "Current artifact manifest is visible but durability is unknown"
                                                  : "Current artifact manifest was not published"));
    }
    return cue::Result<void>::success();
}

/// @brief Windows上でBuild CandidateとArtifact Storeを公開する
class WindowsBuildArtifactPublisher final : public cue::BuildArtifactPublisher
{
  public:
    /// @brief 検証済みProject契約をPublisher全寿命へ所有する
    WindowsBuildArtifactPublisher(std::filesystem::path a_projectRoot, std::string a_projectId,
                                  cue::EngineCompatibility a_compatibility,
                                  const cue::AssertContext &a_assertContext) noexcept
        : m_projectRoot(std::move(a_projectRoot)), m_projectId(std::move(a_projectId)),
          m_projectIdBytes(project_id_bytes(m_projectId)), m_compatibility(std::move(a_compatibility)),
          m_assertContext(&a_assertContext)
    {
    }
    /// @brief 所有Project契約を解放する
    ~WindowsBuildArtifactPublisher() override = default;

    /// @brief Plan固有Binary TreeのExclusive Byte Range Lockを取消可能に取得する
    [[nodiscard]] cue::Result<std::optional<std::unique_ptr<cue::BuildWorkspaceLease>>> acquire_build_lease(
        const cue::BuildPlan &a_plan, const cue::ChildProcessCancellation &a_cancellation) noexcept override
    {
        try
        {
            if (!same_root(m_projectRoot, a_plan.project_root()))
            {
                return cue::Result<std::optional<std::unique_ptr<cue::BuildWorkspaceLease>>>::failure(
                    make_error(*m_assertContext, cue::WindowsBuildArtifactError::InvalidSettings,
                               "Build Plan belongs to another Project Root"));
            }
            const std::filesystem::path lock =
                m_projectRoot / "Generated" / "Build" / "Locks" / (std::string(a_plan.workspace_key()) + ".lock");
            auto acquired = acquire_exclusive_lock(
                lock, a_cancellation, cue::WindowsBuildArtifactError::WorkspaceLockFailed, *m_assertContext);
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
        std::unique_ptr<cue::BuildWorkspaceLease> a_buildLease) noexcept override
    {
        try
        {
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
            if (std::filesystem::exists(candidate, filesystemError) || filesystemError ||
                !std::filesystem::create_directories(candidate, filesystemError) || filesystemError)
            {
                return cue::Result<std::optional<cue::BuildArtifactInventory>>::failure(make_windows_error(
                    *m_assertContext, cue::WindowsBuildArtifactError::CandidateInvalid,
                    filesystemError ? static_cast<DWORD>(filesystemError.value()) : ERROR_ALREADY_EXISTS,
                    "Operation candidate directory is unavailable"));
            }
            const std::filesystem::path candidateModule = candidate / "CueGameModule.dll";
            if (!std::filesystem::copy_file(source, candidateModule, std::filesystem::copy_options::none,
                                            filesystemError) ||
                filesystemError)
            {
                return cue::Result<std::optional<cue::BuildArtifactInventory>>::failure(make_windows_error(
                    *m_assertContext, cue::WindowsBuildArtifactError::CandidateInvalid,
                    filesystemError ? static_cast<DWORD>(filesystemError.value()) : ERROR_FILE_EXISTS,
                    "Game Module candidate copy failed"));
            }
            cue::Result<void> validated = validate_module(candidateModule, a_plan, m_projectIdBytes, *m_assertContext);
            if (!validated)
            {
                return cue::Result<std::optional<cue::BuildArtifactInventory>>::failure(
                    std::move(*validated.try_error()));
            }
            const std::string metadata =
                serialize_metadata(a_plan.operation_id(), m_projectId, m_compatibility,
                                   a_plan.profile().configuration(), a_plan.workspace_compatibility().toolsetVersion);
            cue::Result<void> metadataWritten =
                write_new_file(candidate / "CueGameModule.metadata.json", metadata,
                               cue::WindowsBuildArtifactError::CandidateInvalid, *m_assertContext);
            if (!metadataWritten)
            {
                return cue::Result<std::optional<cue::BuildArtifactInventory>>::failure(
                    std::move(*metadataWritten.try_error()));
            }
            auto candidateModuleHash = hash_file(candidateModule, "CueGameModule.dll", *m_assertContext);
            auto candidateMetadataHash =
                hash_file(candidate / "CueGameModule.metadata.json", "CueGameModule.metadata.json", *m_assertContext);
            if (!candidateModuleHash)
            {
                return cue::Result<std::optional<cue::BuildArtifactInventory>>::failure(
                    std::move(*candidateModuleHash.try_error()));
            }
            if (!candidateMetadataHash)
            {
                return cue::Result<std::optional<cue::BuildArtifactInventory>>::failure(
                    std::move(*candidateMetadataHash.try_error()));
            }
            a_buildLease.reset();
            if (a_cancellation.is_cancel_requested())
            {
                return cue::Result<std::optional<cue::BuildArtifactInventory>>::success(std::nullopt);
            }

            const std::filesystem::path store = *storePath;
            cue::Result<void> storeCreated = ensure_directory(
                store / "Versions", cue::WindowsBuildArtifactError::ArtifactLockFailed, *m_assertContext);
            if (!storeCreated)
            {
                return cue::Result<std::optional<cue::BuildArtifactInventory>>::failure(
                    std::move(*storeCreated.try_error()));
            }
            auto artifactLock =
                acquire_exclusive_lock(store / "Access.lock", a_cancellation,
                                       cue::WindowsBuildArtifactError::ArtifactLockFailed, *m_assertContext);
            if (!artifactLock)
            {
                return cue::Result<std::optional<cue::BuildArtifactInventory>>::failure(
                    std::move(*artifactLock.try_error()));
            }
            if (!artifactLock.try_value()->has_value())
            {
                return cue::Result<std::optional<cue::BuildArtifactInventory>>::success(std::nullopt);
            }
            ByteRangeLease mutationLease(std::move(**artifactLock.try_value()));
            const std::filesystem::path version = store / "Versions" / std::string(a_plan.operation_id());
            if (std::filesystem::exists(version, filesystemError) || filesystemError)
            {
                return cue::Result<std::optional<cue::BuildArtifactInventory>>::failure(
                    make_error(*m_assertContext, cue::WindowsBuildArtifactError::ArtifactAlreadyExists,
                               "Artifact Version already exists"));
            }
            std::filesystem::rename(candidate, version, filesystemError);
            if (filesystemError)
            {
                return cue::Result<std::optional<cue::BuildArtifactInventory>>::failure(
                    make_windows_error(*m_assertContext, cue::WindowsBuildArtifactError::CandidateInvalid,
                                       filesystemError.value(), "Artifact Version could not be published"));
            }
            auto versionModuleHash = hash_file(version / "CueGameModule.dll", "CueGameModule.dll", *m_assertContext);
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
                versionMetadataHash.try_value()->contentHash != candidateMetadataHash.try_value()->contentHash)
            {
                return cue::Result<std::optional<cue::BuildArtifactInventory>>::failure(
                    make_error(*m_assertContext, cue::WindowsBuildArtifactError::CandidateInvalid,
                               "Published Artifact differs from the Candidate snapshot"));
            }
            std::vector<cue::BuildArtifactFile> files;
            files.push_back(std::move(*versionModuleHash.try_value()));
            files.push_back(std::move(*versionMetadataHash.try_value()));
            auto inventory = cue::BuildArtifactInventory::create(a_plan, std::string(a_plan.operation_id()),
                                                                 std::move(files), *m_assertContext);
            if (!inventory)
            {
                return cue::Result<std::optional<cue::BuildArtifactInventory>>::failure(
                    std::move(*inventory.try_error()));
            }
            if (a_cancellation.is_cancel_requested())
            {
                return cue::Result<std::optional<cue::BuildArtifactInventory>>::success(std::nullopt);
            }
            cue::Result<void> current = publish_current(store, a_plan.operation_id(),
                                                        serialize_current(*inventory.try_value()), *m_assertContext);
            if (!current)
            {
                return cue::Result<std::optional<cue::BuildArtifactInventory>>::failure(
                    std::move(*current.try_error()));
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
    std::array<std::uint8_t, 16U> m_projectIdBytes;
    cue::EngineCompatibility m_compatibility;
    const cue::AssertContext *m_assertContext;
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
        return Result<std::unique_ptr<BuildArtifactPublisher>>::success(std::make_unique<WindowsBuildArtifactPublisher>(
            *root, std::string(a_descriptor.project_id().text()), compatibility, a_assertContext));
    }
    catch (...)
    {
        terminate_artifact_exception(a_assertContext);
    }
}
} // namespace cue
