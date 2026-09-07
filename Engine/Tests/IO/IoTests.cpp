#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Fatal.h>
#include <Cue/Foundation/Log.h>
#include <Cue/IO/Error.h>
#include <Cue/IO/Filesystem.h>
#include <Cue/IO/RelativePath.h>
#include <Cue/IO/Windows/WindowsFilesystem.h>

#include <Windows.h>
#include <ShlObj.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace
{
class TestFatalHandler final : public cue::FatalHandler
{
  public:
    /// @brief Test 中の回復不能失敗を追加処理なしで終了 Code へ変換する
    [[noreturn]] void terminate() noexcept override
    {
        std::_Exit(90);
    }

    /// @brief Message 付き回復不能失敗を追加処理なしで終了 Code へ変換する
    [[noreturn]] void terminate(std::string_view) noexcept override
    {
        std::_Exit(91);
    }
};

/// @brief Test 専用 Root Directory を一意 Path へ作成して終了時に限定 Cleanup する
class TestDirectory final
{
  public:
    /// @brief Process と時刻から生成した Test 専用 Directory を作成する
    TestDirectory()
    {
        std::array<wchar_t, MAX_PATH> temporary{};
        const DWORD length = GetTempPathW(static_cast<DWORD>(temporary.size()), temporary.data());
        if (length == 0 || length >= temporary.size())
        {
            return;
        }
        m_path = temporary.data();
        m_path += L"CueIoTests-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
        m_outsidePath = m_path + L"-Outside";
        const bool isRootCreated = CreateDirectoryW(m_path.c_str(), nullptr) != FALSE;
        const bool isOutsideCreated = CreateDirectoryW(m_outsidePath.c_str(), nullptr) != FALSE;
        m_isCreated = isRootCreated && isOutsideCreated;
        if (!m_isCreated)
        {
            RemoveDirectoryW(m_path.c_str());
            RemoveDirectoryW(m_outsidePath.c_str());
        }
    }

    /// @brief TestDirectory の一意 Cleanup 責務を保つため Copy 構築を禁止する
    TestDirectory(const TestDirectory &) = delete;
    /// @brief TestDirectory の一意 Cleanup 責務を保つため Copy 代入を禁止する
    TestDirectory &operator=(const TestDirectory &) = delete;
    /// @brief TestDirectory の所有権を移動する必要がないため Move 構築を禁止する
    TestDirectory(TestDirectory &&) = delete;
    /// @brief TestDirectory の所有権を移動する必要がないため Move 代入を禁止する
    TestDirectory &operator=(TestDirectory &&) = delete;

    /// @brief Test 専用 Directory だけを再帰 Cleanup する
    ~TestDirectory()
    {
        if (m_isCreated)
        {
            std::error_code error;
            std::filesystem::remove_all(m_path, error);
            std::filesystem::remove_all(m_outsidePath, error);
        }
    }

    /// @brief Directory 作成に成功したか返す
    [[nodiscard]] bool is_created() const noexcept
    {
        return m_isCreated;
    }

    /// @brief Windows Filesystem Factory へ渡す UTF-8 Path を返す
    [[nodiscard]] std::string utf8_path() const
    {
        const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, m_path.c_str(),
                                              static_cast<int>(m_path.size()), nullptr, 0, nullptr, nullptr);
        std::string result(static_cast<std::size_t>(count), '\0');
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, m_path.c_str(), static_cast<int>(m_path.size()),
                            result.data(), count, nullptr, nullptr);
        return result;
    }

    /// @brief Root へ結合した Native Path を Reparse Test へ返す
    [[nodiscard]] std::wstring child_path(std::wstring_view a_name) const
    {
        return m_path + L"\\" + std::wstring(a_name);
    }

    /// @brief Root 外 Reparse Point の Target に使用する Test 専用 Sibling Directory を返す
    [[nodiscard]] const std::wstring &outside_path() const noexcept
    {
        return m_outsidePath;
    }

  private:
    std::wstring m_path;
    std::wstring m_outsidePath;
    bool m_isCreated = false;
};

/// @brief Known Folder Factory Test専用の一意なLocalApplicationData直下Directoryを限定Cleanupする
class KnownFolderTestDirectory final
{
  public:
    /// @brief 実User Workspaceと製品Directoryから分離した一意なRelative Pathと長Path Cleanup Pathを生成する
    KnownFolderTestDirectory()
    {
        PWSTR localApplicationData = nullptr;
        const HRESULT result =
            SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &localApplicationData);
        if (FAILED(result) || localApplicationData == nullptr)
        {
            CoTaskMemFree(localApplicationData);
            return;
        }

