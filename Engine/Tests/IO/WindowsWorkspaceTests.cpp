#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Fatal.h>
#include <Cue/Foundation/Log.h>
#include <Cue/IO/Error.h>
#include <Cue/IO/Windows/WindowsWorkspaceFilesystem.h>

#include <Aclapi.h>
#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
constexpr std::string_view k_deepLocator = "D/D/D/D/D/D/D/D/D/D/D/D/D/D/D/D";

/// @brief Test中の回復不能失敗を終了Codeへ変換する
class TestFatalHandler final : public cue::FatalHandler
{
  public:
    /// @brief MessageなしFatalをTest終了Codeへ変換する
    [[noreturn]] void terminate() noexcept override
    {
        std::_Exit(90);
    }

    /// @brief Message付きFatalをTest終了Codeへ変換する
    [[noreturn]] void terminate(std::string_view) noexcept override
    {
        std::_Exit(91);
    }
};

/// @brief Windows Workspace Test専用Directoryを作成して限定Cleanupする
class TestDirectory final
{
  public:
    /// @brief ProcessとTickから一意なRootおよびRoot外Directoryを作成する
    TestDirectory()
    {
        std::array<wchar_t, MAX_PATH> temporary{};
        const DWORD length = GetTempPathW(static_cast<DWORD>(temporary.size()), temporary.data());
        if (length == 0U || length >= temporary.size())
        {
            return;
        }
        m_path = temporary.data();
        m_path +=
            L"CueWorkspaceTests-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
        m_outsidePath = m_path + L"-Outside";
        m_movedPath = m_path + L"-Moved";
        m_created = CreateDirectoryW(m_path.c_str(), nullptr) != FALSE &&
                    CreateDirectoryW(m_outsidePath.c_str(), nullptr) != FALSE;
    }
    /// @brief Test Directoryの一意Cleanup責務を保つためCopy構築を禁止する
    TestDirectory(const TestDirectory &) = delete;
    /// @brief Test Directoryの一意Cleanup責務を保つためCopy代入を禁止する
    TestDirectory &operator=(const TestDirectory &) = delete;
    /// @brief Test DirectoryのPathを固定するためMove構築を禁止する
    TestDirectory(TestDirectory &&) = delete;
    /// @brief Test DirectoryのPathを固定するためMove代入を禁止する
    TestDirectory &operator=(TestDirectory &&) = delete;
    /// @brief Test専用DirectoryだけをBest-effortで再帰削除する
    ~TestDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(m_path, error);
        std::filesystem::remove_all(m_movedPath, error);
        std::filesystem::remove_all(m_outsidePath, error);
    }

    /// @brief Test Directory作成に成功したか返す
    [[nodiscard]] bool is_created() const noexcept
    {
        return m_created;
    }

    /// @brief Factoryへ渡すRoot UTF-8 Pathを返す
    [[nodiscard]] std::string utf8_path() const
    {
        const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, m_path.c_str(),
                                              static_cast<int>(m_path.size()), nullptr, 0, nullptr, nullptr);
        std::string result(static_cast<std::size_t>(count), '\0');
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, m_path.c_str(), static_cast<int>(m_path.size()),
                            result.data(), count, nullptr, nullptr);
        return result;
    }

    /// @brief Root配下のNative Pathを返す
    [[nodiscard]] std::wstring child(std::wstring_view a_relative) const
    {
        return m_path + L"\\" + std::wstring(a_relative);
    }

    /// @brief Root外Test Directory Pathを返す
    [[nodiscard]] const std::wstring &outside_path() const noexcept
    {
        return m_outsidePath;
    }

    /// @brief Root置換試行先Pathを返す
    [[nodiscard]] const std::wstring &moved_path() const noexcept
    {
        return m_movedPath;
    }

    /// @brief Root Native Pathを返す
    [[nodiscard]] const std::wstring &path() const noexcept
    {
        return m_path;
    }

  private:
    std::wstring m_path;
    std::wstring m_outsidePath;
    std::wstring m_movedPath;
    bool m_created = false;
};

