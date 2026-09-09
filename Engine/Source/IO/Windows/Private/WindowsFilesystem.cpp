#include <Cue/IO/Windows/WindowsFilesystem.h>

#include "WindowsFilesystemIdentity.h"

#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Windows/UtfConversion.h>
#include <Cue/IO/Error.h>

#include <Windows.h>
#include <bcrypt.h>
#include <ShlObj.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cwctype>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
constexpr std::size_t k_maxWindowsPathLength = 32767;
constexpr std::size_t k_randomByteCount = 16;
SRWLOCK g_writeLeaseRegistryLock = SRWLOCK_INIT;
std::unordered_map<std::wstring, DWORD> g_heldWriteLeaseThreads;

/// @brief Process内で同じNamed Mutexを再帰取得しないよう取得Threadを登録する
[[nodiscard]] bool register_write_lease(const std::wstring &a_name, DWORD a_threadId)
{
    AcquireSRWLockExclusive(&g_writeLeaseRegistryLock);
    const bool inserted = g_heldWriteLeaseThreads.emplace(a_name, a_threadId).second;
    ReleaseSRWLockExclusive(&g_writeLeaseRegistryLock);
    return inserted;
}

/// @brief Process内Lease登録を取得Threadから解除する
void unregister_write_lease(const std::wstring &a_name) noexcept
{
    AcquireSRWLockExclusive(&g_writeLeaseRegistryLock);
    g_heldWriteLeaseThreads.erase(a_name);
    ReleaseSRWLockExclusive(&g_writeLeaseRegistryLock);
}

/// @brief Win32 Handle を一意所有して全ての終了経路で Close する
class UniqueHandle final
{
  public:
    /// @brief 無効 Handle を所有しない状態で生成する
    UniqueHandle() noexcept = default;
    /// @brief Native Handle の Close 責務を受け取る
    explicit UniqueHandle(HANDLE a_handle) noexcept : m_handle(a_handle)
    {
    }
    /// @brief Handle の二重 Close を防ぐため Copy 構築を禁止する
    UniqueHandle(const UniqueHandle &) = delete;
    /// @brief Handle の二重 Close を防ぐため Copy 代入を禁止する
    UniqueHandle &operator=(const UniqueHandle &) = delete;
    /// @brief Handle の Close 責務を移動する
    UniqueHandle(UniqueHandle &&a_other) noexcept : m_handle(a_other.release())
    {
    }
    /// @brief 現在の Handle を Close してから新しい責務を移動する
    UniqueHandle &operator=(UniqueHandle &&a_other) noexcept
    {
        if (this != &a_other)
        {
            reset(a_other.release());
        }
        return *this;
    }
    /// @brief 有効な Native Handle を Close する
    ~UniqueHandle()
    {
        reset();
    }

    /// @brief 所有 Handle を返す
    [[nodiscard]] HANDLE get() const noexcept
    {
        return m_handle;
    }

    /// @brief Handle が Native API 呼出しに使用可能か判定する
    [[nodiscard]] bool is_valid() const noexcept
    {
        return m_handle != nullptr && m_handle != INVALID_HANDLE_VALUE;
    }

    /// @brief Close せず Handle 所有権を呼び出し側へ渡す
    [[nodiscard]] HANDLE release() noexcept
    {
        HANDLE handle = m_handle;
        m_handle = INVALID_HANDLE_VALUE;
        return handle;
    }

    /// @brief 現在の Handle を Close して指定 Handle を所有する
    void reset(HANDLE a_handle = INVALID_HANDLE_VALUE) noexcept
    {
        if (is_valid())
        {
            CloseHandle(m_handle);
        }
        m_handle = a_handle;
    }

  private:
    HANDLE m_handle = INVALID_HANDLE_VALUE;
};

/// @brief SHGetKnownFolderPathが割り当てた文字列を一意所有する
class UniqueCoTaskMemString final
{
  public:
    /// @brief 空のShell文字列所有者を生成する
    UniqueCoTaskMemString() noexcept = default;
    UniqueCoTaskMemString(const UniqueCoTaskMemString &) = delete;
    UniqueCoTaskMemString &operator=(const UniqueCoTaskMemString &) = delete;

    /// @brief Shell Allocatorへ所有文字列を返却する
    ~UniqueCoTaskMemString() noexcept
    {
        CoTaskMemFree(m_value);
    }

    /// @brief Shell APIが割当先を書き込むAddressを返す
    [[nodiscard]] PWSTR *put() noexcept
    {
        return &m_value;
    }

    /// @brief 所有中のUTF-16文字列を非所有で返す
    [[nodiscard]] PCWSTR get() const noexcept
    {
        return m_value;
    }

  private:
    PWSTR m_value = nullptr;
};

/// @brief Windows Mutex Handleと取得元Root／Path／Threadを一意所有するWrite Lease State
class WindowsFileWriteLeaseState final : public cue::FileWriteLeaseState
{
  public:
    WindowsFileWriteLeaseState(const void *a_owner, std::string a_exactPathKey, std::string a_familyPathKey,
                               std::wstring a_mutexName, DWORD a_ownerThreadId,
                               const cue::AssertContext &a_assertContext, UniqueHandle a_handle) noexcept
        : owner(a_owner), exactPathKey(std::move(a_exactPathKey)), familyPathKey(std::move(a_familyPathKey)),
          mutexName(std::move(a_mutexName)), ownerThreadId(a_ownerThreadId), assertContext(&a_assertContext),
          handle(std::move(a_handle))
    {
    }

    /// @brief 取得ThreadでMutexとProcess内登録を解放する
    ~WindowsFileWriteLeaseState() override
    {
        if (!handle.is_valid())
        {
            return;
        }
        if (GetCurrentThreadId() != ownerThreadId)
        {
            assertContext->fatal_handler().terminate("Windows write lease was destroyed on a foreign thread");
        }
        unregister_write_lease(mutexName);
        ReleaseMutex(handle.get());
    }

    const void *owner;
    std::string exactPathKey;
    std::string familyPathKey;
    std::wstring mutexName;
    DWORD ownerThreadId;
    const cue::AssertContext *assertContext;
    UniqueHandle handle;
};

/// @brief FindFirstFile で取得した Search Handle を FindClose で一意解放する
class UniqueFindHandle final
{
  public:
    /// @brief 無効 Search Handle から生成する
    UniqueFindHandle() noexcept = default;
    /// @brief Search Handle の FindClose 責務を受け取る
    explicit UniqueFindHandle(HANDLE a_handle) noexcept : m_handle(a_handle)
    {
    }
    /// @brief Search Handle の二重解放を防ぐため Copy 構築を禁止する
    UniqueFindHandle(const UniqueFindHandle &) = delete;
    /// @brief Search Handle の二重解放を防ぐため Copy 代入を禁止する
    UniqueFindHandle &operator=(const UniqueFindHandle &) = delete;
    /// @brief Search Handle の解放責務を移動する
    UniqueFindHandle(UniqueFindHandle &&a_other) noexcept : m_handle(a_other.release())
    {
    }
    /// @brief 現在の Search Handle を解放して新しい責務を移動する
    UniqueFindHandle &operator=(UniqueFindHandle &&a_other) noexcept
    {
        if (this != &a_other)
        {
            reset(a_other.release());
        }
        return *this;
    }
    /// @brief 有効な Search Handle を FindClose で解放する
    ~UniqueFindHandle()
    {
        reset();
    }

    /// @brief 所有 Search Handle を返す
    [[nodiscard]] HANDLE get() const noexcept
    {
        return m_handle;
    }

    /// @brief Search Handle が列挙に使用可能か判定する
    [[nodiscard]] bool is_valid() const noexcept
    {
        return m_handle != INVALID_HANDLE_VALUE;
    }

    /// @brief FindClose せず Search Handle 所有権を呼び出し側へ渡す
    [[nodiscard]] HANDLE release() noexcept
    {
        HANDLE handle = m_handle;
        m_handle = INVALID_HANDLE_VALUE;
        return handle;
    }

    /// @brief 現在の Search Handle を FindClose して指定 Handle を所有する
    void reset(HANDLE a_handle = INVALID_HANDLE_VALUE) noexcept
    {
        if (is_valid())
        {
            FindClose(m_handle);
        }
        m_handle = a_handle;
    }

  private:
    HANDLE m_handle = INVALID_HANDLE_VALUE;
};

/// @brief Allocation 失敗を追加 Allocation なしで Fatal 境界へ渡す
[[noreturn]] void terminate_allocation(const cue::AssertContext &a_context) noexcept
{
    a_context.fatal_handler().terminate("Windows filesystem allocation failed");
    std::abort();
}

/// @brief Win32 Error を呼び出し側が扱える Portable IO 分類へ変換する
[[nodiscard]] cue::IoError classify_windows_error(DWORD a_code) noexcept
{
    switch (a_code)
    {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
        return cue::IoError::NotFound;
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS:
        return cue::IoError::AlreadyExists;
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
        return cue::IoError::PermissionDenied;
    case ERROR_DISK_FULL:
    case ERROR_HANDLE_DISK_FULL:
    case ERROR_FILENAME_EXCED_RANGE:
        return cue::IoError::CapacityExceeded;
    default:
        return cue::IoError::IoFailure;
    }
}

/// @brief Native Error Code を保持した Portable IO Error を生成する
[[nodiscard]] cue::Error make_windows_error(const cue::AssertContext &a_context, DWORD a_code,
                                            std::string_view a_summary) noexcept
{
    return cue::make_io_error(a_context, classify_windows_error(a_code), a_summary, static_cast<std::int64_t>(a_code));
}

/// @brief 主処理失敗後の Temporary File 削除失敗も失わず一つの Error へ集約する
[[nodiscard]] cue::Error remove_temporary_after_failure(const cue::AssertContext &a_context,
                                                        const std::wstring &a_temporaryPath,
                                                        cue::Error &&a_primary) noexcept
{
    if (DeleteFileW(a_temporaryPath.c_str()) == FALSE)
    {
        cue::Error cleanup = make_windows_error(a_context, GetLastError(), "Temporary file cleanup failed");
        a_primary.append_secondary_diagnostics(a_context, cleanup, "Atomic write rollback failed", "Cue.IO cleanup");
    }
    return std::move(a_primary);
}

/// @brief Staging 作成途中の Primary Error を維持しながら Directory Cleanup 失敗を追加する
[[nodiscard]] cue::Error remove_staging_after_creation_failure(const cue::AssertContext &a_context,
                                                               const std::wstring &a_stagingPath,
                                                               cue::Error &&a_primary) noexcept
{
    if (RemoveDirectoryW(a_stagingPath.c_str()) == FALSE)
    {
        cue::Error cleanup =
            make_windows_error(a_context, GetLastError(), "Incomplete staging directory cleanup failed");
        a_primary.append_secondary_diagnostics(a_context, cleanup, "Staging creation rollback failed",
                                               "Cue.IO cleanup");
    }
    return std::move(a_primary);
}

/// @brief Windows の Publish と耐久性試行を分離せず、失敗時に可視化済みかを保持する
struct NativePublishOutcome final
{
    bool isPublished;
    DWORD nativeCode;
};