        const std::string leaf =
            "CueIoTests-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64());
        const std::wstring nativeLeaf(leaf.begin(), leaf.end());
        m_relativePath = leaf;
        std::wstring nativePath = std::wstring(localApplicationData) + L"\\" + nativeLeaf;
        if (nativePath.starts_with(L"\\\\"))
        {
            m_cleanupPath = L"\\\\?\\UNC\\" + nativePath.substr(2);
        }
        else
        {
            m_cleanupPath = L"\\\\?\\" + nativePath;
        }
        m_movedPath = m_cleanupPath + L".moved";
        CoTaskMemFree(localApplicationData);
    }

    /// @brief Known Folder Test Directoryの一意Cleanup責務を保つためCopy構築を禁止する
    KnownFolderTestDirectory(const KnownFolderTestDirectory &) = delete;
    /// @brief Known Folder Test Directoryの一意Cleanup責務を保つためCopy代入を禁止する
    KnownFolderTestDirectory &operator=(const KnownFolderTestDirectory &) = delete;
    /// @brief Known Folder Test Directoryの一意Cleanup責務を保つためMove構築を禁止する
    KnownFolderTestDirectory(KnownFolderTestDirectory &&) = delete;
    /// @brief Known Folder Test Directoryの一意Cleanup責務を保つためMove代入を禁止する
    KnownFolderTestDirectory &operator=(KnownFolderTestDirectory &&) = delete;

    /// @brief 異常終了経路でもTest専用DirectoryだけをBest-effortで削除する
    ~KnownFolderTestDirectory()
    {
        if (!m_isRemoved && !m_cleanupPath.empty())
        {
            static_cast<void>(RemoveDirectoryW(m_cleanupPath.c_str()));
        }
    }

    /// @brief Known Folder解決とTest Path生成に成功したか返す
    [[nodiscard]] bool is_ready() const noexcept
    {
        return !m_relativePath.empty() && !m_cleanupPath.empty();
    }

    /// @brief Known Folder Factoryへ渡すTest専用Relative Pathを返す
    [[nodiscard]] const std::string &relative_path() const noexcept
    {
        return m_relativePath;
    }

    /// @brief Root寿命中のRenameがPin HandleのShare契約で拒否されるか確認する
    [[nodiscard]] bool is_replacement_blocked() noexcept
    {
        if (MoveFileExW(m_cleanupPath.c_str(), m_movedPath.c_str(), 0) == FALSE)
        {
            const DWORD nativeCode = GetLastError();
            return nativeCode == ERROR_SHARING_VIOLATION || nativeCode == ERROR_ACCESS_DENIED;
        }
        if (MoveFileExW(m_movedPath.c_str(), m_cleanupPath.c_str(), 0) == FALSE)
        {
            m_cleanupPath = std::move(m_movedPath);
        }
        return false;
    }

    /// @brief Root Handle解放後にTest専用Directoryを長Path APIで削除できたか返す
    [[nodiscard]] bool remove() noexcept
    {
        if (m_isRemoved || m_cleanupPath.empty())
        {
            return m_isRemoved;
        }
        if (RemoveDirectoryW(m_cleanupPath.c_str()) != FALSE)
        {
            m_isRemoved = true;
            return true;
        }
        const DWORD nativeCode = GetLastError();
        if (nativeCode == ERROR_FILE_NOT_FOUND || nativeCode == ERROR_PATH_NOT_FOUND)
        {
            m_isRemoved = true;
            return true;
        }
        return false;
    }

  private:
    std::string m_relativePath;
    std::wstring m_cleanupPath;
    std::wstring m_movedPath;
    bool m_isRemoved = false;
};

/// @brief Portable Path の ASCII 表現を Windows Test 用 UTF-16 へ変換する
[[nodiscard]] std::wstring widen_ascii(std::string_view a_text)
{
    return std::wstring(a_text.begin(), a_text.end());
}

/// @brief 利用者LocatorからWindows AdapterのHidden Recovery Backup Native Pathを生成する
[[nodiscard]] std::wstring native_recovery_backup_path(const TestDirectory &a_directory,
                                                        std::string_view a_destination)
{
    std::wstring relative = widen_ascii(a_destination);
    for (wchar_t &character : relative)
    {
        if (character == L'/')
        {
            character = L'\\';
        }
    }
    const std::size_t separator = relative.find_last_of(L'\\');
    relative.insert(separator == std::wstring::npos ? 0U : separator + 1U, L".CueBackup-");
    return L"\\\\?\\" + a_directory.child_path(relative);
}

enum class FailurePoint : std::uint8_t
{
    None,
    RootBind,
    Query,
    Read,
    CreateDirectories,
    CreateTemporary,
    WriteBeforeFirstByte,
    WriteMidway,
    WriteMidwayCleanup,
    WriteAfterFull,
    FileFlush,
    CreateStaging,
    ValidateStaged,
    PrePublishReparseValidation,
    Publish,
    DirectoryFlush,
    RollbackRemove
};

/// @brief Platform 非依存呼出し側が各 Storage 失敗を再現できる Test Double
class FailingFilesystemRoot final : public cue::FilesystemRoot
{
  public:
    /// @brief 一度だけ失敗させる Operation を設定する
    FailingFilesystemRoot(FailurePoint a_failurePoint, const cue::AssertContext &a_assertContext) noexcept
        : m_failurePoint(a_failurePoint), m_assertContext(&a_assertContext)
    {
    }

    /// @brief Test Double の一意状態を保つため Copy 構築を禁止する
    FailingFilesystemRoot(const FailingFilesystemRoot &) = delete;
    /// @brief Test Double の一意状態を保つため Copy 代入を禁止する
    FailingFilesystemRoot &operator=(const FailingFilesystemRoot &) = delete;
    /// @brief Test Double の状態を移動する必要がないため Move 構築を禁止する
    FailingFilesystemRoot(FailingFilesystemRoot &&) = delete;
    /// @brief Test Double の状態を移動する必要がないため Move 代入を禁止する
    FailingFilesystemRoot &operator=(FailingFilesystemRoot &&) = delete;
    /// @brief Native Resource を持たない Test Double を破棄する
    ~FailingFilesystemRoot() override = default;