/// @brief Test FileのFILE_READ_ATTRIBUTES拒否DACLを所有して終了時に復元する
class ScopedAttributeDenial final
{
  public:
    /// @brief EveryoneへFILE_READ_ATTRIBUTES拒否を追加する
    explicit ScopedAttributeDenial(std::wstring a_path) : m_path(std::move(a_path))
    {
        std::array<std::byte, SECURITY_MAX_SID_SIZE> sidStorage{};
        DWORD sidSize = static_cast<DWORD>(sidStorage.size());
        if (CreateWellKnownSid(WinWorldSid, nullptr, sidStorage.data(), &sidSize) == FALSE)
        {
            return;
        }
        const DWORD queried = GetNamedSecurityInfoW(m_path.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr,
                                                    nullptr, &m_originalDacl, nullptr, &m_securityDescriptor);
        if (queried != ERROR_SUCCESS)
        {
            return;
        }

        EXPLICIT_ACCESSW access{};
        access.grfAccessPermissions = FILE_READ_ATTRIBUTES;
        access.grfAccessMode = DENY_ACCESS;
        access.grfInheritance = NO_INHERITANCE;
        access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        access.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
        access.Trustee.ptstrName = static_cast<LPWSTR>(static_cast<void *>(sidStorage.data()));
        const DWORD built = SetEntriesInAclW(1U, &access, m_originalDacl, &m_deniedDacl);
        if (built != ERROR_SUCCESS)
        {
            return;
        }
        m_isApplied = SetNamedSecurityInfoW(m_path.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr,
                                            m_deniedDacl, nullptr) == ERROR_SUCCESS;
    }
    ScopedAttributeDenial(const ScopedAttributeDenial &) = delete;
    ScopedAttributeDenial &operator=(const ScopedAttributeDenial &) = delete;
    ScopedAttributeDenial(ScopedAttributeDenial &&) = delete;
    ScopedAttributeDenial &operator=(ScopedAttributeDenial &&) = delete;
    /// @brief 元DACLを復元してSecurity Bufferを解放する
    ~ScopedAttributeDenial()
    {
        (void)restore();
        if (m_deniedDacl != nullptr)
        {
            LocalFree(m_deniedDacl);
        }
        if (m_securityDescriptor != nullptr)
        {
            LocalFree(m_securityDescriptor);
        }
    }

    /// @brief 拒否DACLの適用に成功したか返す
    [[nodiscard]] bool is_applied() const noexcept
    {
        return m_isApplied;
    }

    /// @brief 元DACLを一度だけ復元する
    [[nodiscard]] bool restore() noexcept
    {
        if (!m_isApplied)
        {
            return true;
        }
        const DWORD restored = SetNamedSecurityInfoW(m_path.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr,
                                                     nullptr, m_originalDacl, nullptr);
        if (restored == ERROR_SUCCESS)
        {
            m_isApplied = false;
            return true;
        }
        return false;
    }

  private:
    std::wstring m_path;
    PSECURITY_DESCRIPTOR m_securityDescriptor = nullptr;
    PACL m_originalDacl = nullptr;
    PACL m_deniedDacl = nullptr;
    bool m_isApplied = false;
};

/// @brief Resultが指定Portable IO分類を保持するか判定する
template <typename T> [[nodiscard]] bool has_io_error(cue::Result<T> &a_result, cue::IoError a_code) noexcept
{
    const cue::Error *error = a_result.try_error();
    return error != nullptr && error->code().domain() == "Cue.IO" &&
           error->code().value() == static_cast<std::int64_t>(a_code);
}

/// @brief Native Test Fileへ全Byteを書き込む
[[nodiscard]] bool write_file(std::wstring_view a_path, std::string_view a_bytes)
{
    const std::wstring path(a_path);
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return false;
    }
    DWORD written = 0U;
    const BOOL succeeded = WriteFile(file, a_bytes.data(), static_cast<DWORD>(a_bytes.size()), &written, nullptr);
    CloseHandle(file);
    return succeeded != FALSE && written == static_cast<DWORD>(a_bytes.size());
}