/// @brief Destinationが期待したNative Directory Objectを指す場合だけtrueを返す
[[nodiscard]] bool matches_directory_identity(const std::wstring &a_destination,
                                              const cue::windows_io::NativeFilesystemIdentity &a_expected,
                                              const cue::AssertContext &a_context) noexcept
{
    UniqueHandle destination(CreateFileW(a_destination.c_str(), FILE_READ_ATTRIBUTES,
                                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                         OPEN_EXISTING,
                                         FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!destination.is_valid())
    {
        return false;
    }

    BY_HANDLE_FILE_INFORMATION information{};
    if (GetFileInformationByHandle(destination.get(), &information) == FALSE ||
        (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
    {
        return false;
    }
    cue::Result<cue::windows_io::NativeFilesystemIdentity> actual =
        cue::windows_io::inspect_native_filesystem_identity(destination.get(), a_context);
    return actual && actual.try_value()->volumeHigh == a_expected.volumeHigh &&
           actual.try_value()->volumeLow == a_expected.volumeLow &&
           actual.try_value()->entryHigh == a_expected.entryHigh &&
           actual.try_value()->entryLow == a_expected.entryLow;
}

/// @brief MOVEFILE_WRITE_THROUGH の失敗後も Source と Destination の可視状態から公開状態を分類する
[[nodiscard]] NativePublishOutcome publish_with_durability(const std::wstring &a_source,
                                                            const std::wstring &a_destination,
                                                            DWORD a_flags) noexcept
{
    if (MoveFileExW(a_source.c_str(), a_destination.c_str(), a_flags | MOVEFILE_WRITE_THROUGH) != FALSE)
    {
        return NativePublishOutcome{true, ERROR_SUCCESS};
    }

    const DWORD publishCode = GetLastError();
    const DWORD sourceAttributes = GetFileAttributesW(a_source.c_str());
    const DWORD sourceCode = sourceAttributes == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_SUCCESS;
    const DWORD destinationAttributes = GetFileAttributesW(a_destination.c_str());
    const bool isSourceMissing = sourceAttributes == INVALID_FILE_ATTRIBUTES &&
                                 (sourceCode == ERROR_FILE_NOT_FOUND || sourceCode == ERROR_PATH_NOT_FOUND);
    return NativePublishOutcome{isSourceMissing && destinationAttributes != INVALID_FILE_ATTRIBUTES, publishCode};
}

/// @brief 固定済みDirectory Handleを置換なしでRenameし失敗後はDestination Identityから公開状態を分類する
[[nodiscard]] NativePublishOutcome publish_handle_with_durability(
    HANDLE a_source, const std::wstring &a_destination,
    const cue::windows_io::NativeFilesystemIdentity &a_expectedIdentity,
    const cue::AssertContext &a_context) noexcept
{
    constexpr std::size_t headerSize = offsetof(FILE_RENAME_INFO, FileName);
    constexpr std::size_t maxFileNameBytes =
        std::numeric_limits<DWORD>::max() - headerSize - sizeof(wchar_t);
    if (a_destination.size() > maxFileNameBytes / sizeof(wchar_t))
    {
        return NativePublishOutcome{false, ERROR_FILENAME_EXCED_RANGE};
    }

    const DWORD fileNameBytes = static_cast<DWORD>(a_destination.size() * sizeof(wchar_t));
    std::vector<std::byte> storage;
    try
    {
        storage.resize(headerSize + fileNameBytes + sizeof(wchar_t));
    }
    catch (...)
    {
        terminate_allocation(a_context);
    }
    FILE_RENAME_INFO *rename = reinterpret_cast<FILE_RENAME_INFO *>(storage.data());
    rename->ReplaceIfExists = FALSE;
    rename->RootDirectory = nullptr;
    rename->FileNameLength = fileNameBytes;
    std::copy(a_destination.begin(), a_destination.end(), rename->FileName);

    if (SetFileInformationByHandle(a_source, FileRenameInfo, rename, static_cast<DWORD>(storage.size())) != FALSE)
    {
        return NativePublishOutcome{true, ERROR_SUCCESS};
    }

    const DWORD publishCode = GetLastError();
    return NativePublishOutcome{
        matches_directory_identity(a_destination, a_expectedIdentity, a_context), publishCode};
}

/// @brief UTF-8 を Strict UTF-16 へ変換して IO Error として失敗を返す
[[nodiscard]] cue::Result<std::wstring> to_utf16(std::string_view a_text, const cue::AssertContext &a_context) noexcept
{
    std::wstring converted;
    const cue::WindowsUtfConversionResult conversion =
        cue::convert_utf8_to_windows_utf16(a_text, converted, a_context.fatal_handler());
    if (conversion.status != cue::WindowsUtfConversionStatus::Success)
    {
        cue::IoError code = cue::IoError::IoFailure;
        if (conversion.status == cue::WindowsUtfConversionStatus::InvalidSequence)
        {
            code = cue::IoError::InvalidPath;
        }
        else if (conversion.status == cue::WindowsUtfConversionStatus::InputTooLong)
        {
            code = cue::IoError::CapacityExceeded;
        }
        return cue::Result<std::wstring>::failure(
            cue::make_io_error(a_context, code, "Filesystem path UTF-8 conversion failed", conversion.nativeCode));
    }
    return cue::Result<std::wstring>::success(std::move(converted));
}

/// @brief Windows UTF-16 PathをStrict UTF-8へ変換してIO Errorとして失敗を返す
[[nodiscard]] cue::Result<std::string> to_utf8(std::wstring_view a_text,
                                               const cue::AssertContext &a_context) noexcept
{
    std::string converted;
    const cue::WindowsUtfConversionResult conversion =
        cue::convert_windows_utf16_to_utf8(a_text, converted, a_context.fatal_handler());
    if (conversion.status != cue::WindowsUtfConversionStatus::Success)
    {
        cue::IoError code = cue::IoError::IoFailure;
        if (conversion.status == cue::WindowsUtfConversionStatus::InvalidSequence)
        {
            code = cue::IoError::InvalidPath;
        }
        else if (conversion.status == cue::WindowsUtfConversionStatus::InputTooLong)
        {
            code = cue::IoError::CapacityExceeded;
        }
        return cue::Result<std::string>::failure(
            cue::make_io_error(a_context, code, "Filesystem path UTF-16 conversion failed", conversion.nativeCode));
    }
    return cue::Result<std::string>::success(std::move(converted));
}

/// @brief Absolute Windows Path を Extended-length 表現へ変換する
[[nodiscard]] cue::Result<std::wstring> make_extended_path(std::wstring a_path,
                                                           const cue::AssertContext &a_context) noexcept
{
    try
    {
        if (a_path.starts_with(L"\\\\?\\"))
        {
            return cue::Result<std::wstring>::success(std::move(a_path));
        }
        if (a_path.starts_with(L"\\\\"))
        {
            a_path.erase(0, 2);
            a_path.insert(0, L"\\\\?\\UNC\\");
        }
        else
        {
            a_path.insert(0, L"\\\\?\\");
        }
    }
    catch (...)
    {
        terminate_allocation(a_context);
    }

    if (a_path.size() >= k_maxWindowsPathLength)
    {
        return cue::Result<std::wstring>::failure(
            cue::make_io_error(a_context, cue::IoError::CapacityExceeded, "Windows path length exceeds limit"));
    }
    return cue::Result<std::wstring>::success(std::move(a_path));
}

/// @brief Random Byte を lowercase Hex へ変換して Collision-resistant Name へ使用する
[[nodiscard]] cue::Result<std::string> make_random_hex(const cue::AssertContext &a_context) noexcept
{
    std::array<unsigned char, k_randomByteCount> bytes{};
    const NTSTATUS status =
        BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (status < 0)
    {
        return cue::Result<std::string>::failure(
            cue::make_io_error(a_context, cue::IoError::IoFailure, "Windows random name generation failed", status));
    }

    constexpr char k_hex[] = "0123456789abcdef";
    std::string result;
    try
    {
        result.resize(bytes.size() * 2);
        for (std::size_t index = 0; index < bytes.size(); ++index)
        {
            result[index * 2] = k_hex[bytes[index] >> 4];
            result[index * 2 + 1] = k_hex[bytes[index] & 0x0f];
        }
    }
    catch (...)
    {
        terminate_allocation(a_context);
    }
    return cue::Result<std::string>::success(std::move(result));
}

/// @brief Relative Path の親部分を `/` 区切りで返す
[[nodiscard]] std::string_view parent_text(std::string_view a_path) noexcept
{
    const std::size_t separator = a_path.rfind('/');
    return separator == std::string_view::npos ? std::string_view{} : a_path.substr(0, separator);
}

/// @brief Destinationの比較KeyをCross-process Lease Keyへ変換する
[[nodiscard]] std::string write_lease_path_key(const cue::RelativePath &a_path,
                                               const cue::AssertContext &a_context) noexcept
{
    return a_path.comparison_key(a_context);
}

/// @brief Parent と Child Segment を `/` で結合する
[[nodiscard]] std::string join_relative(std::string_view a_parent, std::string_view a_child,
                                        const cue::AssertContext &a_context) noexcept
{
    std::string result;
    try
    {
        result.reserve(a_parent.size() + (a_parent.empty() ? 0 : 1) + a_child.size());
        result.append(a_parent);
        if (!a_parent.empty())
        {
            result.push_back('/');
        }
        result.append(a_child);
    }
    catch (...)
    {
        terminate_allocation(a_context);
    }
    return result;
}

/// @brief Directory Tree が Reparse Point を含まないか再帰検証する
[[nodiscard]] cue::Result<void> validate_tree(const std::wstring &a_directory,
                                              const cue::AssertContext &a_context) noexcept
{
    UniqueHandle directory(CreateFileW(a_directory.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                       FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!directory.is_valid())
    {
        return cue::Result<void>::failure(
            make_windows_error(a_context, GetLastError(), "Staging directory inspection failed"));
    }
    BY_HANDLE_FILE_INFORMATION information{};
    if (GetFileInformationByHandle(directory.get(), &information) == FALSE)
    {
        return cue::Result<void>::failure(
            make_windows_error(a_context, GetLastError(), "Staging directory inspection failed"));
    }
    if ((information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
    {
        return cue::Result<void>::failure(
            cue::make_io_error(a_context, cue::IoError::UnsupportedEntry, "Staging root is a reparse point"));
    }
    if ((information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
    {
        return cue::Result<void>::failure(
            cue::make_io_error(a_context, cue::IoError::TypeMismatch, "Staging root is not a directory"));
    }

    std::wstring pattern;
    try
    {
        pattern = a_directory + L"\\*";
    }
    catch (...)
    {
        terminate_allocation(a_context);
    }

    WIN32_FIND_DATAW data{};
    HANDLE rawFind = FindFirstFileW(pattern.c_str(), &data);
    if (rawFind == INVALID_HANDLE_VALUE)
    {
        return cue::Result<void>::failure(make_windows_error(a_context, GetLastError(), "Staging scan failed"));
    }

    UniqueFindHandle findHandle(rawFind);
    while (true)
    {
        const std::wstring_view name(data.cFileName);
        if (name != L"." && name != L"..")
        {
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
            {
                return cue::Result<void>::failure(cue::make_io_error(a_context, cue::IoError::UnsupportedEntry,
                                                                     "Staging tree contains a reparse point"));
            }

            if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
            {
                std::wstring child;
                try
                {
                    child = a_directory + L"\\" + std::wstring(name);
                }
                catch (...)
                {
                    terminate_allocation(a_context);
                }
                cue::Result<void> nested = validate_tree(child, a_context);
                if (!nested)
                {
                    return nested;
                }
            }
        }

        if (FindNextFileW(findHandle.get(), &data) == FALSE)
        {
            const DWORD code = GetLastError();
            if (code == ERROR_NO_MORE_FILES)
            {
                break;
            }
            return cue::Result<void>::failure(make_windows_error(a_context, code, "Staging scan failed"));
        }
    }
    return cue::Result<void>::success();
}

/// @brief Operation 所有 Directory Tree を Reparse Point を Follow せず削除する
[[nodiscard]] cue::Result<void> remove_tree(const std::wstring &a_directory,
                                            const cue::AssertContext &a_context) noexcept
{
    cue::Result<void> validation = validate_tree(a_directory, a_context);
    if (!validation)
    {
        return validation;
    }

    std::wstring pattern;
    try
    {
        pattern = a_directory + L"\\*";
    }
    catch (...)
    {
        terminate_allocation(a_context);
    }

    WIN32_FIND_DATAW data{};
    HANDLE rawFind = FindFirstFileW(pattern.c_str(), &data);
    if (rawFind == INVALID_HANDLE_VALUE)
    {
        return cue::Result<void>::failure(make_windows_error(a_context, GetLastError(), "Staging cleanup scan failed"));
    }
    UniqueFindHandle findHandle(rawFind);

    while (true)
    {
        const std::wstring_view name(data.cFileName);
        if (name != L"." && name != L"..")
        {
            std::wstring child;
            try
            {
                child = a_directory + L"\\" + std::wstring(name);
            }
            catch (...)
            {
                terminate_allocation(a_context);
            }

            if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
            {
                cue::Result<void> nested = remove_tree(child, a_context);
                if (!nested)
                {
                    return nested;
                }
            }
            else if (DeleteFileW(child.c_str()) == FALSE)
            {
                return cue::Result<void>::failure(
                    make_windows_error(a_context, GetLastError(), "Staging file cleanup failed"));
            }
        }

        if (FindNextFileW(findHandle.get(), &data) == FALSE)
        {
            const DWORD code = GetLastError();
            if (code == ERROR_NO_MORE_FILES)
            {
                break;
            }
            return cue::Result<void>::failure(make_windows_error(a_context, code, "Staging cleanup scan failed"));
        }
    }

    findHandle.reset();
    if (RemoveDirectoryW(a_directory.c_str()) == FALSE)
    {
        return cue::Result<void>::failure(
            make_windows_error(a_context, GetLastError(), "Staging directory cleanup failed"));
    }
    return cue::Result<void>::success();
}

/// @brief Absolute Pathの最終EntryをReparse Pointを追跡せず分類する
[[nodiscard]] cue::Result<cue::EntryType> query_absolute_entry(std::wstring_view a_path,
                                                               const cue::AssertContext &a_context) noexcept
{
    std::wstring path;
    try
    {
        path.assign(a_path);
    }
    catch (...)
    {
        terminate_allocation(a_context);
    }

    cue::Result<std::wstring> extended = make_extended_path(std::move(path), a_context);
    if (!extended)
    {
        return cue::Result<cue::EntryType>::failure(std::move(*extended.try_error()));
    }
    const DWORD attributes = GetFileAttributesW(extended.try_value()->c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES)
    {
        const DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND)
        {
            return cue::Result<cue::EntryType>::success(cue::EntryType::Missing);
        }
        return cue::Result<cue::EntryType>::failure(
            make_windows_error(a_context, code, "Known Folder root inspection failed"));
    }
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
    {
        return cue::Result<cue::EntryType>::success(cue::EntryType::UnsupportedEntry);
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
    {
        return cue::Result<cue::EntryType>::success(cue::EntryType::Directory);
    }
    return cue::Result<cue::EntryType>::success(cue::EntryType::RegularFile);
}

/// @brief Absolute Pathが通常Directoryであることを検証し型衝突を安定分類する
[[nodiscard]] cue::Result<void> require_absolute_directory(std::wstring_view a_path,
                                                           const cue::AssertContext &a_context) noexcept
{
    cue::Result<cue::EntryType> type = query_absolute_entry(a_path, a_context);
    if (!type)
    {
        return cue::Result<void>::failure(std::move(*type.try_error()));
    }
    if (*type.try_value() == cue::EntryType::Directory)
    {
        return cue::Result<void>::success();
    }
    if (*type.try_value() == cue::EntryType::UnsupportedEntry)
    {
        return cue::Result<void>::failure(cue::make_io_error(
            a_context, cue::IoError::UnsupportedEntry, "Known Folder root contains a reparse point"));
    }
    if (*type.try_value() == cue::EntryType::RegularFile)
    {
        return cue::Result<void>::failure(
            cue::make_io_error(a_context, cue::IoError::TypeMismatch, "Known Folder root contains a file"));
    }
    return cue::Result<void>::failure(
        make_windows_error(a_context, ERROR_PATH_NOT_FOUND, "Known Folder root does not exist"));
}

/// @brief 検証済みKnown Folder親Directoryを置換不能な非追跡Handleとして固定する
[[nodiscard]] cue::Result<UniqueHandle> pin_absolute_directory(std::wstring_view a_path,
                                                               const cue::AssertContext &a_context) noexcept
{
    std::wstring path;
    try
    {
        path.assign(a_path);
    }
    catch (...)
    {
        terminate_allocation(a_context);
    }
    cue::Result<std::wstring> extended = make_extended_path(std::move(path), a_context);
    if (!extended)
    {
        return cue::Result<UniqueHandle>::failure(std::move(*extended.try_error()));
    }

    UniqueHandle directory(CreateFileW(extended.try_value()->c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                       FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!directory.is_valid())
    {
        return cue::Result<UniqueHandle>::failure(
            make_windows_error(a_context, GetLastError(), "Known Folder root pin failed"));
    }

    BY_HANDLE_FILE_INFORMATION information{};
    if (GetFileInformationByHandle(directory.get(), &information) == FALSE)
    {
        return cue::Result<UniqueHandle>::failure(
            make_windows_error(a_context, GetLastError(), "Known Folder root identity query failed"));
    }
    if ((information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
    {
        return cue::Result<UniqueHandle>::failure(cue::make_io_error(
            a_context, cue::IoError::UnsupportedEntry, "Known Folder root contains a reparse point"));
    }
    if ((information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
    {
        return cue::Result<UniqueHandle>::failure(
            cue::make_io_error(a_context, cue::IoError::TypeMismatch, "Known Folder root contains a file"));
    }
    return cue::Result<UniqueHandle>::success(std::move(directory));
}

/// @brief Known Folder配下の一Segmentを既存Directoryとして開くか競合安全に作成する
[[nodiscard]] cue::Result<void> open_absolute_directory(std::wstring_view a_path, cue::WindowsRootOpenMode a_mode,
                                                        const cue::AssertContext &a_context) noexcept
{
    constexpr std::size_t k_maxCreateAttempts = 3;
    for (std::size_t attempt = 0; attempt < k_maxCreateAttempts; ++attempt)
    {
        cue::Result<cue::EntryType> type = query_absolute_entry(a_path, a_context);
        if (!type)
        {
            return cue::Result<void>::failure(std::move(*type.try_error()));
        }
        if (*type.try_value() == cue::EntryType::Directory)
        {
            return cue::Result<void>::success();
        }
        if (*type.try_value() == cue::EntryType::UnsupportedEntry)
        {
            return cue::Result<void>::failure(cue::make_io_error(
                a_context, cue::IoError::UnsupportedEntry, "Known Folder root contains a reparse point"));
        }
        if (*type.try_value() == cue::EntryType::RegularFile)
        {
            return cue::Result<void>::failure(
                cue::make_io_error(a_context, cue::IoError::TypeMismatch, "Known Folder root contains a file"));
        }
        if (a_mode == cue::WindowsRootOpenMode::OpenExisting)
        {
            return cue::Result<void>::failure(
                make_windows_error(a_context, ERROR_PATH_NOT_FOUND, "Known Folder root does not exist"));
        }

        std::wstring path;
        try
        {
            path.assign(a_path);
        }
        catch (...)
        {
            terminate_allocation(a_context);
        }
        cue::Result<std::wstring> extended = make_extended_path(std::move(path), a_context);
        if (!extended)
        {
            return cue::Result<void>::failure(std::move(*extended.try_error()));
        }
        if (CreateDirectoryW(extended.try_value()->c_str(), nullptr) != FALSE)
        {
            return require_absolute_directory(a_path, a_context);
        }

        const DWORD code = GetLastError();
        if (code != ERROR_ALREADY_EXISTS)
        {
            return cue::Result<void>::failure(
                make_windows_error(a_context, code, "Known Folder root creation failed"));
        }

        cue::Result<cue::EntryType> racedType = query_absolute_entry(a_path, a_context);
        if (!racedType)
        {
            return cue::Result<void>::failure(std::move(*racedType.try_error()));
        }
        if (*racedType.try_value() == cue::EntryType::Directory)
        {
            return cue::Result<void>::success();
        }
        if (*racedType.try_value() == cue::EntryType::UnsupportedEntry)
        {
            return cue::Result<void>::failure(cue::make_io_error(
                a_context, cue::IoError::UnsupportedEntry, "Known Folder root was replaced by a reparse point"));
        }
        if (*racedType.try_value() == cue::EntryType::RegularFile)
        {
            return cue::Result<void>::failure(cue::make_io_error(
                a_context, cue::IoError::TypeMismatch, "Known Folder root was replaced by a file"));
        }
    }

    return cue::Result<void>::failure(cue::make_io_error(
        a_context, cue::IoError::IoFailure, "Known Folder root remained missing after creation races",
        static_cast<std::int64_t>(ERROR_ALREADY_EXISTS)));
}

/// @brief Root Handle と相対操作を Win32 API へ閉じ込める Filesystem 実装
class WindowsFilesystemRoot final : public cue::FilesystemRoot
{
  public:
    /// @brief 検証済み Root Path と Native Handle を所有する
    WindowsFilesystemRoot(const cue::AssertContext &a_assertContext, std::wstring &&a_rootPath,
                          UniqueHandle &&a_rootHandle, const BY_HANDLE_FILE_INFORMATION &a_rootInformation) noexcept
        : m_assertContext(&a_assertContext), m_rootPath(std::move(a_rootPath)), m_rootHandle(std::move(a_rootHandle)),
          m_rootVolumeSerial(a_rootInformation.dwVolumeSerialNumber),
          m_rootFileIndexHigh(a_rootInformation.nFileIndexHigh), m_rootFileIndexLow(a_rootInformation.nFileIndexLow)
    {
    }

    /// @brief Native Root の一意所有を保つため Copy 構築を禁止する
    WindowsFilesystemRoot(const WindowsFilesystemRoot &) = delete;
    /// @brief Native Root の一意所有を保つため Copy 代入を禁止する
    WindowsFilesystemRoot &operator=(const WindowsFilesystemRoot &) = delete;
    /// @brief Native Root 所有権を移動する
    WindowsFilesystemRoot(WindowsFilesystemRoot &&) noexcept = default;
    /// @brief Native Root 所有権を移動代入する
    WindowsFilesystemRoot &operator=(WindowsFilesystemRoot &&) noexcept = default;
    /// @brief Root Handle と Staging 追跡 Storage を解放する
    ~WindowsFilesystemRoot() override;

    /// @brief Binding時のWindows VolumeとFile IDをOpaque比較値として返す
    [[nodiscard]] cue::Result<cue::FilesystemIdentity> root_identity() const noexcept override;

    /// @brief Known Folder Factoryが取得した親Chain PinをRoot寿命へ移管する
    void adopt_pinned_directories(std::vector<UniqueHandle> &&a_directories) noexcept
    {
        m_pinnedDirectories = std::move(a_directories);
    }

    /// @brief Entry を Follow せず Portable 種別として返す
    [[nodiscard]] cue::Result<cue::EntryType> query_entry(const cue::RelativePath &a_path) noexcept override;
    /// @brief 上限内の Regular File 全体を所有 Byte 列として返す
    [[nodiscard]] cue::Result<std::vector<std::byte>> read_file(const cue::RelativePath &a_path,
                                                                std::size_t a_maxBytes) noexcept override;
    /// @brief Root 配下に不足する Directory を親から作成する
    [[nodiscard]] cue::Result<void> create_directories(const cue::RelativePath &a_path) noexcept override;
    /// @brief 同一 Directory の Temporary File から Atomic Rename する
    [[nodiscard]] cue::Result<void> write_file_atomic(const cue::RelativePath &a_path,
                                                      std::span<const std::byte> a_bytes) noexcept override;
    /// @brief 利用者Locator外のHidden SiblingへRecovery BackupをAtomic公開する
    [[nodiscard]] cue::Result<void> write_recovery_backup_atomic(
        const cue::RelativePath &a_destination, std::span<const std::byte> a_bytes,
        const cue::AssertContext &a_assertContext) noexcept override;
    /// @brief Destination Sidecarを共有なしHandleとして取得する
    [[nodiscard]] cue::Result<cue::FileWriteLease> acquire_file_write_lease(
        const cue::RelativePath &a_path) noexcept override;
    /// @brief Leaseと期待Fingerprintを検証してからAtomic Publishする
    [[nodiscard]] cue::Result<void> write_file_atomic_if_unchanged(
        cue::FileWriteLease &a_lease, const cue::RelativePath &a_path, cue::FileFingerprint a_expected,
        std::size_t a_maximumExpectedBytes, std::span<const std::byte> a_bytes) noexcept override;
    /// @brief Root内Regular Fileを削除する
    [[nodiscard]] cue::Result<void> remove_file(const cue::RelativePath &a_path) noexcept override;
    /// @brief Destination の Sibling へ Operation 所有 Staging を作成する
    [[nodiscard]] cue::Result<cue::StagingArea> create_staging_area(
        const cue::RelativePath &a_destination) noexcept override;
    /// @brief Token が一致する Staging を既存 Destination へ上書きせず公開する
    [[nodiscard]] cue::Result<void> publish_staging_area(
        cue::StagingArea &&a_staging, const cue::RelativePath &a_destination,
        const cue::StagingPublishAuthorization *a_authorization = nullptr) noexcept override;
    /// @brief Token が一致する未公開 Staging だけを再帰削除する
    [[nodiscard]] cue::Result<void> rollback_staging_area(cue::StagingArea &&a_staging) noexcept override;

  private:
    /// @brief Operation が作成した Staging Directory の Path と Native Identity を保持する
    struct StagingRecord final
    {
        std::string path;
        std::string destinationKey;
        UniqueHandle handle;
        cue::windows_io::NativeFilesystemIdentity identity;
    };

    /// @brief Root Absolute Path と検証済み Relative Path を Extended Windows Path へ結合する
    [[nodiscard]] cue::Result<std::wstring> absolute_path(const cue::RelativePath &a_path) const noexcept;
    /// @brief Relative Path の既存 Component に Reparse Point がないか検証する
    [[nodiscard]] cue::Result<cue::EntryType> validate_entry(const cue::RelativePath &a_path) const noexcept;
    /// @brief Staging Token と Path がこの Root の発行値に一致するか判定する
    [[nodiscard]] bool owns_staging(const cue::StagingArea &a_staging) const noexcept;
    /// @brief Staging Path が Token 発行時と同じ Native Directory Object か検証する
    [[nodiscard]] cue::Result<void> validate_staging_identity(std::uint64_t a_token,
                                                              const cue::RelativePath &a_path) const noexcept;
    /// @brief Root Path が Binding 時と同じ Directory Object を指すか再検証する
    [[nodiscard]] cue::Result<void> verify_root_identity() const noexcept;
    /// @brief Optional Lease／Fingerprint条件を適用するAtomic Write共通経路
    [[nodiscard]] cue::Result<void> write_file_atomic_internal(const cue::RelativePath &a_path,
                                                               std::span<const std::byte> a_bytes,
                                                               cue::FileWriteLease *a_lease,
                                                               const cue::FileFingerprint *a_expected,
                                                               std::size_t a_maximumExpectedBytes,
                                                               const std::wstring *a_destinationOverride) noexcept;

    const cue::AssertContext *m_assertContext;
    std::wstring m_rootPath;
    std::vector<UniqueHandle> m_pinnedDirectories;
    UniqueHandle m_rootHandle;
    std::unordered_map<std::uint64_t, StagingRecord> m_stagingPaths;
    DWORD m_rootVolumeSerial;
    DWORD m_rootFileIndexHigh;
    DWORD m_rootFileIndexLow;
    std::uint64_t m_nextToken = 1;
};

WindowsFilesystemRoot::~WindowsFilesystemRoot()
{
    for (const auto &[token, record] : m_stagingPaths)
    {
        cue::Result<cue::RelativePath> path = cue::RelativePath::parse(record.path, *m_assertContext);
        if (path && validate_staging_identity(token, *path.try_value()))
        {
            cue::Result<std::wstring> full = absolute_path(*path.try_value());
            if (full)
            {
                static_cast<void>(remove_tree(*full.try_value(), *m_assertContext));
            }
        }
    }
}

cue::Result<cue::FilesystemIdentity> WindowsFilesystemRoot::root_identity() const noexcept
{
    cue::Result<void> verified = verify_root_identity();
    if (!verified)
    {
        return cue::Result<cue::FilesystemIdentity>::failure(std::move(*verified.try_error()));
    }

    cue::Result<cue::windows_io::NativeFilesystemIdentity> identity =
        cue::windows_io::inspect_native_filesystem_identity(m_rootHandle.get(), *m_assertContext);
    if (!identity)
    {
        return cue::Result<cue::FilesystemIdentity>::failure(std::move(*identity.try_error()));
    }

    constexpr std::uint64_t k_windowsIdentityProvider = 0x57494E3200000000ULL;
    return cue::Result<cue::FilesystemIdentity>::success(make_filesystem_identity(
        k_windowsIdentityProvider, identity.try_value()->volumeHigh, identity.try_value()->volumeLow,
        identity.try_value()->entryHigh, identity.try_value()->entryLow));
}

cue::Result<void> WindowsFilesystemRoot::verify_root_identity() const noexcept
{
    UniqueHandle current(CreateFileW(m_rootPath.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                     FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!current.is_valid())
    {
        return cue::Result<void>::failure(cue::make_io_error(
            *m_assertContext, cue::IoError::OutsideRoot, "Filesystem root path no longer resolves to the bound root",
            static_cast<std::int64_t>(GetLastError())));
    }

    BY_HANDLE_FILE_INFORMATION information{};
    if (GetFileInformationByHandle(current.get(), &information) == FALSE)
    {
        return cue::Result<void>::failure(cue::make_io_error(*m_assertContext, cue::IoError::OutsideRoot,
                                                             "Filesystem root identity could not be verified",
                                                             static_cast<std::int64_t>(GetLastError())));
    }

    if ((information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        information.dwVolumeSerialNumber != m_rootVolumeSerial || information.nFileIndexHigh != m_rootFileIndexHigh ||
        information.nFileIndexLow != m_rootFileIndexLow)
    {
        return cue::Result<void>::failure(
            cue::make_io_error(*m_assertContext, cue::IoError::OutsideRoot, "Filesystem root identity changed"));
    }
    return cue::Result<void>::success();
}

cue::Result<std::wstring> WindowsFilesystemRoot::absolute_path(const cue::RelativePath &a_path) const noexcept
{
    cue::Result<void> identity = verify_root_identity();
    if (!identity)
    {
        return cue::Result<std::wstring>::failure(std::move(*identity.try_error()));
    }

    cue::Result<std::wstring> converted = to_utf16(a_path.text(), *m_assertContext);
    if (!converted)
    {
        return cue::Result<std::wstring>::failure(std::move(*converted.try_error()));
    }

    std::wstring relative = std::move(*converted.try_value());
    for (wchar_t &character : relative)
    {
        if (character == L'/')
        {
            character = L'\\';
        }
    }

    std::wstring result;
    try
    {
        result.reserve(m_rootPath.size() + 1 + relative.size());
        result = m_rootPath;
        result.push_back(L'\\');
        result.append(relative);
    }
    catch (...)
    {
        terminate_allocation(*m_assertContext);
    }

    if (result.size() >= k_maxWindowsPathLength)
    {
        return cue::Result<std::wstring>::failure(
            cue::make_io_error(*m_assertContext, cue::IoError::CapacityExceeded, "Windows path length exceeds limit"));
    }
    return cue::Result<std::wstring>::success(std::move(result));
}

cue::Result<cue::EntryType> WindowsFilesystemRoot::validate_entry(const cue::RelativePath &a_path) const noexcept
{
    cue::Result<std::wstring> fullResult = absolute_path(a_path);
    if (!fullResult)
    {
        return cue::Result<cue::EntryType>::failure(std::move(*fullResult.try_error()));
    }
    const std::wstring &full = *fullResult.try_value();

    std::size_t componentStart = m_rootPath.size() + 1;
    while (componentStart <= full.size())
    {
        const std::size_t separator = full.find(L'\\', componentStart);
        const std::size_t componentEnd = separator == std::wstring::npos ? full.size() : separator;
        std::wstring current;
        try
        {
            current.assign(full.data(), componentEnd);
        }
        catch (...)
        {
            terminate_allocation(*m_assertContext);
        }

        const DWORD attributes = GetFileAttributesW(current.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES)
        {
            const DWORD code = GetLastError();
            if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND)
            {
                return cue::Result<cue::EntryType>::success(cue::EntryType::Missing);
            }
            return cue::Result<cue::EntryType>::failure(
                make_windows_error(*m_assertContext, code, "Windows filesystem entry inspection failed"));
        }
        if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
        {
            return cue::Result<cue::EntryType>::success(cue::EntryType::UnsupportedEntry);
        }
        if (separator == std::wstring::npos)
        {
            if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
            {
                return cue::Result<cue::EntryType>::success(cue::EntryType::Directory);
            }
            return cue::Result<cue::EntryType>::success(cue::EntryType::RegularFile);
        }
        if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
        {
            return cue::Result<cue::EntryType>::failure(
                cue::make_io_error(*m_assertContext, cue::IoError::TypeMismatch, "Path parent is not a directory"));
        }
        componentStart = separator + 1;
    }
    return cue::Result<cue::EntryType>::success(cue::EntryType::Missing);
}

cue::Result<cue::EntryType> WindowsFilesystemRoot::query_entry(const cue::RelativePath &a_path) noexcept
{
    return validate_entry(a_path);
}

cue::Result<std::vector<std::byte>> WindowsFilesystemRoot::read_file(const cue::RelativePath &a_path,
                                                                     std::size_t a_maxBytes) noexcept
{
    cue::Result<cue::EntryType> type = validate_entry(a_path);
    if (!type)
    {
        return cue::Result<std::vector<std::byte>>::failure(std::move(*type.try_error()));
    }
    if (*type.try_value() == cue::EntryType::Missing)
    {
        return cue::Result<std::vector<std::byte>>::failure(
            cue::make_io_error(*m_assertContext, cue::IoError::NotFound, "File does not exist"));
    }
    if (*type.try_value() != cue::EntryType::RegularFile)
    {
        const cue::IoError code = *type.try_value() == cue::EntryType::UnsupportedEntry ? cue::IoError::UnsupportedEntry
                                                                                        : cue::IoError::TypeMismatch;
        return cue::Result<std::vector<std::byte>>::failure(
            cue::make_io_error(*m_assertContext, code, "Entry is not a readable regular file"));
    }

    cue::Result<std::wstring> fullResult = absolute_path(a_path);
    if (!fullResult)
    {
        return cue::Result<std::vector<std::byte>>::failure(std::move(*fullResult.try_error()));
    }
    UniqueHandle file(CreateFileW(fullResult.try_value()->c_str(), GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!file.is_valid())
    {
        return cue::Result<std::vector<std::byte>>::failure(
            make_windows_error(*m_assertContext, GetLastError(), "File open failed"));
    }

    LARGE_INTEGER size{};
    if (GetFileSizeEx(file.get(), &size) == FALSE)
    {
        return cue::Result<std::vector<std::byte>>::failure(
            make_windows_error(*m_assertContext, GetLastError(), "File size query failed"));
    }
    if (size.QuadPart < 0 || static_cast<unsigned long long>(size.QuadPart) > a_maxBytes ||
        static_cast<unsigned long long>(size.QuadPart) > (std::numeric_limits<std::size_t>::max)())
    {
        return cue::Result<std::vector<std::byte>>::failure(
            cue::make_io_error(*m_assertContext, cue::IoError::CapacityExceeded, "File exceeds read limit"));
    }

    std::vector<std::byte> bytes;
    try
    {
        bytes.resize(static_cast<std::size_t>(size.QuadPart));
    }
    catch (...)
    {
        terminate_allocation(*m_assertContext);
    }

    std::size_t offset = 0;
    while (offset < bytes.size())
    {
        const DWORD request = static_cast<DWORD>(
            (std::min)(bytes.size() - offset, static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
        DWORD read = 0;
        if (ReadFile(file.get(), bytes.data() + offset, request, &read, nullptr) == FALSE)
        {
            return cue::Result<std::vector<std::byte>>::failure(
                make_windows_error(*m_assertContext, GetLastError(), "File read failed"));
        }
        if (read == 0)
        {
            return cue::Result<std::vector<std::byte>>::failure(
                cue::make_io_error(*m_assertContext, cue::IoError::IoFailure, "File read ended before expected size"));
        }
        offset += read;
    }
    return cue::Result<std::vector<std::byte>>::success(std::move(bytes));
}

cue::Result<void> WindowsFilesystemRoot::create_directories(const cue::RelativePath &a_path) noexcept
{
    std::size_t begin = 0;
    while (begin < a_path.text().size())
    {
        const std::size_t separator = a_path.text().find('/', begin);
        const std::size_t end = separator == std::string_view::npos ? a_path.text().size() : separator;
        cue::Result<cue::RelativePath> prefix =
            cue::RelativePath::parse(a_path.text().substr(0, end), *m_assertContext);
        if (!prefix)
        {
            return cue::Result<void>::failure(std::move(*prefix.try_error()));
        }
        cue::Result<cue::EntryType> type = validate_entry(*prefix.try_value());
        if (!type)
        {
            return cue::Result<void>::failure(std::move(*type.try_error()));
        }
        if (*type.try_value() == cue::EntryType::Missing)
        {
            cue::Result<std::wstring> full = absolute_path(*prefix.try_value());
            if (!full)
            {
                return cue::Result<void>::failure(std::move(*full.try_error()));
            }
            constexpr std::size_t k_maxCreateAttempts = 3;
            bool wasCreated = false;
            for (std::size_t attempt = 0; attempt < k_maxCreateAttempts; ++attempt)
            {
                if (CreateDirectoryW(full.try_value()->c_str(), nullptr) != FALSE)
                {
                    wasCreated = true;
                    break;
                }

                const DWORD code = GetLastError();
                if (code != ERROR_ALREADY_EXISTS)
                {
                    return cue::Result<void>::failure(
                        make_windows_error(*m_assertContext, code, "Directory creation failed"));
                }

                cue::Result<cue::EntryType> racedType = validate_entry(*prefix.try_value());
                if (!racedType)
                {
                    return cue::Result<void>::failure(std::move(*racedType.try_error()));
                }
                if (*racedType.try_value() == cue::EntryType::Directory)
                {
                    wasCreated = true;
                    break;
                }
                if (*racedType.try_value() == cue::EntryType::RegularFile)
                {
                    return cue::Result<void>::failure(cue::make_io_error(
                        *m_assertContext, cue::IoError::TypeMismatch,
                        "Directory path was occupied by a file during creation"));
                }
                if (*racedType.try_value() == cue::EntryType::UnsupportedEntry)
                {
                    return cue::Result<void>::failure(cue::make_io_error(
                        *m_assertContext, cue::IoError::UnsupportedEntry,
                        "Directory path was occupied by an unsupported entry during creation"));
                }
            }
            if (!wasCreated)
            {
                return cue::Result<void>::failure(cue::make_io_error(
                    *m_assertContext, cue::IoError::IoFailure,
                    "Directory path remained missing after creation races",
                    static_cast<std::int64_t>(ERROR_ALREADY_EXISTS)));
            }
        }
        else if (*type.try_value() != cue::EntryType::Directory)
        {
            const cue::IoError code = *type.try_value() == cue::EntryType::UnsupportedEntry
                                          ? cue::IoError::UnsupportedEntry
                                          : cue::IoError::TypeMismatch;
            return cue::Result<void>::failure(
                cue::make_io_error(*m_assertContext, code, "Directory path contains an unsupported entry"));
        }
        if (separator == std::string_view::npos)
        {
            break;
        }
        begin = separator + 1;
    }
    return cue::Result<void>::success();
}

cue::Result<void> WindowsFilesystemRoot::write_file_atomic(const cue::RelativePath &a_path,
                                                           std::span<const std::byte> a_bytes) noexcept
{
    return write_file_atomic_internal(a_path, a_bytes, nullptr, nullptr, 0U, nullptr);
}

cue::Result<void> WindowsFilesystemRoot::write_recovery_backup_atomic(
    const cue::RelativePath &a_destination, std::span<const std::byte> a_bytes,
    const cue::AssertContext &) noexcept
{
    auto destination = absolute_path(a_destination);
    if (!destination)
    {
        return cue::Result<void>::failure(std::move(*destination.try_error()));
    }
    try
    {
        const std::size_t separator = destination.try_value()->find_last_of(L'\\');
        if (separator == std::wstring::npos)
        {
            return cue::Result<void>::failure(
                cue::make_io_error(*m_assertContext, cue::IoError::InvalidPath, "Backup destination has no parent"));
        }
        destination.try_value()->insert(separator + 1U, L".CueBackup-");
    }
    catch (...)
    {
        terminate_allocation(*m_assertContext);
    }
    if (destination.try_value()->size() >= k_maxWindowsPathLength)
    {
        return cue::Result<void>::failure(
            cue::make_io_error(*m_assertContext, cue::IoError::CapacityExceeded, "Recovery backup path is too long"));
    }
    return write_file_atomic_internal(a_destination, a_bytes, nullptr, nullptr, 0U, &*destination.try_value());
}

cue::Result<cue::FileWriteLease> WindowsFilesystemRoot::acquire_file_write_lease(
    const cue::RelativePath &a_path) noexcept
{
    cue::Result<cue::EntryType> destinationType = validate_entry(a_path);
    if (!destinationType)
    {
        return cue::Result<cue::FileWriteLease>::failure(std::move(*destinationType.try_error()));
    }
    if (*destinationType.try_value() != cue::EntryType::Missing &&
        *destinationType.try_value() != cue::EntryType::RegularFile)
    {
        const cue::IoError code = *destinationType.try_value() == cue::EntryType::UnsupportedEntry
                                      ? cue::IoError::UnsupportedEntry
                                      : cue::IoError::TypeMismatch;
        return cue::Result<cue::FileWriteLease>::failure(
            cue::make_io_error(*m_assertContext, code, "Write lease destination is unsupported"));
    }

    try
    {
        std::string exactPathKey = a_path.comparison_key(*m_assertContext);
        std::string familyPathKey = write_lease_path_key(a_path, *m_assertContext);
        const std::uint64_t familyDigest =
            cue::file_content_digest(std::as_bytes(std::span(familyPathKey.data(), familyPathKey.size())));
        std::array<wchar_t, 160> mutexName{};
        const int mutexNameLength = _snwprintf_s(
            mutexName.data(), mutexName.size(), _TRUNCATE, L"Global\\CueEngine.WriteLease.%08lx.%08lx%08lx.%016llx",
            static_cast<unsigned long>(m_rootVolumeSerial), static_cast<unsigned long>(m_rootFileIndexHigh),
            static_cast<unsigned long>(m_rootFileIndexLow), static_cast<unsigned long long>(familyDigest));
        if (mutexNameLength < 0)
        {
            return cue::Result<cue::FileWriteLease>::failure(cue::make_io_error(
                *m_assertContext, cue::IoError::CapacityExceeded, "Scene write lease mutex name exceeds limit"));
        }

        UniqueHandle mutexHandle(CreateMutexW(nullptr, FALSE, mutexName.data()));
        if (!mutexHandle.is_valid())
        {
            return cue::Result<cue::FileWriteLease>::failure(
                make_windows_error(*m_assertContext, GetLastError(), "Scene write lease mutex creation failed"));
        }
        const DWORD waitResult = WaitForSingleObject(mutexHandle.get(), 0U);
        if (waitResult == WAIT_TIMEOUT)
        {
            return cue::Result<cue::FileWriteLease>::failure(
                cue::make_io_error(*m_assertContext, cue::IoError::Busy, "Scene write lease is already held"));
        }
        if (waitResult != WAIT_OBJECT_0 && waitResult != WAIT_ABANDONED)
        {
            const DWORD code = waitResult == WAIT_FAILED ? GetLastError() : ERROR_INVALID_HANDLE;
            return cue::Result<cue::FileWriteLease>::failure(
                make_windows_error(*m_assertContext, code, "Scene write lease mutex acquisition failed"));
        }
        const DWORD ownerThreadId = GetCurrentThreadId();
        std::wstring mutexNameText(mutexName.data());
        if (!register_write_lease(mutexNameText, ownerThreadId))
        {
            ReleaseMutex(mutexHandle.get());
            return cue::Result<cue::FileWriteLease>::failure(
                cue::make_io_error(*m_assertContext, cue::IoError::Busy, "Scene write lease is already held"));
        }

        auto state = std::make_unique<WindowsFileWriteLeaseState>(
            this, std::move(exactPathKey), std::move(familyPathKey), std::move(mutexNameText), ownerThreadId,
            *m_assertContext, std::move(mutexHandle));

        if (*destinationType.try_value() == cue::EntryType::RegularFile)
        {
            cue::Result<std::wstring> destination = absolute_path(a_path);
            if (!destination)
            {
                return cue::Result<cue::FileWriteLease>::failure(std::move(*destination.try_error()));
            }
            UniqueHandle file(CreateFileW(destination.try_value()->c_str(), FILE_READ_ATTRIBUTES,
                                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                                          nullptr));
            if (!file.is_valid())
            {
                return cue::Result<cue::FileWriteLease>::failure(
                    make_windows_error(*m_assertContext, GetLastError(), "Scene link count inspection failed"));
            }
            BY_HANDLE_FILE_INFORMATION information{};
            if (GetFileInformationByHandle(file.get(), &information) == FALSE)
            {
                return cue::Result<cue::FileWriteLease>::failure(
                    make_windows_error(*m_assertContext, GetLastError(), "Scene link count query failed"));
            }
            if (information.nNumberOfLinks != 1U)
            {
                return cue::Result<cue::FileWriteLease>::failure(cue::make_io_error(
                    *m_assertContext, cue::IoError::UnsupportedEntry, "Hard-linked scene files cannot be replaced"));
            }
        }

        return cue::Result<cue::FileWriteLease>::success(make_file_write_lease(std::move(state)));
    }
    catch (...)
    {
        terminate_allocation(*m_assertContext);
    }
}

cue::Result<void> WindowsFilesystemRoot::write_file_atomic_if_unchanged(cue::FileWriteLease &a_lease,
                                                                        const cue::RelativePath &a_path,
                                                                        cue::FileFingerprint a_expected,
                                                                        std::size_t a_maximumExpectedBytes,
                                                                        std::span<const std::byte> a_bytes) noexcept
{
    return write_file_atomic_internal(a_path, a_bytes, &a_lease, &a_expected, a_maximumExpectedBytes, nullptr);
}

cue::Result<void> WindowsFilesystemRoot::remove_file(const cue::RelativePath &a_path) noexcept
{
    cue::Result<cue::EntryType> type = validate_entry(a_path);
    if (!type)
    {
        return cue::Result<void>::failure(std::move(*type.try_error()));
    }
    if (*type.try_value() == cue::EntryType::Missing)
    {
        return cue::Result<void>::success();
    }
    if (*type.try_value() != cue::EntryType::RegularFile)
    {
        const cue::IoError code = *type.try_value() == cue::EntryType::UnsupportedEntry ? cue::IoError::UnsupportedEntry
                                                                                        : cue::IoError::TypeMismatch;
        return cue::Result<void>::failure(cue::make_io_error(*m_assertContext, code, "Entry is not a removable file"));
    }
    cue::Result<std::wstring> full = absolute_path(a_path);
    if (!full)
    {
        return cue::Result<void>::failure(std::move(*full.try_error()));
    }
    if (DeleteFileW(full.try_value()->c_str()) == FALSE)
    {
        const DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND)
        {
            return cue::Result<void>::success();
        }
        return cue::Result<void>::failure(make_windows_error(*m_assertContext, code, "File removal failed"));
    }
    return cue::Result<void>::success();
}

cue::Result<void> WindowsFilesystemRoot::write_file_atomic_internal(const cue::RelativePath &a_path,
                                                                    std::span<const std::byte> a_bytes,
                                                                    cue::FileWriteLease *a_lease,
                                                                    const cue::FileFingerprint *a_expected,
                                                                    std::size_t a_maximumExpectedBytes,
                                                                    const std::wstring *a_destinationOverride) noexcept
{
    if ((a_lease == nullptr) != (a_expected == nullptr))
    {
        return cue::Result<void>::failure(cue::make_io_error(*m_assertContext, cue::IoError::PreconditionFailed,
                                                             "Atomic write condition is invalid"));
    }
    if (a_lease != nullptr)
    {
        auto *state = dynamic_cast<WindowsFileWriteLeaseState *>(file_write_lease_state(*a_lease));
        if (state == nullptr || state->owner != this || state->ownerThreadId != GetCurrentThreadId() ||
            state->exactPathKey != a_path.comparison_key(*m_assertContext) ||
            state->familyPathKey != write_lease_path_key(a_path, *m_assertContext) || !state->handle.is_valid())
        {
            return cue::Result<void>::failure(cue::make_io_error(*m_assertContext, cue::IoError::PreconditionFailed,
                                                                 "Atomic write lease does not own destination"));
        }
    }
    const std::string_view parent = parent_text(a_path.text());
    if (!parent.empty())
    {
        cue::Result<cue::RelativePath> parentPath = cue::RelativePath::parse(parent, *m_assertContext);
        if (!parentPath)
        {
            return cue::Result<void>::failure(std::move(*parentPath.try_error()));
        }
        cue::Result<cue::EntryType> parentType = validate_entry(*parentPath.try_value());
        if (!parentType)
        {
            return cue::Result<void>::failure(std::move(*parentType.try_error()));
        }
        if (*parentType.try_value() != cue::EntryType::Directory)
        {
            cue::IoError code = cue::IoError::TypeMismatch;
            if (*parentType.try_value() == cue::EntryType::Missing)
            {
                code = cue::IoError::NotFound;
            }
            else if (*parentType.try_value() == cue::EntryType::UnsupportedEntry)
            {
                code = cue::IoError::UnsupportedEntry;
            }
            return cue::Result<void>::failure(
                cue::make_io_error(*m_assertContext, code, "Atomic file parent is unavailable"));
        }
    }

    cue::Result<std::wstring> resolvedDestination = absolute_path(a_path);
    if (!resolvedDestination)
    {
        return cue::Result<void>::failure(std::move(*resolvedDestination.try_error()));
    }
    const std::wstring &destination =
        a_destinationOverride == nullptr ? *resolvedDestination.try_value() : *a_destinationOverride;

    std::wstring temporaryPath;
    UniqueHandle temporary;
    for (std::size_t attempt = 0; attempt < 8; ++attempt)
    {
        cue::Result<std::string> random = make_random_hex(*m_assertContext);
        if (!random)
        {
            return cue::Result<void>::failure(std::move(*random.try_error()));
        }
        const std::string name = "CueTemp-" + *random.try_value() + ".tmp";
        const std::size_t separator = destination.find_last_of(L'\\');
        if (separator == std::wstring::npos)
        {
            return cue::Result<void>::failure(
                cue::make_io_error(*m_assertContext, cue::IoError::InvalidPath, "Atomic destination has no parent"));
        }
        try
        {
            temporaryPath.assign(destination, 0U, separator + 1U);
            temporaryPath.append(name.begin(), name.end());
        }
        catch (...)
        {
            terminate_allocation(*m_assertContext);
        }
        if (temporaryPath.size() >= k_maxWindowsPathLength)
        {
            return cue::Result<void>::failure(
                cue::make_io_error(*m_assertContext, cue::IoError::CapacityExceeded, "Temporary path is too long"));
        }
        temporary.reset(CreateFileW(temporaryPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                    FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (temporary.is_valid())
        {
            break;
        }
        if (GetLastError() != ERROR_FILE_EXISTS)
        {
            return cue::Result<void>::failure(
                make_windows_error(*m_assertContext, GetLastError(), "Temporary file creation failed"));
        }
    }
    if (!temporary.is_valid())
    {
        return cue::Result<void>::failure(cue::make_io_error(*m_assertContext, cue::IoError::AlreadyExists,
                                                             "Temporary file name attempts were exhausted"));
    }

    std::size_t offset = 0;
    while (offset < a_bytes.size())
    {
        const DWORD request = static_cast<DWORD>(
            (std::min)(a_bytes.size() - offset, static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
        DWORD written = 0;
        if (WriteFile(temporary.get(), a_bytes.data() + offset, request, &written, nullptr) == FALSE || written == 0)
        {
            const DWORD nativeCode = GetLastError();
            const DWORD code = nativeCode == ERROR_SUCCESS ? ERROR_WRITE_FAULT : nativeCode;
            temporary.reset();
            cue::Error primary = make_windows_error(*m_assertContext, code, "Temporary file write failed");
            return cue::Result<void>::failure(
                remove_temporary_after_failure(*m_assertContext, temporaryPath, std::move(primary)));
        }
        offset += written;
    }
    if (FlushFileBuffers(temporary.get()) == FALSE)
    {
        const DWORD code = GetLastError();
        temporary.reset();
        cue::Error primary = make_windows_error(*m_assertContext, code, "Temporary file flush failed");
        return cue::Result<void>::failure(
            remove_temporary_after_failure(*m_assertContext, temporaryPath, std::move(primary)));
    }
    temporary.reset();

    cue::EntryType destinationTypeValue = cue::EntryType::Missing;
    if (a_destinationOverride == nullptr)
    {
        cue::Result<cue::EntryType> destinationType = validate_entry(a_path);
        if (!destinationType)
        {
            return cue::Result<void>::failure(remove_temporary_after_failure(
                *m_assertContext, temporaryPath, std::move(*destinationType.try_error())));
        }
        destinationTypeValue = *destinationType.try_value();
    }
    else
    {
        const DWORD attributes = GetFileAttributesW(destination.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES)
        {
            const DWORD code = GetLastError();
            if (code != ERROR_FILE_NOT_FOUND && code != ERROR_PATH_NOT_FOUND)
            {
                cue::Error primary = make_windows_error(*m_assertContext, code, "Recovery backup inspection failed");
                return cue::Result<void>::failure(
                    remove_temporary_after_failure(*m_assertContext, temporaryPath, std::move(primary)));
            }
        }
        else if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
        {
            destinationTypeValue = cue::EntryType::UnsupportedEntry;
        }
        else if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        {
            destinationTypeValue = cue::EntryType::Directory;
        }
        else
        {
            destinationTypeValue = cue::EntryType::RegularFile;
        }
    }
    if (destinationTypeValue == cue::EntryType::Directory || destinationTypeValue == cue::EntryType::UnsupportedEntry)
    {
        const cue::IoError code = destinationTypeValue == cue::EntryType::UnsupportedEntry
                                      ? cue::IoError::UnsupportedEntry
                                      : cue::IoError::TypeMismatch;
        cue::Error primary = cue::make_io_error(*m_assertContext, code, "Atomic destination is not a regular file");
        return cue::Result<void>::failure(
            remove_temporary_after_failure(*m_assertContext, temporaryPath, std::move(primary)));
    }
    if (a_expected != nullptr)
    {
        cue::Result<cue::FileFingerprint> current =
            cue::fingerprint_file(*this, a_path, a_maximumExpectedBytes, *m_assertContext);
        if (!current)
        {
            cue::Error primary = current.try_error()->root_code().domain() == "Cue.IO" &&
                                         current.try_error()->root_code().value() ==
                                             static_cast<std::int64_t>(cue::IoError::CapacityExceeded)
                                     ? cue::make_io_error(*m_assertContext, cue::IoError::PreconditionFailed,
                                                          "Atomic destination exceeds the expected content limit")
                                     : std::move(*current.try_error());
            return cue::Result<void>::failure(
                remove_temporary_after_failure(*m_assertContext, temporaryPath, std::move(primary)));
        }
        if (*current.try_value() != *a_expected)
        {
            cue::Error primary = cue::make_io_error(*m_assertContext, cue::IoError::PreconditionFailed,
                                                    "Atomic destination changed before publish");
            return cue::Result<void>::failure(
                remove_temporary_after_failure(*m_assertContext, temporaryPath, std::move(primary)));
        }
    }

    DWORD flags = 0;
    if (destinationTypeValue == cue::EntryType::RegularFile)
    {
        flags |= MOVEFILE_REPLACE_EXISTING;
    }
    const NativePublishOutcome publish = publish_with_durability(temporaryPath, destination, flags);
    if (!publish.isPublished)
    {
        cue::Error primary = make_windows_error(*m_assertContext, publish.nativeCode, "Atomic file publish failed");
        return cue::Result<void>::failure(
            remove_temporary_after_failure(*m_assertContext, temporaryPath, std::move(primary)));
    }
    if (publish.nativeCode != ERROR_SUCCESS)
    {
        return cue::Result<void>::failure(cue::make_io_error(
            *m_assertContext, cue::IoError::DurabilityUnknown,
            "Atomic file is published but Windows durability confirmation failed", publish.nativeCode));
    }
    return cue::Result<void>::success();
}

cue::Result<cue::StagingArea> WindowsFilesystemRoot::create_staging_area(
    const cue::RelativePath &a_destination) noexcept
{
    cue::Result<cue::EntryType> destinationType = validate_entry(a_destination);
    if (!destinationType)
    {
        return cue::Result<cue::StagingArea>::failure(std::move(*destinationType.try_error()));
    }
    if (*destinationType.try_value() != cue::EntryType::Missing)
    {
        return cue::Result<cue::StagingArea>::failure(
            cue::make_io_error(*m_assertContext, cue::IoError::AlreadyExists, "Staging destination already exists"));
    }

    const std::string_view parent = parent_text(a_destination.text());
    for (std::size_t attempt = 0; attempt < 8; ++attempt)
    {
        cue::Result<std::string> random = make_random_hex(*m_assertContext);
        if (!random)
        {
            return cue::Result<cue::StagingArea>::failure(std::move(*random.try_error()));
        }
        const std::string relativeText = join_relative(parent, "CueStaging-" + *random.try_value(), *m_assertContext);
        cue::Result<cue::RelativePath> relative = cue::RelativePath::parse(relativeText, *m_assertContext);
        if (!relative)
        {
            return cue::Result<cue::StagingArea>::failure(std::move(*relative.try_error()));
        }
        cue::Result<std::wstring> full = absolute_path(*relative.try_value());
        if (!full)
        {
            return cue::Result<cue::StagingArea>::failure(std::move(*full.try_error()));
        }
        if (CreateDirectoryW(full.try_value()->c_str(), nullptr) != FALSE)
        {
            UniqueHandle stagingHandle(
                CreateFileW(full.try_value()->c_str(), DELETE | FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_WRITE_THROUGH,
                            nullptr));
            if (!stagingHandle.is_valid())
            {
                const DWORD code = GetLastError();
                cue::Error primary =
                    make_windows_error(*m_assertContext, code, "Staging directory identity open failed");
                return cue::Result<cue::StagingArea>::failure(
                    remove_staging_after_creation_failure(*m_assertContext, *full.try_value(), std::move(primary)));
            }
            BY_HANDLE_FILE_INFORMATION information{};
            if (GetFileInformationByHandle(stagingHandle.get(), &information) == FALSE)
            {
                const DWORD code = GetLastError();
                stagingHandle.reset();
                cue::Error primary =
                    make_windows_error(*m_assertContext, code, "Staging directory identity query failed");
                return cue::Result<cue::StagingArea>::failure(
                    remove_staging_after_creation_failure(*m_assertContext, *full.try_value(), std::move(primary)));
            }
            cue::Result<cue::windows_io::NativeFilesystemIdentity> nativeIdentity =
                cue::windows_io::inspect_native_filesystem_identity(stagingHandle.get(), *m_assertContext);
            if (!nativeIdentity)
            {
                stagingHandle.reset();
                return cue::Result<cue::StagingArea>::failure(remove_staging_after_creation_failure(
                    *m_assertContext, *full.try_value(), std::move(*nativeIdentity.try_error())));
            }
            std::uint64_t token = m_nextToken++;
            if (token == 0)
            {
                token = m_nextToken++;
            }
            try
            {
                m_stagingPaths.emplace(token,
                                       StagingRecord{relativeText, a_destination.comparison_key(*m_assertContext),
                                                     std::move(stagingHandle), *nativeIdentity.try_value()});
            }
            catch (...)
            {
                RemoveDirectoryW(full.try_value()->c_str());
                terminate_allocation(*m_assertContext);
            }
            return cue::Result<cue::StagingArea>::success(make_staging_area(std::move(*relative.try_value()), token));
        }
        if (GetLastError() != ERROR_ALREADY_EXISTS)
        {
            return cue::Result<cue::StagingArea>::failure(
                make_windows_error(*m_assertContext, GetLastError(), "Staging directory creation failed"));
        }
    }
    return cue::Result<cue::StagingArea>::failure(cue::make_io_error(*m_assertContext, cue::IoError::AlreadyExists,
                                                                     "Staging directory name attempts were exhausted"));
}

bool WindowsFilesystemRoot::owns_staging(const cue::StagingArea &a_staging) const noexcept
{
    const auto found = m_stagingPaths.find(staging_token(a_staging));
    return found != m_stagingPaths.end() && found->second.path == a_staging.path().text();
}

cue::Result<void> WindowsFilesystemRoot::validate_staging_identity(std::uint64_t a_token,
                                                                   const cue::RelativePath &a_path) const noexcept
{
    const auto found = m_stagingPaths.find(a_token);
    if (found == m_stagingPaths.end() || found->second.path != a_path.text() || !found->second.handle.is_valid())
    {
        return cue::Result<void>::failure(
            cue::make_io_error(*m_assertContext, cue::IoError::OutsideRoot, "Staging ownership token is invalid"));
    }

    cue::Result<std::wstring> full = absolute_path(a_path);
    if (!full)
    {
        return cue::Result<void>::failure(std::move(*full.try_error()));
    }
    UniqueHandle current(CreateFileW(full.try_value()->c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                     FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!current.is_valid())
    {
        return cue::Result<void>::failure(cue::make_io_error(*m_assertContext, cue::IoError::OutsideRoot,
                                                             "Staging directory no longer resolves to the owned object",
                                                             static_cast<std::int64_t>(GetLastError())));
    }
    BY_HANDLE_FILE_INFORMATION information{};
    if (GetFileInformationByHandle(current.get(), &information) == FALSE)
    {
        return cue::Result<void>::failure(cue::make_io_error(*m_assertContext, cue::IoError::OutsideRoot,
                                                             "Staging directory identity could not be verified",
                                                             static_cast<std::int64_t>(GetLastError())));
    }
    if ((information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
    {
        return cue::Result<void>::failure(
            cue::make_io_error(*m_assertContext, cue::IoError::UnsupportedEntry, "Staging root is a reparse point"));
    }
    cue::Result<cue::windows_io::NativeFilesystemIdentity> nativeIdentity =
        cue::windows_io::inspect_native_filesystem_identity(current.get(), *m_assertContext);
    if (!nativeIdentity)
    {
        return cue::Result<void>::failure(std::move(*nativeIdentity.try_error()));
    }
    const cue::windows_io::NativeFilesystemIdentity &expected = found->second.identity;
    if ((information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        nativeIdentity.try_value()->volumeHigh != expected.volumeHigh ||
        nativeIdentity.try_value()->volumeLow != expected.volumeLow ||
        nativeIdentity.try_value()->entryHigh != expected.entryHigh ||
        nativeIdentity.try_value()->entryLow != expected.entryLow)
    {
        return cue::Result<void>::failure(
            cue::make_io_error(*m_assertContext, cue::IoError::OutsideRoot, "Staging directory identity changed"));
    }
    return cue::Result<void>::success();
}

cue::Result<void> WindowsFilesystemRoot::publish_staging_area(
    cue::StagingArea &&a_staging, const cue::RelativePath &a_destination,
    const cue::StagingPublishAuthorization *a_authorization) noexcept
{
    if (!owns_staging(a_staging))
    {
        return cue::Result<void>::failure(
            cue::make_io_error(*m_assertContext, cue::IoError::OutsideRoot, "Staging ownership token is invalid"));
    }
    const auto record = m_stagingPaths.find(staging_token(a_staging));
    if (record->second.destinationKey != a_destination.comparison_key(*m_assertContext))
    {
        return cue::Result<void>::failure(cue::make_io_error(
            *m_assertContext, cue::IoError::OutsideRoot, "Staging token does not belong to the publish destination"));
    }
    cue::Result<void> identity = validate_staging_identity(staging_token(a_staging), a_staging.path());
    if (!identity)
    {
        return identity;
    }
    cue::Result<cue::EntryType> destinationType = validate_entry(a_destination);
    if (!destinationType)
    {
        return cue::Result<void>::failure(std::move(*destinationType.try_error()));
    }
    if (*destinationType.try_value() != cue::EntryType::Missing)
    {
        return cue::Result<void>::failure(
            cue::make_io_error(*m_assertContext, cue::IoError::AlreadyExists, "Publish destination already exists"));
    }
    cue::Result<std::wstring> stagingPath = absolute_path(a_staging.path());
    cue::Result<std::wstring> destinationPath = absolute_path(a_destination);
    if (!stagingPath)
    {
        return cue::Result<void>::failure(std::move(*stagingPath.try_error()));
    }
    if (!destinationPath)
    {
        return cue::Result<void>::failure(std::move(*destinationPath.try_error()));
    }
    cue::Result<void> validation = validate_tree(*stagingPath.try_value(), *m_assertContext);
    if (!validation)
    {
        return validation;
    }
    if (a_authorization != nullptr && !a_authorization->try_authorize())
    {
        return cue::Result<void>::failure(cue::make_io_error(
            *m_assertContext, cue::IoError::PreconditionFailed,
            "Staging directory publish authorization was rejected"));
    }
    const NativePublishOutcome publish = publish_handle_with_durability(
        record->second.handle.get(), *destinationPath.try_value(), record->second.identity, *m_assertContext);
    if (!publish.isPublished)
    {
        return cue::Result<void>::failure(
            make_windows_error(*m_assertContext, publish.nativeCode, "Staging directory publish failed"));
    }
    const std::uint64_t token = staging_token(a_staging);
    m_stagingPaths.erase(token);
    invalidate_staging(a_staging);
    if (publish.nativeCode != ERROR_SUCCESS)
    {
        return cue::Result<void>::failure(cue::make_io_error(
            *m_assertContext, cue::IoError::DurabilityUnknown,
            "Staging directory is published but Windows durability confirmation failed", publish.nativeCode));
    }
    return cue::Result<void>::success();
}

cue::Result<void> WindowsFilesystemRoot::rollback_staging_area(cue::StagingArea &&a_staging) noexcept
{
    if (!owns_staging(a_staging))
    {
        return cue::Result<void>::failure(
            cue::make_io_error(*m_assertContext, cue::IoError::OutsideRoot, "Staging ownership token is invalid"));
    }
    cue::Result<void> identity = validate_staging_identity(staging_token(a_staging), a_staging.path());
    if (!identity)
    {
        return identity;
    }
    cue::Result<std::wstring> stagingPath = absolute_path(a_staging.path());
    if (!stagingPath)
    {
        return cue::Result<void>::failure(std::move(*stagingPath.try_error()));
    }
    cue::Result<void> removed = remove_tree(*stagingPath.try_value(), *m_assertContext);
    if (!removed)
    {
        return removed;
    }
    const std::uint64_t token = staging_token(a_staging);
    m_stagingPaths.erase(token);
    invalidate_staging(a_staging);
    return cue::Result<void>::success();
}
} // namespace

namespace cue
{
Result<std::unique_ptr<FilesystemRoot>> create_windows_known_folder_filesystem_root(
    WindowsKnownFolder a_folder, const RelativePath &a_relativeRoot, WindowsRootOpenMode a_mode,
    const AssertContext &a_assertContext) noexcept
{
    if (a_folder != WindowsKnownFolder::LocalApplicationData)
    {
        return Result<std::unique_ptr<FilesystemRoot>>::failure(
            make_io_error(a_assertContext, IoError::InvalidPath, "Windows Known Folder is unsupported"));
    }

    UniqueCoTaskMemString knownFolderPath;
    const HRESULT resolved =
        SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, knownFolderPath.put());
    if (FAILED(resolved) || knownFolderPath.get() == nullptr)
    {
        return Result<std::unique_ptr<FilesystemRoot>>::failure(make_io_error(
            a_assertContext, IoError::IoFailure, "Windows Known Folder resolution failed",
            static_cast<std::int64_t>(resolved)));
    }

    std::wstring currentPath;
    try
    {
        currentPath.assign(knownFolderPath.get());
    }
    catch (...)
    {
        terminate_allocation(a_assertContext);
    }

    Result<void> knownFolder = require_absolute_directory(currentPath, a_assertContext);
    if (!knownFolder)
    {
        return Result<std::unique_ptr<FilesystemRoot>>::failure(std::move(*knownFolder.try_error()));
    }
    std::vector<UniqueHandle> pinnedDirectories;
    Result<UniqueHandle> pinnedKnownFolder = pin_absolute_directory(currentPath, a_assertContext);
    if (!pinnedKnownFolder)
    {
        return Result<std::unique_ptr<FilesystemRoot>>::failure(std::move(*pinnedKnownFolder.try_error()));
    }
    try
    {
        pinnedDirectories.push_back(std::move(*pinnedKnownFolder.try_value()));
    }
    catch (...)
    {
        terminate_allocation(a_assertContext);
    }

    Result<std::wstring> relativePath = to_utf16(a_relativeRoot.text(), a_assertContext);
    if (!relativePath)
    {
        return Result<std::unique_ptr<FilesystemRoot>>::failure(std::move(*relativePath.try_error()));
    }

    std::size_t begin = 0;
    while (begin < relativePath.try_value()->size())
    {
        const std::size_t separator = relativePath.try_value()->find(L'/', begin);
        const std::size_t end = separator == std::wstring::npos ? relativePath.try_value()->size() : separator;
        const std::wstring_view segment(relativePath.try_value()->data() + begin, end - begin);
        if (currentPath.size() + 1 + segment.size() >= k_maxWindowsPathLength)
        {
            return Result<std::unique_ptr<FilesystemRoot>>::failure(make_io_error(
                a_assertContext, IoError::CapacityExceeded, "Windows Known Folder root path is too long"));
        }
        try
        {
            currentPath.push_back(L'\\');
            currentPath.append(segment);
        }
        catch (...)
        {
            terminate_allocation(a_assertContext);
        }

        Result<void> opened = open_absolute_directory(currentPath, a_mode, a_assertContext);
        if (!opened)
        {
            return Result<std::unique_ptr<FilesystemRoot>>::failure(std::move(*opened.try_error()));
        }
        Result<UniqueHandle> pinned = pin_absolute_directory(currentPath, a_assertContext);
        if (!pinned)
        {
            return Result<std::unique_ptr<FilesystemRoot>>::failure(std::move(*pinned.try_error()));
        }
        try
        {
            pinnedDirectories.push_back(std::move(*pinned.try_value()));
        }
        catch (...)
        {
            terminate_allocation(a_assertContext);
        }
        if (separator == std::wstring::npos)
        {
            break;
        }
        begin = separator + 1;
    }

    Result<std::string> rootPath = to_utf8(currentPath, a_assertContext);
    if (!rootPath)
    {
        return Result<std::unique_ptr<FilesystemRoot>>::failure(std::move(*rootPath.try_error()));
    }
    Result<std::unique_ptr<FilesystemRoot>> filesystem =
        create_windows_filesystem_root(*rootPath.try_value(), a_assertContext);
    if (!filesystem)
    {
        return filesystem;
    }
    static_cast<WindowsFilesystemRoot *>(filesystem.try_value()->get())
        ->adopt_pinned_directories(std::move(pinnedDirectories));
    return filesystem;
}

Result<std::unique_ptr<FilesystemRoot>> create_windows_filesystem_root(std::string_view a_rootPath,
                                                                       const AssertContext &a_assertContext) noexcept
{
    Result<std::wstring> converted = to_utf16(a_rootPath, a_assertContext);
    if (!converted)
    {
        return Result<std::unique_ptr<FilesystemRoot>>::failure(std::move(*converted.try_error()));
    }

    const std::wstring_view input(*converted.try_value());
    const bool isDriveAbsolute = input.size() >= 3 && std::iswalpha(input[0]) != 0 && input[1] == L':' &&
                                 (input[2] == L'\\' || input[2] == L'/');
    const bool isUncAbsolute = input.starts_with(L"\\\\");
    if (!isDriveAbsolute && !isUncAbsolute)
    {
        return Result<std::unique_ptr<FilesystemRoot>>::failure(
            make_io_error(a_assertContext, IoError::InvalidPath, "Filesystem root path must be absolute"));
    }

    DWORD required = GetFullPathNameW(converted.try_value()->c_str(), 0, nullptr, nullptr);
    if (required == 0)
    {
        return Result<std::unique_ptr<FilesystemRoot>>::failure(
            make_windows_error(a_assertContext, GetLastError(), "Root absolute path resolution failed"));
    }
    std::wstring absolute;
    try
    {
        absolute.resize(required);
    }
    catch (...)
    {
        terminate_allocation(a_assertContext);
    }
    const DWORD written = GetFullPathNameW(converted.try_value()->c_str(), required, absolute.data(), nullptr);
    if (written == 0 || written >= required)
    {
        return Result<std::unique_ptr<FilesystemRoot>>::failure(
            make_windows_error(a_assertContext, GetLastError(), "Root absolute path resolution failed"));
    }
    absolute.resize(written);
    while (absolute.size() > 3 && (absolute.back() == L'\\' || absolute.back() == L'/'))
    {
        absolute.pop_back();
    }

    Result<std::wstring> extended = make_extended_path(std::move(absolute), a_assertContext);
    if (!extended)
    {
        return Result<std::unique_ptr<FilesystemRoot>>::failure(std::move(*extended.try_error()));
    }
    UniqueHandle root(CreateFileW(extended.try_value()->c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                  FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!root.is_valid())
    {
        return Result<std::unique_ptr<FilesystemRoot>>::failure(
            make_windows_error(a_assertContext, GetLastError(), "Filesystem root open failed"));
    }
    BY_HANDLE_FILE_INFORMATION information{};
    if (GetFileInformationByHandle(root.get(), &information) == FALSE)
    {
        return Result<std::unique_ptr<FilesystemRoot>>::failure(
            make_windows_error(a_assertContext, GetLastError(), "Filesystem root inspection failed"));
    }
    if ((information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
    {
        return Result<std::unique_ptr<FilesystemRoot>>::failure(
            make_io_error(a_assertContext, IoError::TypeMismatch, "Filesystem root is not a directory"));
    }
    if ((information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
    {
        return Result<std::unique_ptr<FilesystemRoot>>::failure(
            make_io_error(a_assertContext, IoError::UnsupportedEntry, "Filesystem root is a reparse point"));
    }

    const DWORD finalRequired =
        GetFinalPathNameByHandleW(root.get(), nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (finalRequired == 0)
    {
        return Result<std::unique_ptr<FilesystemRoot>>::failure(
            make_windows_error(a_assertContext, GetLastError(), "Filesystem root final path query failed"));
    }
    std::wstring finalPath;
    try
    {
        finalPath.resize(finalRequired);
    }
    catch (...)
    {
        terminate_allocation(a_assertContext);
    }
    const DWORD finalWritten =
        GetFinalPathNameByHandleW(root.get(), finalPath.data(), finalRequired, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (finalWritten == 0 || finalWritten >= finalRequired)
    {
        return Result<std::unique_ptr<FilesystemRoot>>::failure(
            make_windows_error(a_assertContext, GetLastError(), "Filesystem root final path query failed"));
    }
    finalPath.resize(finalWritten);

    try
    {
        std::unique_ptr<FilesystemRoot> filesystem = std::make_unique<WindowsFilesystemRoot>(
            a_assertContext, std::move(finalPath), std::move(root), information);
        return Result<std::unique_ptr<FilesystemRoot>>::success(std::move(filesystem));
    }
    catch (...)
    {
        terminate_allocation(a_assertContext);
    }
}
} // namespace cue