    /// @brief Test Double InstanceをMemory Root Identityとして返す
    [[nodiscard]] cue::Result<cue::FilesystemIdentity> root_identity() const noexcept override
    {
        return cue::Result<cue::FilesystemIdentity>::success(make_filesystem_identity(
            0x544553544D454D32ULL, static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(this))));
    }

    /// @brief Query Failure Point を一度だけ再現する
    [[nodiscard]] cue::Result<cue::EntryType> query_entry(const cue::RelativePath &) noexcept override
    {
        if (consume(FailurePoint::Query))
        {
            return cue::Result<cue::EntryType>::failure(make_failure());
        }
        return cue::Result<cue::EntryType>::success(cue::EntryType::Missing);
    }

    /// @brief Read Failure Point を一度だけ再現する
    [[nodiscard]] cue::Result<std::vector<std::byte>> read_file(const cue::RelativePath &,
                                                                std::size_t) noexcept override
    {
        if (consume(FailurePoint::Read))
        {
            return cue::Result<std::vector<std::byte>>::failure(make_failure());
        }
        return cue::Result<std::vector<std::byte>>::success({});
    }

    /// @brief Directory 作成 Failure Point を一度だけ再現する
    [[nodiscard]] cue::Result<void> create_directories(const cue::RelativePath &) noexcept override
    {
        return consume(FailurePoint::CreateDirectories) ? cue::Result<void>::failure(make_failure())
                                                        : cue::Result<void>::success();
    }

    /// @brief Atomic Write Failure Point を一度だけ再現する
    [[nodiscard]] cue::Result<void> write_file_atomic(const cue::RelativePath &,
                                                      std::span<const std::byte>) noexcept override
    {
        if (consume(FailurePoint::CreateTemporary))
        {
            return cue::Result<void>::failure(make_failure());
        }
        m_hasTemporary = true;
        if (consume(FailurePoint::WriteMidwayCleanup))
        {
            cue::Error primary = make_failure();
            cue::Error cleanup = make_failure();
            primary.append_secondary_diagnostics(*m_assertContext, cleanup, "Injected temporary cleanup failure",
                                                 "Cue.IO test cleanup");
            return cue::Result<void>::failure(std::move(primary));
        }
        if (consume(FailurePoint::WriteBeforeFirstByte) || consume(FailurePoint::WriteMidway) ||
            consume(FailurePoint::WriteAfterFull) || consume(FailurePoint::FileFlush) || consume(FailurePoint::Publish))
        {
            m_hasTemporary = false;
            return cue::Result<void>::failure(make_failure());
        }

        m_hasTemporary = false;
        m_hasOriginalFile = false;
        if (consume(FailurePoint::DirectoryFlush))
        {
            return cue::Result<void>::failure(make_durability_failure());
        }
        return cue::Result<void>::success();
    }

    /// @brief Recovery Backup公開FailureをTest DoubleのIO失敗へ変換する
    [[nodiscard]] cue::Result<void> write_recovery_backup_atomic(
        const cue::RelativePath &, std::span<const std::byte>, const cue::AssertContext &) noexcept override
    {
        return cue::Result<void>::failure(make_failure());
    }

    [[nodiscard]] cue::Result<cue::FileWriteLease> acquire_file_write_lease(const cue::RelativePath &) noexcept override
    {
        return cue::Result<cue::FileWriteLease>::failure(make_failure());
    }

    [[nodiscard]] cue::Result<void> write_file_atomic_if_unchanged(cue::FileWriteLease &, const cue::RelativePath &,
                                                                   cue::FileFingerprint, std::size_t,
                                                                   std::span<const std::byte>) noexcept override
    {
        return cue::Result<void>::failure(make_failure());
    }

    [[nodiscard]] cue::Result<void> remove_file(const cue::RelativePath &) noexcept override
    {
        return cue::Result<void>::failure(make_failure());
    }

    /// @brief Staging 作成 Failure Point または偽造不能 Token を返す
    [[nodiscard]] cue::Result<cue::StagingArea> create_staging_area(const cue::RelativePath &) noexcept override
    {
        if (consume(FailurePoint::CreateStaging))
        {
            return cue::Result<cue::StagingArea>::failure(make_failure());
        }
        m_hasStaging = true;
        auto path = cue::RelativePath::parse("CueStaging-test", *m_assertContext);
        return cue::Result<cue::StagingArea>::success(make_staging_area(std::move(*path.try_value()), 1));
    }

    /// @brief Staging 検証から耐久性確認までの Publish Failure Point を一度だけ再現する
    [[nodiscard]] cue::Result<void> publish_staging_area(cue::StagingArea &&a_staging,
                                                         const cue::RelativePath &) noexcept override
    {
        if (consume(FailurePoint::ValidateStaged) || consume(FailurePoint::PrePublishReparseValidation) ||
            consume(FailurePoint::Publish))
        {
            return cue::Result<void>::failure(make_failure());
        }

        m_hasStaging = false;
        m_isProjectPublished = true;
        invalidate_staging(a_staging);
        if (consume(FailurePoint::DirectoryFlush))
        {
            return cue::Result<void>::failure(make_durability_failure());
        }
        return cue::Result<void>::success();
    }

    /// @brief Staging Rollback Remove Failure Point を一度だけ再現する
    [[nodiscard]] cue::Result<void> rollback_staging_area(cue::StagingArea &&a_staging) noexcept override
    {
        if (consume(FailurePoint::RollbackRemove))
        {
            return cue::Result<void>::failure(make_failure());
        }
        m_hasStaging = false;
        invalidate_staging(a_staging);
        return cue::Result<void>::success();
    }

    /// @brief Atomic Write 失敗後に元 Destination が維持されたか返す
    [[nodiscard]] bool has_original_file() const noexcept
    {
        return m_hasOriginalFile;
    }

    /// @brief Atomic Write の Temporary File が Cleanup されたか検証するため存在状態を返す
    [[nodiscard]] bool has_temporary() const noexcept
    {
        return m_hasTemporary;
    }

    /// @brief Project Publish 前失敗後に Staging が Rollback 可能な状態か返す
    [[nodiscard]] bool has_staging() const noexcept
    {
        return m_hasStaging;
    }

    /// @brief Publish 後の耐久性失敗でも完成名が可視化済みか返す
    [[nodiscard]] bool is_project_published() const noexcept
    {
        return m_isProjectPublished;
    }

  private:
    /// @brief 指定 Failure Point が未消費なら一度だけ true を返す
    [[nodiscard]] bool consume(FailurePoint a_failurePoint) noexcept
    {
        if (m_failurePoint == a_failurePoint)
        {
            m_failurePoint = FailurePoint::None;
            return true;
        }
        return false;
    }

    /// @brief Test Double 用の安定した Portable 分類 Error を生成する
    [[nodiscard]] cue::Error make_failure() const noexcept
    {
        return cue::make_io_error(*m_assertContext, cue::IoError::IoFailure, "Injected filesystem failure");
    }

    /// @brief Publish 済みだが耐久性だけ確定できない Portable Error を生成する
    [[nodiscard]] cue::Error make_durability_failure() const noexcept
    {
        return cue::make_io_error(*m_assertContext, cue::IoError::DurabilityUnknown,
                                  "Injected directory durability failure");
    }

    FailurePoint m_failurePoint;
    const cue::AssertContext *m_assertContext;
    bool m_hasOriginalFile = true;
    bool m_hasTemporary = false;
    bool m_hasStaging = false;
    bool m_isProjectPublished = false;
};