/// @brief Directory Namespaceで大文字小文字だけ異なるEntryを個別作成できるようにする
[[nodiscard]] bool enable_case_sensitive_directory(std::wstring_view a_path) noexcept
{
    const std::wstring path(a_path);
    HANDLE directory = CreateFileW(path.c_str(), FILE_LIST_DIRECTORY | FILE_WRITE_ATTRIBUTES,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                   FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (directory == INVALID_HANDLE_VALUE)
    {
        return false;
    }
    FILE_CASE_SENSITIVE_INFO information{};
    information.Flags = FILE_CS_FLAG_CASE_SENSITIVE_DIR;
    const bool succeeded =
        SetFileInformationByHandle(directory, FileCaseSensitiveInfo, &information, sizeof(information)) != FALSE;
    CloseHandle(directory);
    return succeeded;
}

/// @brief Junction用Mount Point Reparse BufferのNative Layoutを表す
struct MountPointReparseBuffer final
{
    DWORD reparseTag = IO_REPARSE_TAG_MOUNT_POINT;
    WORD reparseDataLength = 0U;
    WORD reserved = 0U;
    WORD substituteNameOffset = 0U;
    WORD substituteNameLength = 0U;
    WORD printNameOffset = 0U;
    WORD printNameLength = 0U;
    wchar_t pathBuffer[1]{};
};

/// @brief 特権不要のDirectory Junctionを作成してReparse Point Fixtureを保証する
[[nodiscard]] bool create_directory_junction(std::wstring_view a_linkPath, std::wstring_view a_targetPath)
{
    const std::wstring linkPath(a_linkPath);
    if (CreateDirectoryW(linkPath.c_str(), nullptr) == FALSE)
    {
        return false;
    }

    const std::wstring substituteName = L"\\??\\" + std::wstring(a_targetPath);
    const std::wstring printName(a_targetPath);
    const std::size_t substituteBytes = substituteName.size() * sizeof(wchar_t);
    const std::size_t printBytes = printName.size() * sizeof(wchar_t);
    const std::size_t pathBytes = substituteBytes + sizeof(wchar_t) + printBytes + sizeof(wchar_t);
    const std::size_t totalBytes = offsetof(MountPointReparseBuffer, pathBuffer) + pathBytes;
    if (substituteBytes > MAXWORD || printBytes > MAXWORD || pathBytes + 8U > MAXWORD || totalBytes > MAXDWORD)
    {
        RemoveDirectoryW(linkPath.c_str());
        return false;
    }

    std::vector<std::uint32_t> storage((totalBytes + sizeof(std::uint32_t) - 1U) / sizeof(std::uint32_t), 0U);
    auto *buffer = reinterpret_cast<MountPointReparseBuffer *>(storage.data());
    buffer->reparseTag = IO_REPARSE_TAG_MOUNT_POINT;
    buffer->reparseDataLength = static_cast<WORD>(pathBytes + 8U);
    buffer->substituteNameOffset = 0U;
    buffer->substituteNameLength = static_cast<WORD>(substituteBytes);
    buffer->printNameOffset = static_cast<WORD>(substituteBytes + sizeof(wchar_t));
    buffer->printNameLength = static_cast<WORD>(printBytes);
    std::memcpy(buffer->pathBuffer, substituteName.data(), substituteBytes);
    std::memcpy(reinterpret_cast<std::byte *>(buffer->pathBuffer) + buffer->printNameOffset, printName.data(),
                printBytes);

    HANDLE link = CreateFileW(linkPath.c_str(), GENERIC_WRITE, 0U, nullptr, OPEN_EXISTING,
                              FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (link == INVALID_HANDLE_VALUE)
    {
        RemoveDirectoryW(linkPath.c_str());
        return false;
    }
    DWORD returned = 0U;
    const BOOL succeeded = DeviceIoControl(link, FSCTL_SET_REPARSE_POINT, buffer, static_cast<DWORD>(totalBytes),
                                           nullptr, 0U, &returned, nullptr);
    CloseHandle(link);
    if (succeeded == FALSE)
    {
        RemoveDirectoryW(linkPath.c_str());
        return false;
    }
    return true;
}

/// @brief RelativePathの最大Segment数までNative Directory Fixtureを作成する
[[nodiscard]] bool create_deep_directory_fixture(const TestDirectory &a_directory)
{
    std::wstring path = a_directory.path();
    for (std::size_t index = 0U; index < 16U; ++index)
    {
        path += L"\\D";
        if (CreateDirectoryW(path.c_str(), nullptr) == FALSE)
        {
            return false;
        }
    }
    return write_file(path + L"\\Tail.bin", "tail");
}

/// @brief Snapshotから表示名が一致するEntryを返す
[[nodiscard]] const cue::WorkspaceEntry *find_entry(const cue::DirectorySnapshot &a_snapshot,
                                                    std::string_view a_name) noexcept
{
    for (const cue::WorkspaceEntry &entry : a_snapshot.entries)
    {
        if (entry.displayName == a_name)
        {
            return &entry;
        }
    }
    return nullptr;
}

/// @brief Directory、File、Unsupported Entryの決定的列挙を検証する
[[nodiscard]] bool test_listing(cue::WorkspaceFilesystem &a_filesystem, const cue::AssertContext &a_assertContext)
{
    const cue::TraversalLimits limits{8U, 64U, 64U, 16U * 1024U};
    auto first = a_filesystem.list_directory(cue::WorkspaceDirectory::root(), limits);
    auto second = a_filesystem.list_directory(cue::WorkspaceDirectory::root(), limits);
    if (!first || !second || first.try_value()->entries.size() != second.try_value()->entries.size() ||
        first.try_value()->generation == second.try_value()->generation)
    {
        return false;
    }
    for (std::size_t index = 0U; index < first.try_value()->entries.size(); ++index)
    {
        if (first.try_value()->entries[index].displayName != second.try_value()->entries[index].displayName ||
            first.try_value()->entries[index].parentGeneration != first.try_value()->generation)
        {
            return false;
        }
    }

    const cue::WorkspaceEntry *folder = find_entry(*first.try_value(), "Folder");
    const cue::WorkspaceEntry *alpha = find_entry(*first.try_value(), "alpha.bin");
    const cue::WorkspaceEntry *zeta = find_entry(*first.try_value(), "Zeta.bin");
    const cue::WorkspaceEntry *hidden = find_entry(*first.try_value(), ".Hidden");
    const cue::WorkspaceEntry *reparse = find_entry(*first.try_value(), "ReparseLink");
    if (folder == nullptr || alpha == nullptr || zeta == nullptr || hidden == nullptr || !folder->is_operable() ||
        folder->type != cue::WorkspaceEntryType::Directory || alpha->byteSize != 5U || zeta->byteSize != 4U ||
        hidden->is_operable() || hidden->rejection != cue::WorkspaceDiagnosticCode::UnsupportedName ||
        reparse == nullptr || reparse->is_operable() ||
        reparse->rejection != cue::WorkspaceDiagnosticCode::ReparsePoint)
    {
        return false;
    }

    std::vector<std::string> names;
    for (const cue::WorkspaceEntry &entry : first.try_value()->entries)
    {
        names.push_back(entry.displayName);
    }
    const auto folderPosition = std::find(names.begin(), names.end(), "Folder");
    const auto alphaPosition = std::find(names.begin(), names.end(), "alpha.bin");
    const auto zetaPosition = std::find(names.begin(), names.end(), "Zeta.bin");
    const auto hiddenPosition = std::find(names.begin(), names.end(), ".Hidden");
    if (folderPosition == names.end() || alphaPosition == names.end() || zetaPosition == names.end() ||
        hiddenPosition == names.end() ||
        !(folderPosition < alphaPosition && alphaPosition < zetaPosition && zetaPosition < hiddenPosition))
    {
        return false;
    }

    auto folderLocator = cue::RelativePath::parse("Folder", a_assertContext);
    if (!folderLocator)
    {
        return false;
    }
    auto folderDirectory = a_filesystem.bind_directory(std::move(*folderLocator.try_value()), a_assertContext);
    if (!folderDirectory)
    {
        return false;
    }
    auto nested = a_filesystem.list_directory(*folderDirectory.try_value(), limits);
    return nested && nested.try_value()->entries.size() == 1U &&
           nested.try_value()->entries[0].displayName == "Needle.txt";
}

/// @brief Bounded SearchがReparse PointをTraversalせず各上限を拒否するか検証する
[[nodiscard]] bool test_search(cue::WorkspaceFilesystem &a_filesystem, const cue::AssertContext &a_assertContext)
{
    auto found = cue::search_workspace(a_filesystem, cue::WorkspaceDirectory::root(), "needle",
                                       cue::TraversalLimits{32U, 64U, 64U, 16U * 1024U}, a_assertContext);
    if (!found || found.try_value()->entries.size() != 1U || found.try_value()->entries[0].displayName != "Needle.txt")
    {
        return false;
    }
    auto depth = cue::search_workspace(a_filesystem, cue::WorkspaceDirectory::root(), {},
                                       cue::TraversalLimits{1U, 64U, 64U, 16U * 1024U}, a_assertContext);
    auto visited = cue::search_workspace(a_filesystem, cue::WorkspaceDirectory::root(), {},
                                         cue::TraversalLimits{8U, 1U, 64U, 16U * 1024U}, a_assertContext);
    auto results = cue::search_workspace(a_filesystem, cue::WorkspaceDirectory::root(), {},
                                         cue::TraversalLimits{8U, 64U, 1U, 16U * 1024U}, a_assertContext);
    auto metadata = cue::search_workspace(a_filesystem, cue::WorkspaceDirectory::root(), {},
                                          cue::TraversalLimits{8U, 64U, 64U, 1U}, a_assertContext);
    if (!has_io_error(depth, cue::IoError::CapacityExceeded) ||
        !has_io_error(visited, cue::IoError::CapacityExceeded) ||
        !has_io_error(results, cue::IoError::CapacityExceeded) ||
        !has_io_error(metadata, cue::IoError::CapacityExceeded))
    {
        return false;
    }
    auto outside = cue::search_workspace(a_filesystem, cue::WorkspaceDirectory::root(), "OutsideSecret",
                                         cue::TraversalLimits{32U, 64U, 64U, 16U * 1024U}, a_assertContext);
    if (!outside || !outside.try_value()->entries.empty())
    {
        return false;
    }
    auto linkLocator = cue::RelativePath::parse("ReparseLink", a_assertContext);
    if (!linkLocator)
    {
        return false;
    }
    auto linkDirectory = a_filesystem.bind_directory(std::move(*linkLocator.try_value()), a_assertContext);
    if (!linkDirectory)
    {
        return false;
    }
    auto escaped = a_filesystem.list_directory(*linkDirectory.try_value(), cue::TraversalLimits{2U, 4U, 4U, 1024U});
    if (!has_io_error(escaped, cue::IoError::UnsupportedEntry))
    {
        return false;
    }
    return true;
}

/// @brief Share ViolationをEntry単位Permission診断とRescan要求へ変換するか検証する
[[nodiscard]] bool test_access_denied(cue::WorkspaceFilesystem &a_filesystem, const TestDirectory &a_directory)
{
    const std::wstring lockedPath = a_directory.child(L"Locked.bin");
    if (!write_file(lockedPath, "locked"))
    {
        return false;
    }
    ScopedAttributeDenial denied(lockedPath);
    if (!denied.is_applied())
    {
        return false;
    }
    auto snapshot =
        a_filesystem.list_directory(cue::WorkspaceDirectory::root(), cue::TraversalLimits{4U, 64U, 64U, 16U * 1024U});
    const bool restored = denied.restore();
    if (!snapshot || snapshot.try_value()->state != cue::WorkspaceSnapshotState::RescanRequired)
    {
        return false;
    }
    const cue::WorkspaceEntry *entry = find_entry(*snapshot.try_value(), "Locked.bin");
    if (entry == nullptr || entry->rejection != cue::WorkspaceDiagnosticCode::PermissionDenied)
    {
        return false;
    }
    for (const cue::WorkspaceDiagnostic &diagnostic : snapshot.try_value()->diagnostics)
    {
        if (diagnostic.displayName == "Locked.bin" && diagnostic.code == cue::WorkspaceDiagnosticCode::PermissionDenied)
        {
            return restored;
        }
    }
    return false;
}

/// @brief Workspace Factoryが絶対DirectoryだけをRootとして受理するか検証する
[[nodiscard]] bool test_factory_validation(const TestDirectory &a_directory, const cue::AssertContext &a_assertContext)
{
    auto relative = cue::create_windows_workspace_filesystem("RelativeRoot", a_assertContext);
    auto unc = cue::create_windows_workspace_filesystem("\\\\server\\share", a_assertContext);
    auto mixedCaseExtendedUnc = cue::create_windows_workspace_filesystem("\\\\?\\UnC\\server\\share", a_assertContext);
    const std::string extendedPath = "\\\\?\\" + a_directory.utf8_path();
    auto extended = cue::create_windows_workspace_filesystem(extendedPath, a_assertContext);
    const std::string extendedDriveRoot = "\\\\?\\" + a_directory.utf8_path().substr(0U, 3U);
    auto driveRoot = cue::create_windows_workspace_filesystem(extendedDriveRoot, a_assertContext);
    const std::string filePath = a_directory.utf8_path() + "/alpha.bin";
    auto file = cue::create_windows_workspace_filesystem(filePath, a_assertContext);
    std::string embeddedNul = a_directory.utf8_path();
    embeddedNul.push_back('\0');
    embeddedNul.append("Ignored");
    auto nul = cue::create_windows_workspace_filesystem(embeddedNul, a_assertContext);
    return has_io_error(relative, cue::IoError::InvalidPath) && has_io_error(unc, cue::IoError::UnsupportedEntry) &&
           has_io_error(mixedCaseExtendedUnc, cue::IoError::UnsupportedEntry) && extended && driveRoot &&
           has_io_error(file, cue::IoError::TypeMismatch) && has_io_error(nul, cue::IoError::InvalidPath);
}

/// @brief User Locator上限のDirectory直下を内部Bound Pathで列挙できるか検証する
[[nodiscard]] bool test_deep_bound_path(cue::WorkspaceFilesystem &a_filesystem,
                                        const cue::AssertContext &a_assertContext)
{
    auto locator = cue::RelativePath::parse(k_deepLocator, a_assertContext);
    if (!locator)
    {
        return false;
    }
    auto directory = a_filesystem.bind_directory(std::move(*locator.try_value()), a_assertContext);
    if (!directory)
    {
        return false;
    }
    auto listed = a_filesystem.list_directory(*directory.try_value(), cue::TraversalLimits{2U, 8U, 8U, 4096U});
    if (!listed || listed.try_value()->entries.size() != 1U)
    {
        return false;
    }
    const cue::WorkspaceEntry &entry = listed.try_value()->entries[0];
    const std::string expected = std::string(k_deepLocator) + "/Tail.bin";
    return entry.is_operable() && entry.locator->text() == expected;
}

/// @brief 個別には有効な16 Segment RootとChildをAbsolute Pathから内部PathへBindingできるか検証する
[[nodiscard]] bool test_deep_external_binding(cue::WorkspaceFilesystem &a_filesystem, const TestDirectory &a_directory)
{
    const std::string absolute = a_directory.utf8_path() + "/" + std::string(k_deepLocator) + "/Tail.bin";
    auto bound = a_filesystem.bind_external_path(absolute);
    return bound && bound.try_value()->text() == std::string(k_deepLocator) + "/Tail.bin";
}

/// @brief Case-sensitive親配下の大小文字Alias Rootを別Workspaceとして拒否するか検証する
[[nodiscard]] bool test_case_sensitive_root_alias(const TestDirectory &a_directory,
                                                  const cue::AssertContext &a_assertContext)
{
    const std::wstring parent = a_directory.child(L"CaseParent");
    const std::wstring canonicalRoot = parent + L"\\Project";
    const std::wstring aliasRoot = parent + L"\\project";
    if (CreateDirectoryW(parent.c_str(), nullptr) == FALSE || !enable_case_sensitive_directory(parent) ||
        CreateDirectoryW(canonicalRoot.c_str(), nullptr) == FALSE ||
        CreateDirectoryW(aliasRoot.c_str(), nullptr) == FALSE ||
        !write_file(canonicalRoot + L"\\Tail.bin", "canonical") || !write_file(aliasRoot + L"\\Tail.bin", "alias"))
    {
        return false;
    }
    auto filesystem =
        cue::create_windows_workspace_filesystem(std::filesystem::path(canonicalRoot).string(), a_assertContext);
    if (!filesystem)
    {
        return false;
    }
    auto selected =
        (**filesystem.try_value()).bind_external_path(std::filesystem::path(aliasRoot + L"\\Tail.bin").string());
    return has_io_error(selected, cue::IoError::OutsideRoot);
}

/// @brief 別Rootから発行されたDirectory Capabilityを拒否するか検証する
[[nodiscard]] bool test_binding_origin(cue::WorkspaceFilesystem &a_filesystem, const TestDirectory &a_directory,
                                       const cue::AssertContext &a_assertContext)
{
    auto otherFilesystem = cue::create_windows_workspace_filesystem(
        std::filesystem::path(a_directory.outside_path()).string(), a_assertContext);
    auto locator = cue::RelativePath::parse("Folder", a_assertContext);
    if (!otherFilesystem || !locator)
    {
        return false;
    }
    auto directory = a_filesystem.bind_directory(std::move(*locator.try_value()), a_assertContext);
    if (!directory)
    {
        return false;
    }
    auto listed =
        (**otherFilesystem.try_value()).list_directory(*directory.try_value(), cue::TraversalLimits{2U, 8U, 8U, 4096U});
    return has_io_error(listed, cue::IoError::OutsideRoot);
}

/// @brief Factory引数のAssertContext破棄後もWorkspaceが所有Copyを使用できるか検証する
[[nodiscard]] cue::Result<std::unique_ptr<cue::WorkspaceFilesystem>> create_with_temporary_context(
    std::string_view a_rootPath, cue::Logger &a_logger, cue::FatalHandler &a_fatalHandler) noexcept
{
    cue::AssertContext temporaryContext(a_logger, a_fatalHandler);
    return cue::create_windows_workspace_filesystem(a_rootPath, temporaryContext);
}

/// @brief Root直下から子DirectoryへのRenameが自己変更を親Chain競合と誤認しないことを検証する
[[nodiscard]] bool test_rename_into_descendant_parent(cue::WorkspaceFilesystem &a_filesystem,
                                                      const TestDirectory &a_directory,
                                                      const cue::AssertContext &a_assertContext) noexcept
{
    if (CreateDirectoryW(a_directory.child(L"MoveArea").c_str(), nullptr) == FALSE ||
        CreateDirectoryW(a_directory.child(L"MoveArea\\Child").c_str(), nullptr) == FALSE ||
        !write_file(a_directory.child(L"MoveArea\\MoveSource.txt"), "move"))
    {
        return false;
    }
    auto sourceLocator = cue::RelativePath::parse("MoveArea/MoveSource.txt", a_assertContext);
    auto destinationLocator = cue::RelativePath::parse("MoveArea/Child/MoveDestination.txt", a_assertContext);
    if (!sourceLocator || !destinationLocator)
    {
        return false;
    }
    auto source = a_filesystem.bind_root_path(std::move(*sourceLocator.try_value()), a_assertContext);
    auto destination = a_filesystem.bind_root_path(std::move(*destinationLocator.try_value()), a_assertContext);
    if (!source || !destination)
    {
        return false;
    }

    cue::WorkspaceMutationResult renamed = a_filesystem.rename_entry(*source.try_value(), *destination.try_value(),
                                                                     cue::TraversalLimits{8U, 64U, 64U, 16U * 1024U});
    const bool published = renamed.outcome == cue::WorkspaceMutationOutcome::Committed ||
                           renamed.outcome == cue::WorkspaceMutationOutcome::CommittedButDurabilityUnknown;
    return published &&
           GetFileAttributesW(a_directory.child(L"MoveArea\\MoveSource.txt").c_str()) == INVALID_FILE_ATTRIBUTES &&
           GetFileAttributesW(a_directory.child(L"MoveArea\\Child\\MoveDestination.txt").c_str()) !=
               INVALID_FILE_ATTRIBUTES;
}
} // namespace

/// @brief Windows Workspace列挙、Reparse拒否、競合診断、Root Pinを統合検証する
int main()
{
    TestFatalHandler fatalHandler;
    std::vector<std::unique_ptr<cue::LogSink>> sinks;
    cue::Logger logger(fatalHandler, std::move(sinks));
    cue::AssertContext assertContext(logger, fatalHandler);
    TestDirectory directory;
    if (!directory.is_created() || CreateDirectoryW(directory.child(L"Folder").c_str(), nullptr) == FALSE ||
        !write_file(directory.child(L"Folder\\Needle.txt"), "needle") ||
        !write_file(directory.child(L"alpha.bin"), "alpha") || !write_file(directory.child(L"Zeta.bin"), "zeta") ||
        !write_file(directory.child(L".Hidden"), "hidden") || !create_deep_directory_fixture(directory) ||
        !write_file(directory.outside_path() + L"\\OutsideSecret.bin", "secret"))
    {
        return 1;
    }

    const std::wstring linkPath = directory.child(L"ReparseLink");
    if (!create_directory_junction(linkPath, directory.outside_path()))
    {
        return 2;
    }

    if (!test_factory_validation(directory, assertContext))
    {
        return 3;
    }
    auto filesystem = cue::create_windows_workspace_filesystem(directory.utf8_path(), assertContext);
    if (!filesystem)
    {
        return 4;
    }
    if (!test_listing(**filesystem.try_value(), assertContext))
    {
        return 5;
    }
    if (!test_search(**filesystem.try_value(), assertContext))
    {
        return 6;
    }
    if (!test_access_denied(**filesystem.try_value(), directory))
    {
        return 7;
    }
    if (!test_deep_bound_path(**filesystem.try_value(), assertContext))
    {
        return 8;
    }
    if (!test_deep_external_binding(**filesystem.try_value(), directory))
    {
        return 9;
    }
    if (!test_binding_origin(**filesystem.try_value(), directory, assertContext))
    {
        return 10;
    }
    if (!test_case_sensitive_root_alias(directory, assertContext))
    {
        return 11;
    }

    auto temporaryContextFilesystem = create_with_temporary_context(directory.utf8_path(), logger, fatalHandler);
    if (!temporaryContextFilesystem)
    {
        return 12;
    }
    auto temporaryContextSnapshot =
        (**temporaryContextFilesystem.try_value())
            .list_directory(cue::WorkspaceDirectory::root(), cue::TraversalLimits{2U, 64U, 64U, 32U * 1024U});
    if (!temporaryContextSnapshot)
    {
        return 13;
    }

    if (!test_rename_into_descendant_parent(**filesystem.try_value(), directory, assertContext))
    {
        return 14;
    }

    auto limited = (**filesystem.try_value())
                       .list_directory(cue::WorkspaceDirectory::root(), cue::TraversalLimits{2U, 64U, 1U, 16U * 1024U});
    if (!has_io_error(limited, cue::IoError::CapacityExceeded))
    {
        return 15;
    }

    if (MoveFileExW(directory.path().c_str(), directory.moved_path().c_str(), 0U) != FALSE)
    {
        MoveFileExW(directory.moved_path().c_str(), directory.path().c_str(), 0U);
        return 16;
    }
    const DWORD replacementCode = GetLastError();
    return replacementCode == ERROR_SHARING_VIOLATION || replacementCode == ERROR_ACCESS_DENIED ? 0 : 17;
}