/// @brief Root Bind Failure Point を Storage Operation と同じ Portable Error 契約で再現する
[[nodiscard]] cue::Result<std::unique_ptr<cue::FilesystemRoot>> create_failing_filesystem_root(
    FailurePoint a_failurePoint, const cue::AssertContext &a_assertContext) noexcept
{
    if (a_failurePoint == FailurePoint::RootBind)
    {
        return cue::Result<std::unique_ptr<cue::FilesystemRoot>>::failure(
            cue::make_io_error(a_assertContext, cue::IoError::IoFailure, "Injected root bind failure"));
    }

    try
    {
        std::unique_ptr<cue::FilesystemRoot> filesystem =
            std::make_unique<FailingFilesystemRoot>(a_failurePoint, a_assertContext);
        return cue::Result<std::unique_ptr<cue::FilesystemRoot>>::success(std::move(filesystem));
    }
    catch (...)
    {
        a_assertContext.fatal_handler().terminate("Failing filesystem allocation failed");
        std::abort();
    }
}

/// @brief Result が指定 Portable IO 分類を保持するか判定する
template <typename T> [[nodiscard]] bool has_io_error(cue::Result<T> &a_result, cue::IoError a_code) noexcept
{
    const cue::Error *error = a_result.try_error();
    return error != nullptr && error->code().domain() == "Cue.IO" &&
           error->code().value() == static_cast<std::int64_t>(a_code);
}

/// @brief Portable Relative Path の境界値と Root 脱出拒否を検証する
[[nodiscard]] bool test_relative_paths(const cue::AssertContext &a_assertContext)
{
    auto valid = cue::RelativePath::parse("Assets/Source", a_assertContext);
    auto parent = cue::RelativePath::parse("../Outside", a_assertContext);
    auto rooted = cue::RelativePath::parse("C:/Outside", a_assertContext);
    auto backslash = cue::RelativePath::parse("Assets\\Source", a_assertContext);
    auto reserved = cue::RelativePath::parse("NUL.data", a_assertContext);
    auto hidden = cue::RelativePath::parse(".Hidden", a_assertContext);
    const std::string nonAscii(1, static_cast<char>(0xe9));
    auto localeSensitive = cue::RelativePath::parse(nonAscii, a_assertContext);
    const std::string longSegment(65, 'a');
    auto tooLong = cue::RelativePath::parse(longSegment, a_assertContext);

    return valid && valid.try_value()->comparison_key(a_assertContext) == "assets/source" &&
           has_io_error(parent, cue::IoError::InvalidPath) && has_io_error(rooted, cue::IoError::InvalidPath) &&
           has_io_error(backslash, cue::IoError::InvalidPath) && has_io_error(reserved, cue::IoError::InvalidPath) &&
           has_io_error(hidden, cue::IoError::InvalidPath) &&
           has_io_error(localeSensitive, cue::IoError::InvalidPath) && has_io_error(tooLong, cue::IoError::InvalidPath);
}

/// @brief Windows Root 内の Directory 作成と Atomic File 置換を実 Filesystem で検証する
[[nodiscard]] bool test_windows_file_operations(cue::FilesystemRoot &a_filesystem, const TestDirectory &a_directory,
                                                const cue::AssertContext &a_assertContext)
{
    auto directory = cue::RelativePath::parse("Data/Nested", a_assertContext);
    auto file = cue::RelativePath::parse("Data/Nested/State.bin", a_assertContext);
    auto missingParentFile = cue::RelativePath::parse("Missing/State.bin", a_assertContext);
    const std::array first{std::byte{1}, std::byte{2}, std::byte{3}};
    const std::array second{std::byte{9}, std::byte{8}};
    if (!directory || !file || !missingParentFile)
    {
        return false;
    }
    auto missingParentWrite = a_filesystem.write_file_atomic(*missingParentFile.try_value(), first);
    if (!has_io_error(missingParentWrite, cue::IoError::NotFound) ||
        !a_filesystem.create_directories(*directory.try_value()) ||
        !a_filesystem.write_file_atomic(*file.try_value(), first))
    {
        return false;
    }
    auto firstRead = a_filesystem.read_file(*file.try_value(), 16);
    if (!firstRead || *firstRead.try_value() != std::vector<std::byte>(first.begin(), first.end()) ||
        !a_filesystem.write_file_atomic(*file.try_value(), second))
    {
        return false;
    }
    auto secondRead = a_filesystem.read_file(*file.try_value(), 16);
    auto limitedRead = a_filesystem.read_file(*file.try_value(), 1);
    if (!secondRead || *secondRead.try_value() != std::vector<std::byte>(second.begin(), second.end()) ||
        !has_io_error(limitedRead, cue::IoError::CapacityExceeded))
    {
        return false;
    }

    const std::string longParentSegment(53U, 'p');
    const std::string longParentText = longParentSegment + "/" + longParentSegment + "/" + longParentSegment + "/" +
                                       longParentSegment;
    const std::string longFileText = longParentText + "/x";
    auto longParent = cue::RelativePath::parse(longParentText, a_assertContext);
    auto longFile = cue::RelativePath::parse(longFileText, a_assertContext);
    if (!longParent || !longFile || !a_filesystem.create_directories(*longParent.try_value()) ||
        !a_filesystem.write_file_atomic(*longFile.try_value(), first))
    {
        return false;
    }
    auto longFileRead = a_filesystem.read_file(*longFile.try_value(), 16U);
    if (!longFileRead || *longFileRead.try_value() != std::vector<std::byte>(first.begin(), first.end()))
    {
        return false;
    }

    const std::string maximumPathSegment(60U, 'q');
    const std::string maximumPathParent = maximumPathSegment + "/" + maximumPathSegment + "/" +
                                          maximumPathSegment + "/" + maximumPathSegment;
    const std::string maximumPathText = maximumPathParent + "/12345678901";
    auto maximumPathParentLocator = cue::RelativePath::parse(maximumPathParent, a_assertContext);
    auto maximumPath = cue::RelativePath::parse(maximumPathText, a_assertContext);
    if (!maximumPathParentLocator || !maximumPath ||
        !a_filesystem.create_directories(*maximumPathParentLocator.try_value()) ||
        !a_filesystem.write_recovery_backup_atomic(*maximumPath.try_value(), second, a_assertContext))
    {
        return false;
    }
    const std::wstring maximumPathBackupNative = native_recovery_backup_path(a_directory, maximumPathText);
    if (GetFileAttributesW(maximumPathBackupNative.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        return false;
    }

    const std::wstring nativePath = a_directory.child_path(L"Data\\Nested\\State.bin");
    HANDLE locked = CreateFileW(nativePath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (locked == INVALID_HANDLE_VALUE)
    {
        return false;
    }
    cue::Result<void> failedReplace = a_filesystem.write_file_atomic(*file.try_value(), first);
    CloseHandle(locked);

    auto preserved = a_filesystem.read_file(*file.try_value(), 16);
    if (!has_io_error(failedReplace, cue::IoError::PermissionDenied) || !preserved ||
        *preserved.try_value() != std::vector<std::byte>(second.begin(), second.end()))
    {
        return false;
    }

    auto expected = cue::fingerprint_file(a_filesystem, *file.try_value(), 16U, a_assertContext);
    auto competingRoot = cue::create_windows_filesystem_root(a_directory.utf8_path(), a_assertContext);
    auto backup = cue::RelativePath::parse("Data/Nested/State.bin.backup", a_assertContext);
    auto formerSidecar = cue::RelativePath::parse("Data/Nested/State.bin.cuelock", a_assertContext);
    const std::string maximumSegmentText(64U, 'x');
    auto maximumSegment = cue::RelativePath::parse(maximumSegmentText, a_assertContext);
    if (!expected || !competingRoot || !backup || !formerSidecar || !maximumSegment ||
        !a_filesystem.write_file_atomic(*formerSidecar.try_value(), second) ||
        !a_filesystem.write_file_atomic(*backup.try_value(), second))
    {
        return false;
    }
    {
        auto lease = a_filesystem.acquire_file_write_lease(*file.try_value());
        if (!lease)
        {
            return false;
        }
        auto busy = (*competingRoot.try_value())->acquire_file_write_lease(*file.try_value());
        auto backupLease = (*competingRoot.try_value())->acquire_file_write_lease(*backup.try_value());
        if (!has_io_error(busy, cue::IoError::Busy) || !backupLease ||
            !a_filesystem.write_file_atomic_if_unchanged(*lease.try_value(), *file.try_value(), *expected.try_value(),
                                                         16U, first))
        {
            return false;
        }
    }
    auto preservedFormerSidecar = a_filesystem.read_file(*formerSidecar.try_value(), 16U);
    auto maximumSegmentLease = a_filesystem.acquire_file_write_lease(*maximumSegment.try_value());
    if (!preservedFormerSidecar ||
        *preservedFormerSidecar.try_value() != std::vector<std::byte>(second.begin(), second.end()) ||
        !maximumSegmentLease ||
        !a_filesystem.write_recovery_backup_atomic(*maximumSegment.try_value(), first, a_assertContext))
    {
        return false;
    }
    const std::wstring maximumBackupNative = native_recovery_backup_path(a_directory, maximumSegmentText);
    if (GetFileAttributesW(maximumBackupNative.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        return false;
    }
    if (!a_filesystem.write_recovery_backup_atomic(*file.try_value(), first, a_assertContext))
    {
        return false;
    }
    auto preservedUserBackup = a_filesystem.read_file(*backup.try_value(), 16U);
    if (!preservedUserBackup ||
        *preservedUserBackup.try_value() != std::vector<std::byte>(second.begin(), second.end()))
    {
        return false;
    }
    {
        auto lease = a_filesystem.acquire_file_write_lease(*file.try_value());
        if (!lease)
        {
            return false;
        }
        auto stale = a_filesystem.write_file_atomic_if_unchanged(*lease.try_value(), *file.try_value(),
                                                                 *expected.try_value(), 16U, second);
        if (!has_io_error(stale, cue::IoError::PreconditionFailed))
        {
            return false;
        }
    }
    auto afterStale = a_filesystem.read_file(*file.try_value(), 16U);
    if (!afterStale || *afterStale.try_value() != std::vector<std::byte>(first.begin(), first.end()))
    {
        return false;
    }
    {
        auto crossThreadExpected = cue::fingerprint_file(a_filesystem, *file.try_value(), 16U, a_assertContext);
        auto threadBoundLease = a_filesystem.acquire_file_write_lease(*file.try_value());
        if (!crossThreadExpected || !threadBoundLease)
        {
            return false;
        }
        bool foreignThreadRejected = false;
        std::thread leaseThread(
            [&a_filesystem, &file, &crossThreadExpected, &threadBoundLease, &first, &foreignThreadRejected]()
            {
                auto foreignWrite = a_filesystem.write_file_atomic_if_unchanged(
                    *threadBoundLease.try_value(), *file.try_value(), *crossThreadExpected.try_value(), 16U, first);
                foreignThreadRejected = has_io_error(foreignWrite, cue::IoError::PreconditionFailed);
            });
        leaseThread.join();
        auto ownerWrite = a_filesystem.write_file_atomic_if_unchanged(*threadBoundLease.try_value(), *file.try_value(),
                                                                      *crossThreadExpected.try_value(), 16U, first);
        if (!foreignThreadRejected || !ownerWrite)
        {
            return false;
        }
    }
    {
        auto leaseAfterThreadCheck = a_filesystem.acquire_file_write_lease(*file.try_value());
        if (!leaseAfterThreadCheck)
        {
            return false;
        }
    }
    const std::wstring hardLinkPath = a_directory.child_path(L"Data\\Nested\\StateAlias.bin");
    if (CreateHardLinkW(hardLinkPath.c_str(), nativePath.c_str(), nullptr) == FALSE)
    {
        return false;
    }
    {
        auto hardLinkedFingerprint = cue::fingerprint_file(a_filesystem, *file.try_value(), 16U, a_assertContext);
        auto backupLease = a_filesystem.acquire_file_write_lease(*backup.try_value());
        if (!hardLinkedFingerprint || !backupLease)
        {
            return false;
        }
        auto mismatchedLeaseWrite = a_filesystem.write_file_atomic_if_unchanged(
            *backupLease.try_value(), *file.try_value(), *hardLinkedFingerprint.try_value(), 16U, second);
        if (!has_io_error(mismatchedLeaseWrite, cue::IoError::PreconditionFailed))
        {
            return false;
        }
    }
    auto hardLinkLease = a_filesystem.acquire_file_write_lease(*file.try_value());
    const bool hardLinkRejected = has_io_error(hardLinkLease, cue::IoError::UnsupportedEntry);
    const bool hardLinkRemoved = DeleteFileW(hardLinkPath.c_str()) != FALSE;
    if (!hardLinkRejected || !hardLinkRemoved || !a_filesystem.remove_file(*file.try_value()) ||
        !a_filesystem.remove_file(*formerSidecar.try_value()))
    {
        return false;
    }
    auto removed = a_filesystem.query_entry(*file.try_value());
    return removed && *removed.try_value() == cue::EntryType::Missing && a_filesystem.remove_file(*file.try_value());
}

/// @brief Staging Directory の Publish、既存 Destination 拒否、Rollback を検証する
[[nodiscard]] bool test_windows_staging(cue::FilesystemRoot &a_filesystem, const cue::AssertContext &a_assertContext)
{
    auto project = cue::RelativePath::parse("Project", a_assertContext);
    auto existing = cue::RelativePath::parse("Existing", a_assertContext);
    auto rollbackTarget = cue::RelativePath::parse("RollbackTarget", a_assertContext);
    auto racedTarget = cue::RelativePath::parse("RacedTarget", a_assertContext);
    if (!project || !existing || !rollbackTarget || !racedTarget ||
        !a_filesystem.create_directories(*existing.try_value()))
    {
        return false;
    }
    auto rejected = a_filesystem.create_staging_area(*existing.try_value());
    auto staging = a_filesystem.create_staging_area(*project.try_value());
    if (!has_io_error(rejected, cue::IoError::AlreadyExists) || !staging)
    {
        return false;
    }

    const std::string childText = std::string(staging.try_value()->path().text()) + "/Assets";
    auto child = cue::RelativePath::parse(childText, a_assertContext);
    if (!child || !a_filesystem.create_directories(*child.try_value()) ||
        !a_filesystem.publish_staging_area(std::move(*staging.try_value()), *project.try_value()))
    {
        return false;
    }
    auto projectType = a_filesystem.query_entry(*project.try_value());
    auto rollback = a_filesystem.create_staging_area(*rollbackTarget.try_value());
    if (!projectType || *projectType.try_value() != cue::EntryType::Directory || !rollback)
    {
        return false;
    }
    const std::string rollbackPathText(rollback.try_value()->path().text());
    auto rollbackPath = cue::RelativePath::parse(rollbackPathText, a_assertContext);
    if (!rollbackPath || !a_filesystem.rollback_staging_area(std::move(*rollback.try_value())))
    {
        return false;
    }
    auto rollbackType = a_filesystem.query_entry(*rollbackPath.try_value());
    if (!rollbackType || *rollbackType.try_value() != cue::EntryType::Missing)
    {
        return false;
    }

    auto racedStaging = a_filesystem.create_staging_area(*racedTarget.try_value());
    if (!racedStaging)
    {
        return false;
    }
    const std::string racedStagingText(racedStaging.try_value()->path().text());
    auto racedStagingPath = cue::RelativePath::parse(racedStagingText, a_assertContext);
    if (!racedStagingPath || !a_filesystem.create_directories(*racedTarget.try_value()))
    {
        return false;
    }
    auto racedPublish =
        a_filesystem.publish_staging_area(std::move(*racedStaging.try_value()), *racedTarget.try_value());
    if (!has_io_error(racedPublish, cue::IoError::AlreadyExists) ||
        !a_filesystem.rollback_staging_area(std::move(*racedStaging.try_value())))
    {
        return false;
    }
    auto racedType = a_filesystem.query_entry(*racedTarget.try_value());
    auto racedStagingType = a_filesystem.query_entry(*racedStagingPath.try_value());
    if (!racedType || *racedType.try_value() != cue::EntryType::Directory || !racedStagingType ||
        *racedStagingType.try_value() != cue::EntryType::Missing)
    {
        return false;
    }

    auto firstTarget = cue::RelativePath::parse("MoveFirst", a_assertContext);
    auto secondTarget = cue::RelativePath::parse("MoveSecond", a_assertContext);
    auto first = a_filesystem.create_staging_area(*firstTarget.try_value());
    auto second = a_filesystem.create_staging_area(*secondTarget.try_value());
    if (!first || !second)
    {
        return false;
    }
    *first.try_value() = std::move(*second.try_value());
    if (!a_filesystem.rollback_staging_area(std::move(*first.try_value())) ||
        !a_filesystem.rollback_staging_area(std::move(*second.try_value())))
    {
        return false;
    }

    auto boundParent = cue::RelativePath::parse("Bound", a_assertContext);
    auto otherParent = cue::RelativePath::parse("Other", a_assertContext);
    auto boundDestination = cue::RelativePath::parse("Bound/Project", a_assertContext);
    auto otherDestination = cue::RelativePath::parse("Other/Project", a_assertContext);
    if (!a_filesystem.create_directories(*boundParent.try_value()) ||
        !a_filesystem.create_directories(*otherParent.try_value()))
    {
        return false;
    }
    auto boundStaging = a_filesystem.create_staging_area(*boundDestination.try_value());
    auto wrongPublish =
        a_filesystem.publish_staging_area(std::move(*boundStaging.try_value()), *otherDestination.try_value());
    return has_io_error(wrongPublish, cue::IoError::OutsideRoot) &&
           a_filesystem.rollback_staging_area(std::move(*boundStaging.try_value()));
}

/// @brief 利用可能な Windows 環境で Reparse Point を Unsupported Entry として拒否することを検証する
[[nodiscard]] bool test_reparse_rejection(cue::FilesystemRoot &a_filesystem, const TestDirectory &a_directory,
                                          const cue::AssertContext &a_assertContext)
{
    const std::wstring &target = a_directory.outside_path();
    const std::wstring link = a_directory.child_path(L"ReparseLink");
    if (CreateSymbolicLinkW(link.c_str(), target.c_str(),
                            SYMBOLIC_LINK_FLAG_DIRECTORY | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE) == FALSE)
    {
        const DWORD code = GetLastError();
        return code == ERROR_PRIVILEGE_NOT_HELD || code == ERROR_INVALID_PARAMETER || code == ERROR_NOT_SUPPORTED;
    }
    auto path = cue::RelativePath::parse("ReparseLink", a_assertContext);
    auto child = cue::RelativePath::parse("ReparseLink/Escape.bin", a_assertContext);
    auto type = a_filesystem.query_entry(*path.try_value());
    const std::array<std::byte, 1> bytes{std::byte{1}};
    auto directWrite = a_filesystem.write_file_atomic(*path.try_value(), bytes);
    auto childWrite = a_filesystem.write_file_atomic(*child.try_value(), bytes);
    return type && *type.try_value() == cue::EntryType::UnsupportedEntry &&
           has_io_error(directWrite, cue::IoError::UnsupportedEntry) &&
           has_io_error(childWrite, cue::IoError::UnsupportedEntry);
}

/// @brief Staging Root 自体の Reparse Point を Publish と Rollback が Follow しないことを検証する
[[nodiscard]] bool test_staging_reparse_root(cue::FilesystemRoot &a_filesystem, const TestDirectory &a_directory,
                                             const cue::AssertContext &a_assertContext)
{
    auto destination = cue::RelativePath::parse("ReparseProject", a_assertContext);
    auto staging = a_filesystem.create_staging_area(*destination.try_value());
    if (!staging)
    {
        return false;
    }
    const std::wstring stagingPath = a_directory.child_path(widen_ascii(staging.try_value()->path().text()));
    const std::wstring displacedPath = stagingPath + L"-Original";
    if (MoveFileExW(stagingPath.c_str(), displacedPath.c_str(), 0) == FALSE)
    {
        return false;
    }
    if (CreateSymbolicLinkW(stagingPath.c_str(), a_directory.outside_path().c_str(),
                            SYMBOLIC_LINK_FLAG_DIRECTORY | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE) == FALSE)
    {
        const DWORD code = GetLastError();
        return code == ERROR_PRIVILEGE_NOT_HELD || code == ERROR_INVALID_PARAMETER || code == ERROR_NOT_SUPPORTED;
    }

    auto publish = a_filesystem.publish_staging_area(std::move(*staging.try_value()), *destination.try_value());
    auto rollback = a_filesystem.rollback_staging_area(std::move(*staging.try_value()));
    const DWORD outsideAttributes = GetFileAttributesW(a_directory.outside_path().c_str());
    const DWORD originalAttributes = GetFileAttributesW(displacedPath.c_str());
    return has_io_error(publish, cue::IoError::UnsupportedEntry) &&
           has_io_error(rollback, cue::IoError::UnsupportedEntry) && outsideAttributes != INVALID_FILE_ATTRIBUTES &&
           (outsideAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 && originalAttributes != INVALID_FILE_ATTRIBUTES &&
           (originalAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

/// @brief Staging Path を同名 Directory へ差し替えても Operation 所有物として扱わないことを検証する
[[nodiscard]] bool test_staging_identity_replacement(cue::FilesystemRoot &a_filesystem,
                                                     const TestDirectory &a_directory,
                                                     const cue::AssertContext &a_assertContext)
{
    auto destination = cue::RelativePath::parse("IdentityProject", a_assertContext);
    auto staging = a_filesystem.create_staging_area(*destination.try_value());
    if (!staging)
    {
        return false;
    }
    const std::wstring stagingPath = a_directory.child_path(widen_ascii(staging.try_value()->path().text()));
    const std::wstring displacedPath = stagingPath + L"-Original";
    if (MoveFileExW(stagingPath.c_str(), displacedPath.c_str(), 0) == FALSE ||
        CreateDirectoryW(stagingPath.c_str(), nullptr) == FALSE)
    {
        return false;
    }

    auto publish = a_filesystem.publish_staging_area(std::move(*staging.try_value()), *destination.try_value());
    auto rollback = a_filesystem.rollback_staging_area(std::move(*staging.try_value()));
    const DWORD replacementAttributes = GetFileAttributesW(stagingPath.c_str());
    const DWORD originalAttributes = GetFileAttributesW(displacedPath.c_str());
    return has_io_error(publish, cue::IoError::OutsideRoot) && has_io_error(rollback, cue::IoError::OutsideRoot) &&
           replacementAttributes != INVALID_FILE_ATTRIBUTES &&
           (replacementAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 && originalAttributes != INVALID_FILE_ATTRIBUTES &&
           (originalAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

/// @brief 全 Operation Failure Point が一度だけ Portable Error を返すことを検証する
[[nodiscard]] bool test_failure_injection(const cue::AssertContext &a_assertContext)
{
    auto path = cue::RelativePath::parse("Data", a_assertContext);
    const std::array<std::byte, 1> bytes{std::byte{1}};
    auto rootBind = create_failing_filesystem_root(FailurePoint::RootBind, a_assertContext);
    FailingFilesystemRoot query(FailurePoint::Query, a_assertContext);
    FailingFilesystemRoot read(FailurePoint::Read, a_assertContext);
    FailingFilesystemRoot directories(FailurePoint::CreateDirectories, a_assertContext);
    FailingFilesystemRoot create(FailurePoint::CreateStaging, a_assertContext);
    auto queryResult = query.query_entry(*path.try_value());
    auto queryRetry = query.query_entry(*path.try_value());
    auto readResult = read.read_file(*path.try_value(), 4);
    auto directoryResult = directories.create_directories(*path.try_value());
    auto createResult = create.create_staging_area(*path.try_value());
    if (!has_io_error(rootBind, cue::IoError::IoFailure) || !has_io_error(queryResult, cue::IoError::IoFailure) ||
        !queryRetry || !has_io_error(readResult, cue::IoError::IoFailure) ||
        !has_io_error(directoryResult, cue::IoError::IoFailure) || !has_io_error(createResult, cue::IoError::IoFailure))
    {
        return false;
    }

    constexpr std::array atomicFailures{FailurePoint::CreateTemporary, FailurePoint::WriteBeforeFirstByte,
                                        FailurePoint::WriteMidway,     FailurePoint::WriteAfterFull,
                                        FailurePoint::FileFlush,       FailurePoint::Publish};
    for (const FailurePoint failure : atomicFailures)
    {
        FailingFilesystemRoot filesystem(failure, a_assertContext);
        auto result = filesystem.write_file_atomic(*path.try_value(), bytes);
        if (!has_io_error(result, cue::IoError::IoFailure) || !filesystem.has_original_file() ||
            filesystem.has_temporary())
        {
            return false;
        }
    }

    FailingFilesystemRoot cleanup(FailurePoint::WriteMidwayCleanup, a_assertContext);
    auto cleanupResult = cleanup.write_file_atomic(*path.try_value(), bytes);
    if (!has_io_error(cleanupResult, cue::IoError::IoFailure) || !cleanup.has_original_file() ||
        !cleanup.has_temporary() || cleanupResult.try_error()->contexts().empty())
    {
        return false;
    }

    FailingFilesystemRoot atomicDurability(FailurePoint::DirectoryFlush, a_assertContext);
    auto atomicDurabilityResult = atomicDurability.write_file_atomic(*path.try_value(), bytes);
    if (!has_io_error(atomicDurabilityResult, cue::IoError::DurabilityUnknown) ||
        atomicDurability.has_original_file() || atomicDurability.has_temporary())
    {
        return false;
    }

    constexpr std::array stagingFailures{FailurePoint::ValidateStaged, FailurePoint::PrePublishReparseValidation,
                                         FailurePoint::Publish};
    for (const FailurePoint failure : stagingFailures)
    {
        FailingFilesystemRoot filesystem(failure, a_assertContext);
        auto staging = filesystem.create_staging_area(*path.try_value());
        auto result = filesystem.publish_staging_area(std::move(*staging.try_value()), *path.try_value());
        if (!has_io_error(result, cue::IoError::IoFailure) || !filesystem.has_staging() ||
            filesystem.is_project_published() || !filesystem.rollback_staging_area(std::move(*staging.try_value())))
        {
            return false;
        }
    }

    FailingFilesystemRoot stagingDurability(FailurePoint::DirectoryFlush, a_assertContext);
    auto staging = stagingDurability.create_staging_area(*path.try_value());
    auto stagingDurabilityResult =
        stagingDurability.publish_staging_area(std::move(*staging.try_value()), *path.try_value());
    if (!has_io_error(stagingDurabilityResult, cue::IoError::DurabilityUnknown) || stagingDurability.has_staging() ||
        !stagingDurability.is_project_published())
    {
        return false;
    }

    FailingFilesystemRoot rollback(FailurePoint::RollbackRemove, a_assertContext);
    auto rollbackStaging = rollback.create_staging_area(*path.try_value());
    auto rollbackResult = rollback.rollback_staging_area(std::move(*rollbackStaging.try_value()));
    return has_io_error(rollbackResult, cue::IoError::IoFailure) && rollback.has_staging();
}

/// @brief Windows Filesystem Root が相対 Path を現在 Directory 基準へ暗黙展開しないことを検証する
[[nodiscard]] bool test_root_factory_validation(const cue::AssertContext &a_assertContext)
{
    auto relative = cue::create_windows_filesystem_root("RelativeRoot", a_assertContext);
    auto driveRelative = cue::create_windows_filesystem_root("C:RelativeRoot", a_assertContext);
    return has_io_error(relative, cue::IoError::InvalidPath) && has_io_error(driveRelative, cue::IoError::InvalidPath);
}

/// @brief LocalApplicationData配下のWorkspace Rootを作成後にOpenExistingで再利用できるか検証する
[[nodiscard]] bool test_known_folder_root(const cue::AssertContext &a_assertContext)
{
    KnownFolderTestDirectory directory;
    if (!directory.is_ready())
    {
        return false;
    }
    auto relative = cue::RelativePath::parse(directory.relative_path(), a_assertContext);
    if (!relative)
    {
        return false;
    }
    auto created = cue::create_windows_known_folder_filesystem_root(
        cue::WindowsKnownFolder::LocalApplicationData, *relative.try_value(),
        cue::WindowsRootOpenMode::CreateOrOpen, a_assertContext);
    if (!created)
    {
        return false;
    }
    auto probePath = cue::RelativePath::parse("AtomicWriteProbe.bin", a_assertContext);
    constexpr std::array<std::byte, 3> k_probeBytes{std::byte{0x43}, std::byte{0x55}, std::byte{0x45}};
    if (!probePath || !(**created.try_value()).write_file_atomic(*probePath.try_value(), k_probeBytes) ||
        !(**created.try_value()).remove_file(*probePath.try_value()))
    {
        return false;
    }
    created.try_value()->reset();

    auto reopened = cue::create_windows_known_folder_filesystem_root(
        cue::WindowsKnownFolder::LocalApplicationData, *relative.try_value(),
        cue::WindowsRootOpenMode::OpenExisting, a_assertContext);
    if (!reopened || *reopened.try_value() == nullptr || !directory.is_replacement_blocked())
    {
        return false;
    }
    reopened.try_value()->reset();
    return directory.remove();
}
} // namespace

/// @brief Portable Path、Windows Storage、Reparse 拒否、Failure Injection 契約を統合検証する
int main()
{
    TestFatalHandler fatalHandler;
    std::vector<std::unique_ptr<cue::LogSink>> sinks;
    cue::Logger logger(fatalHandler, std::move(sinks));
    cue::AssertContext assertContext(logger, fatalHandler);
    TestDirectory directory;
    if (!directory.is_created() || !test_relative_paths(assertContext) || !test_failure_injection(assertContext) ||
        !test_root_factory_validation(assertContext) || !test_known_folder_root(assertContext))
    {
        return 1;
    }

    auto filesystem = cue::create_windows_filesystem_root(directory.utf8_path(), assertContext);
    if (!filesystem)
    {
        return 2;
    }

    if (!test_windows_file_operations(**filesystem.try_value(), directory, assertContext))
    {
        return 3;
    }
    if (!test_windows_staging(**filesystem.try_value(), assertContext))
    {
        return 4;
    }
    if (!test_reparse_rejection(**filesystem.try_value(), directory, assertContext))
    {
        return 5;
    }
    if (!test_staging_reparse_root(**filesystem.try_value(), directory, assertContext))
    {
        return 6;
    }
    return test_staging_identity_replacement(**filesystem.try_value(), directory, assertContext) ? 0 : 7;
}
