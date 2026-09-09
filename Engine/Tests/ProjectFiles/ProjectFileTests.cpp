#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Fatal.h>
#include <Cue/Foundation/Log.h>
#include <Cue/IO/Error.h>
#include <Cue/IO/Windows/WindowsWorkspaceFilesystem.h>
#include <Cue/Project/Descriptor.h>
#include <Cue/ProjectFiles/Error.h>
#include <Cue/ProjectFiles/Service.h>

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
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

class TestDirectory final
{
  public:
    /// @brief 任意のRoot Case Policyを持つProject Files Integration Test用Directory Treeを作成する
    explicit TestDirectory(bool a_caseSensitiveRoot = false)
    {
        std::array<wchar_t, MAX_PATH> temporary{};
        const DWORD length = GetTempPathW(static_cast<DWORD>(temporary.size()), temporary.data());
        if (length == 0U || length >= temporary.size())
        {
            return;
        }
        static std::atomic_uint64_t sequence{0U};
        const std::uint64_t instance = sequence.fetch_add(1U, std::memory_order_relaxed);
        m_path = temporary.data();
        m_path += L"CueProjectFilesTests-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                  std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(instance);
        m_created = CreateDirectoryW(m_path.c_str(), nullptr) != FALSE;
        if (m_created && a_caseSensitiveRoot)
        {
            HANDLE directory =
                CreateFileW(m_path.c_str(), FILE_LIST_DIRECTORY | FILE_WRITE_ATTRIBUTES,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            FILE_CASE_SENSITIVE_INFO information{};
            information.Flags = FILE_CS_FLAG_CASE_SENSITIVE_DIR;
            m_created = directory != INVALID_HANDLE_VALUE &&
                        SetFileInformationByHandle(directory, FileCaseSensitiveInfo, &information,
                                                   sizeof(information)) != FALSE;
            if (directory != INVALID_HANDLE_VALUE)
            {
                CloseHandle(directory);
            }
        }
        m_created = m_created && CreateDirectoryW(child(L"Assets").c_str(), nullptr) != FALSE &&
                    CreateDirectoryW(child(L"Assets\\Source").c_str(), nullptr) != FALSE &&
                    CreateDirectoryW(child(L"Assets\\Runtime").c_str(), nullptr) != FALSE &&
                    CreateDirectoryW(child(L"Generated").c_str(), nullptr) != FALSE &&
                    CreateDirectoryW(child(L"Saved").c_str(), nullptr) != FALSE;
    }
    /// @brief Test Directory所有権の重複を防ぐためCopy構築を禁止する
    TestDirectory(const TestDirectory &) = delete;
    /// @brief Test Directory所有権の重複を防ぐためCopy代入を禁止する
    TestDirectory &operator=(const TestDirectory &) = delete;
    /// @brief Test終了時に作成済みDirectory Treeを削除する
    ~TestDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(m_path, error);
    }

    /// @brief Test Directory Treeが完全に作成されたか返す
    [[nodiscard]] bool is_created() const noexcept
    {
        return m_created;
    }

    /// @brief Test Root配下のNative Child Pathを返す
    [[nodiscard]] std::wstring child(std::wstring_view a_relative) const
    {
        return m_path + L"\\" + std::wstring(a_relative);
    }

    /// @brief Workspace Factoryへ渡すTest RootのUTF-8 Pathを返す
    [[nodiscard]] std::string utf8_path() const
    {
        const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, m_path.c_str(),
                                              static_cast<int>(m_path.size()), nullptr, 0, nullptr, nullptr);
        std::string result(static_cast<std::size_t>(count), '\0');
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, m_path.c_str(), static_cast<int>(m_path.size()),
                            result.data(), count, nullptr, nullptr);
        return result;
    }

  private:
    std::wstring m_path;
    bool m_created = false;
};

class SequenceOperationIdSource final : public cue::project_files::ProjectFileOperationIdSource
{
  public:
    /// @brief 決定的Operation ID列を所有するTest Sourceを構築する
    SequenceOperationIdSource(const cue::AssertContext &a_assertContext, std::vector<std::string> a_ids) noexcept
        : m_assertContext(&a_assertContext), m_ids(std::move(a_ids))
    {
    }
    /// @brief 所有するOperation ID列を解放する
    ~SequenceOperationIdSource() override = default;

    /// @brief 次の決定的Operation IDまたは枯渇Errorを返す
    [[nodiscard]] cue::Result<std::string> next_operation_id() noexcept override
    {
        if (m_next == m_ids.size())
        {
            return cue::Result<std::string>::failure(cue::project_files::make_project_file_error(
                *m_assertContext, cue::project_files::ProjectFileError::InvalidRequest,
                "Test operation id source is exhausted"));
        }
        return cue::Result<std::string>::success(std::move(m_ids[m_next++]));
    }

  private:
    const cue::AssertContext *m_assertContext;
    std::vector<std::string> m_ids;
    std::size_t m_next = 0U;
};

/// @brief Errorが指定ProjectFiles分類を保持するか判定する
[[nodiscard]] bool has_project_file_error(const cue::Error *a_error,
                                          cue::project_files::ProjectFileError a_code) noexcept
{
    return a_error != nullptr && a_error->code().domain() == "Cue.ProjectFiles" &&
           a_error->code().value() == static_cast<std::int64_t>(a_code);
}

/// @brief Test用Native Fileへ指定Byte列を書き込む
[[nodiscard]] bool write_file(std::wstring_view a_path, std::span<const std::byte> a_bytes) noexcept
{
    HANDLE file = CreateFileW(a_path.data(), GENERIC_WRITE, 0U, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return false;
    }
    DWORD written = 0U;
    const bool success =
        WriteFile(file, a_bytes.data(), static_cast<DWORD>(a_bytes.size()), &written, nullptr) != FALSE &&
        written == a_bytes.size();
    CloseHandle(file);
    return success;
}

/// @brief Test RootのCueProject.jsonへDescriptorを直列化して保存する
[[nodiscard]] bool write_project_descriptor(const TestDirectory &a_directory,
                                            const cue::ProjectDescriptor &a_descriptor,
                                            const cue::AssertContext &a_assertContext) noexcept
{
    auto serialized = cue::serialize_project_descriptor(a_descriptor, a_assertContext);
    return serialized &&
           write_file(a_directory.child(L"CueProject.json"),
                      std::as_bytes(std::span(serialized.try_value()->data(), serialized.try_value()->size())));
}

/// @brief Test DirectoryをCase-sensitive Namespaceへ変更する
[[nodiscard]] bool enable_case_sensitive_directory(std::wstring_view a_path) noexcept
{
    HANDLE directory = CreateFileW(a_path.data(), FILE_LIST_DIRECTORY | FILE_WRITE_ATTRIBUTES,
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

/// @brief Test用Native Fileを小容量Byte列として読み込む
[[nodiscard]] std::vector<std::byte> read_file(std::wstring_view a_path)
{
    std::vector<std::byte> bytes;
    HANDLE file = CreateFileW(a_path.data(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return bytes;
    }
    LARGE_INTEGER size{};
    if (GetFileSizeEx(file, &size) == FALSE || size.QuadPart < 0 || size.QuadPart > 1024)
    {
        CloseHandle(file);
        return bytes;
    }
    bytes.resize(static_cast<std::size_t>(size.QuadPart));
    DWORD read = 0U;
    if (!bytes.empty() && (ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) == FALSE ||
                           read != bytes.size()))
    {
        bytes.clear();
    }
    CloseHandle(file);
    return bytes;
}

/// @brief ProjectFiles Test用Descriptorを構築する
[[nodiscard]] cue::Result<cue::ProjectDescriptor> make_descriptor(const cue::AssertContext &a_assertContext) noexcept
{
    auto id = cue::ProjectId::parse("12345678-1234-4234-8234-123456789abc", a_assertContext);
    if (!id)
    {
        return cue::Result<cue::ProjectDescriptor>::failure(std::move(*id.try_error()));
    }
    return cue::create_blank_project_descriptor(
        *id.try_value(), "Project Files Test",
        cue::EngineCompatibility{cue::EngineVersion{1U, 0U, 0U}, cue::EngineVersion{2U, 0U, 0U}},
        "00000000-0000-4000-8000-000000000099", a_assertContext);
}

/// @brief 決定的Operation ID SourceをPolymorphic所有権で返す
[[nodiscard]] std::unique_ptr<cue::project_files::ProjectFileOperationIdSource> make_id_source(
    const cue::AssertContext &a_assertContext, std::vector<std::string> a_ids)
{
    return std::make_unique<SequenceOperationIdSource>(a_assertContext, std::move(a_ids));
}

/// @brief Operation-owned Staging Entryが残っていないか検証する
[[nodiscard]] bool has_no_staging_entries(const TestDirectory &a_directory)
{
    std::error_code error;
    for (const std::filesystem::directory_entry &entry :
         std::filesystem::directory_iterator(a_directory.child(L"Assets\\Source"), error))
    {
        const std::wstring name = entry.path().filename().wstring();
        if (name.find(L".cuefile-staging") != std::wstring::npos ||
            name.find(L".cuedir-staging") != std::wstring::npos || name.find(L"cuecopy-") != std::wstring::npos)
        {
            return false;
        }
    }
    return !error;
}

/// @brief Rename、Move、File Copy、Directory Copyと拒否条件を検証する
[[nodiscard]] bool test_transfer_operations(const cue::ProjectDescriptor &a_descriptor,
                                            const cue::AssertContext &a_assertContext)
{
    TestDirectory directory;
    const std::array<std::byte, 4U> content{std::byte{'d'}, std::byte{'a'}, std::byte{'t'}, std::byte{'a'}};
    const std::array<std::byte, 3U> nestedContent{std::byte{'s'}, std::byte{'u'}, std::byte{'b'}};
    if (!directory.is_created() || !write_project_descriptor(directory, a_descriptor, a_assertContext) ||
        CreateDirectoryW(directory.child(L"Assets\\Source\\Folder").c_str(), nullptr) == FALSE ||
        CreateDirectoryW(directory.child(L"Assets\\Source\\Other").c_str(), nullptr) == FALSE ||
        CreateDirectoryW(directory.child(L"Assets\\Source\\Tree").c_str(), nullptr) == FALSE ||
        CreateDirectoryW(directory.child(L"Assets\\Source\\Tree\\Nested").c_str(), nullptr) == FALSE ||
        CreateDirectoryW(directory.child(L"Assets\\Source\\Tree\\ZParent").c_str(), nullptr) == FALSE ||
        CreateDirectoryW(directory.child(L"Assets\\Source\\Tree\\ZParent\\AChild").c_str(), nullptr) == FALSE ||
        !write_file(directory.child(L"Assets\\Source\\Rename.txt"), content) ||
        !write_file(directory.child(L"Assets\\Source\\Case.txt"), content) ||
        !write_file(directory.child(L"Assets\\Source\\Folder\\Move.txt"), content) ||
        !write_file(directory.child(L"Assets\\Source\\Tree\\Root.bin"), content) ||
        !write_file(directory.child(L"Assets\\Source\\Tree\\Nested\\Child.bin"), nestedContent) ||
        !write_file(directory.child(L"Assets\\Source\\Tree\\ZParent\\AChild\\Deep.bin"), nestedContent) ||
        !write_file(directory.child(L"Assets\\Source\\Existing.bin"), nestedContent))
    {
        return false;
    }

    const std::wstring deepParent = L"Assets\\Source\\A\\B\\C\\D\\E\\F\\G\\H\\I\\J\\K\\L\\M\\N\\O";
    std::error_code deepParentError;
    std::filesystem::create_directories(directory.child(deepParent), deepParentError);
    if (deepParentError)
    {
        return false;
    }

    auto workspace = cue::create_windows_workspace_filesystem(directory.utf8_path(), a_assertContext);
    if (!workspace)
    {
        return false;
    }
    std::vector<std::string> ids{"10101010-1010-4010-8010-101010101010", "20202020-2020-4020-8020-202020202020",
                                 "30303030-3030-4030-8030-303030303030", "40404040-4040-4040-8040-404040404040",
                                 "50505050-5050-4050-8050-505050505050", "60606060-6060-4060-8060-606060606060",
                                 "70707070-7070-4070-8070-707070707070", "80808080-8080-4080-8080-808080808080",
                                 "90909090-9090-4090-8090-909090909090", "12121212-1212-4212-8212-121212121212",
                                 "13131313-1313-4313-8313-131313131313"};
    auto service = cue::project_files::ProjectFileService::create(a_descriptor, std::move(*workspace.try_value()),
                                                                  make_id_source(a_assertContext, std::move(ids)),
                                                                  a_assertContext);
    if (!service)
    {
        return false;
    }
    const cue::TraversalLimits traversal{16U, 1024U, 1024U, 1024U * 1024U};
    const cue::ContentVerificationLimits contentLimits{1024U * 1024U, 4U * 1024U * 1024U};

    auto source = cue::RelativePath::parse("Rename.txt", a_assertContext);
    auto destination = cue::RelativePath::parse("Renamed.txt", a_assertContext);
    auto renamed =
        service.try_value()->rename(cue::project_files::ProjectFileArea::SourceAssets, std::move(*source.try_value()),
                                    std::move(*destination.try_value()), traversal);
    if (!renamed || !renamed.try_value()->source().has_value() || *renamed.try_value()->source() != "Rename.txt" ||
        renamed.try_value()->destination() != "Renamed.txt" ||
        renamed.try_value()->kind() != cue::project_files::ProjectFileOperationKind::Rename ||
        renamed.try_value()->outcome() !=
            cue::project_files::ProjectFileOperationOutcome::CommittedButDurabilityUnknown ||
        GetFileAttributesW(directory.child(L"Assets\\Source\\Rename.txt").c_str()) != INVALID_FILE_ATTRIBUTES ||
        read_file(directory.child(L"Assets\\Source\\Renamed.txt")) !=
            std::vector<std::byte>(content.begin(), content.end()))
    {
        return false;
    }

    source = cue::RelativePath::parse("Case.txt", a_assertContext);
    destination = cue::RelativePath::parse("case.txt", a_assertContext);
    auto caseRenamed =
        service.try_value()->rename(cue::project_files::ProjectFileArea::SourceAssets, std::move(*source.try_value()),
                                    std::move(*destination.try_value()), traversal);
    WIN32_FIND_DATAW caseData{};
    HANDLE caseFind = FindFirstFileW(directory.child(L"Assets\\Source\\case.txt").c_str(), &caseData);
    if (caseFind != INVALID_HANDLE_VALUE)
    {
        FindClose(caseFind);
    }
    if (!caseRenamed ||
        caseRenamed.try_value()->outcome() !=
            cue::project_files::ProjectFileOperationOutcome::CommittedButDurabilityUnknown ||
        caseFind == INVALID_HANDLE_VALUE || std::wstring_view(caseData.cFileName) != L"case.txt")
    {
        return false;
    }

    source = cue::RelativePath::parse("Folder/Move.txt", a_assertContext);
    destination = cue::RelativePath::parse("Other/Moved.txt", a_assertContext);
    auto moved =
        service.try_value()->move(cue::project_files::ProjectFileArea::SourceAssets, std::move(*source.try_value()),
                                  std::move(*destination.try_value()), traversal);
    if (!moved ||
        moved.try_value()->outcome() !=
            cue::project_files::ProjectFileOperationOutcome::CommittedButDurabilityUnknown ||
        GetFileAttributesW(directory.child(L"Assets\\Source\\Folder\\Move.txt").c_str()) != INVALID_FILE_ATTRIBUTES ||
        read_file(directory.child(L"Assets\\Source\\Other\\Moved.txt")) !=
            std::vector<std::byte>(content.begin(), content.end()))
    {
        return false;
    }

    source = cue::RelativePath::parse("Renamed.txt", a_assertContext);
    destination = cue::RelativePath::parse("Copied.bin", a_assertContext);
    auto copied =
        service.try_value()->copy(cue::project_files::ProjectFileArea::SourceAssets, std::move(*source.try_value()),
                                  std::move(*destination.try_value()), traversal, contentLimits);
    if (!copied ||
        copied.try_value()->outcome() !=
            cue::project_files::ProjectFileOperationOutcome::CommittedButDurabilityUnknown ||
        read_file(directory.child(L"Assets\\Source\\Renamed.txt")) !=
            std::vector<std::byte>(content.begin(), content.end()) ||
        read_file(directory.child(L"Assets\\Source\\Copied.bin")) !=
            std::vector<std::byte>(content.begin(), content.end()))
    {
        return false;
    }

    source = cue::RelativePath::parse("Tree", a_assertContext);
    destination = cue::RelativePath::parse("CopiedTree", a_assertContext);
    auto treeCopied =
        service.try_value()->copy(cue::project_files::ProjectFileArea::SourceAssets, std::move(*source.try_value()),
                                  std::move(*destination.try_value()), traversal, contentLimits);
    if (!treeCopied ||
        treeCopied.try_value()->outcome() !=
            cue::project_files::ProjectFileOperationOutcome::CommittedButDurabilityUnknown ||
        read_file(directory.child(L"Assets\\Source\\CopiedTree\\Root.bin")) !=
            std::vector<std::byte>(content.begin(), content.end()) ||
        read_file(directory.child(L"Assets\\Source\\CopiedTree\\Nested\\Child.bin")) !=
            std::vector<std::byte>(nestedContent.begin(), nestedContent.end()) ||
        read_file(directory.child(L"Assets\\Source\\CopiedTree\\ZParent\\AChild\\Deep.bin")) !=
            std::vector<std::byte>(nestedContent.begin(), nestedContent.end()))
    {
        return false;
    }

    source = cue::RelativePath::parse("Tree", a_assertContext);
    destination = cue::RelativePath::parse("A/B/C/D/E/F/G/H/I/J/K/L/M/N/O/CopiedTree", a_assertContext);
    auto deepTreeCopied =
        service.try_value()->copy(cue::project_files::ProjectFileArea::SourceAssets, std::move(*source.try_value()),
                                  std::move(*destination.try_value()), traversal, contentLimits);
    if (!deepTreeCopied ||
        deepTreeCopied.try_value()->outcome() !=
            cue::project_files::ProjectFileOperationOutcome::CommittedButDurabilityUnknown ||
        read_file(directory.child(
            L"Assets\\Source\\A\\B\\C\\D\\E\\F\\G\\H\\I\\J\\K\\L\\M\\N\\O\\CopiedTree\\ZParent\\AChild\\Deep.bin")) !=
            std::vector<std::byte>(nestedContent.begin(), nestedContent.end()))
    {
        return false;
    }

    source = cue::RelativePath::parse("Tree", a_assertContext);
    destination = cue::RelativePath::parse("FailedTree", a_assertContext);
    auto failedTree =
        service.try_value()->copy(cue::project_files::ProjectFileArea::SourceAssets, std::move(*source.try_value()),
                                  std::move(*destination.try_value()), traversal, {2U, 32U});
    if (!failedTree ||
        failedTree.try_value()->outcome() != cue::project_files::ProjectFileOperationOutcome::NotCommitted ||
        !has_project_file_error(failedTree.try_value()->try_primary_error(),
                                cue::project_files::ProjectFileError::LimitExceeded) ||
        GetFileAttributesW(directory.child(L"Assets\\Source\\FailedTree").c_str()) != INVALID_FILE_ATTRIBUTES ||
        !has_no_staging_entries(directory))
    {
        return false;
    }

    source = cue::RelativePath::parse("Renamed.txt", a_assertContext);
    destination = cue::RelativePath::parse("Existing.bin", a_assertContext);
    auto conflict =
        service.try_value()->copy(cue::project_files::ProjectFileArea::SourceAssets, std::move(*source.try_value()),
                                  std::move(*destination.try_value()), traversal, contentLimits);
    if (!conflict || conflict.try_value()->outcome() != cue::project_files::ProjectFileOperationOutcome::NotCommitted ||
        !has_project_file_error(conflict.try_value()->try_primary_error(),
                                cue::project_files::ProjectFileError::Conflict) ||
        read_file(directory.child(L"Assets\\Source\\Existing.bin")) !=
            std::vector<std::byte>(nestedContent.begin(), nestedContent.end()))
    {
        return false;
    }

    source = cue::RelativePath::parse("Tree", a_assertContext);
    destination = cue::RelativePath::parse("Tree/Nested/Cycle", a_assertContext);
    auto cycle =
        service.try_value()->move(cue::project_files::ProjectFileArea::SourceAssets, std::move(*source.try_value()),
                                  std::move(*destination.try_value()), traversal);
    source = cue::RelativePath::parse("Copied.bin", a_assertContext);
    destination = cue::RelativePath::parse("Copied.bin", a_assertContext);
    auto same =
        service.try_value()->copy(cue::project_files::ProjectFileArea::SourceAssets, std::move(*source.try_value()),
                                  std::move(*destination.try_value()), traversal, contentLimits);
    source = cue::RelativePath::parse("Copied.bin", a_assertContext);
    destination = cue::RelativePath::parse("Limit.bin", a_assertContext);
    auto invalidLimits =
        service.try_value()->copy(cue::project_files::ProjectFileArea::SourceAssets, std::move(*source.try_value()),
                                  std::move(*destination.try_value()), traversal, {0U, 1U});
    return cycle && same && invalidLimits &&
           cycle.try_value()->outcome() == cue::project_files::ProjectFileOperationOutcome::NotCommitted &&
           same.try_value()->outcome() == cue::project_files::ProjectFileOperationOutcome::NotCommitted &&
           invalidLimits.try_value()->outcome() == cue::project_files::ProjectFileOperationOutcome::NotCommitted &&
           has_project_file_error(cycle.try_value()->try_primary_error(),
                                  cue::project_files::ProjectFileError::InvalidRequest) &&
           has_project_file_error(same.try_value()->try_primary_error(),
                                  cue::project_files::ProjectFileError::InvalidRequest) &&
           has_project_file_error(invalidLimits.try_value()->try_primary_error(),
                                  cue::project_files::ProjectFileError::InvalidRequest) &&
           has_no_staging_entries(directory);
}

/// @brief Create-new、Area Policy、Rollback、Thread契約を検証する
[[nodiscard]] bool test_create_and_policy(const TestDirectory &a_directory, const cue::ProjectDescriptor &a_descriptor,
                                          const cue::AssertContext &a_assertContext)
{
    auto workspace = cue::create_windows_workspace_filesystem(a_directory.utf8_path(), a_assertContext);
    if (!workspace)
    {
        return false;
    }
    std::vector<std::string> ids{"11111111-1111-4111-8111-111111111111", "22222222-2222-4222-8222-222222222222",
                                 "33333333-3333-4333-8333-333333333333", "44444444-4444-4444-8444-444444444444",
                                 "55555555-5555-4555-8555-555555555555", "88888888-8888-4888-8888-888888888888",
                                 "99999999-9999-4999-8999-999999999999"};
    auto serviceResult = cue::project_files::ProjectFileService::create(a_descriptor, std::move(*workspace.try_value()),
                                                                        make_id_source(a_assertContext, std::move(ids)),
                                                                        a_assertContext);
    if (!serviceResult)
    {
        return false;
    }
    cue::project_files::ProjectFileService service = std::move(*serviceResult.try_value());

    auto folderPath = cue::RelativePath::parse("CreatedFolder", a_assertContext);
    if (!folderPath)
    {
        return false;
    }
    auto folder =
        service.create_directory(cue::project_files::ProjectFileArea::SourceAssets, std::move(*folderPath.try_value()));
    if (!folder ||
        folder.try_value()->outcome() !=
            cue::project_files::ProjectFileOperationOutcome::CommittedButDurabilityUnknown ||
        folder.try_value()->operation_id() != "11111111-1111-4111-8111-111111111111" ||
        folder.try_value()->kind() != cue::project_files::ProjectFileOperationKind::DirectoryCreation ||
        folder.try_value()->area() != cue::project_files::ProjectFileArea::SourceAssets ||
        folder.try_value()->destination() != "CreatedFolder" ||
        folder.try_value()->stage() != cue::project_files::ProjectFileOperationStage::Verify ||
        !has_project_file_error(folder.try_value()->try_primary_error(),
                                cue::project_files::ProjectFileError::RecoveryRequired) ||
        !folder.try_value()->secondary_diagnostics().empty() || folder.try_value()->rescan_directories().size() != 1U ||
        !folder.try_value()->rescan_directories()[0U].empty() ||
        GetFileAttributesW(a_directory.child(L"Assets\\Source\\CreatedFolder").c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        return false;
    }

    folderPath = cue::RelativePath::parse("CreatedFolder", a_assertContext);
    auto folderConflict =
        service.create_directory(cue::project_files::ProjectFileArea::SourceAssets, std::move(*folderPath.try_value()));
    if (!folderConflict ||
        folderConflict.try_value()->outcome() != cue::project_files::ProjectFileOperationOutcome::NotCommitted ||
        !has_project_file_error(folderConflict.try_value()->try_primary_error(),
                                cue::project_files::ProjectFileError::Conflict))
    {
        return false;
    }

    folderPath = cue::RelativePath::parse("createdfolder", a_assertContext);
    auto portableCaseConflict =
        service.create_directory(cue::project_files::ProjectFileArea::SourceAssets, std::move(*folderPath.try_value()));
    if (!portableCaseConflict ||
        portableCaseConflict.try_value()->outcome() != cue::project_files::ProjectFileOperationOutcome::NotCommitted ||
        !has_project_file_error(portableCaseConflict.try_value()->try_primary_error(),
                                cue::project_files::ProjectFileError::Conflict))
    {
        return false;
    }

    const std::array<std::byte, 4U> newBytes{std::byte{'n'}, std::byte{'e'}, std::byte{'w'}, std::byte{'!'}};
    auto filePath = cue::RelativePath::parse("Created.bin", a_assertContext);
    auto file = service.create_file(cue::project_files::ProjectFileArea::SourceAssets, std::move(*filePath.try_value()),
                                    newBytes);
    if (!file ||
        file.try_value()->outcome() != cue::project_files::ProjectFileOperationOutcome::CommittedButDurabilityUnknown ||
        !has_project_file_error(file.try_value()->try_primary_error(),
                                cue::project_files::ProjectFileError::RecoveryRequired) ||
        read_file(a_directory.child(L"Assets\\Source\\Created.bin")) !=
            std::vector<std::byte>(newBytes.begin(), newBytes.end()))
    {
        return false;
    }

    const std::array<std::byte, 3U> oldBytes{std::byte{'o'}, std::byte{'l'}, std::byte{'d'}};
    if (!write_file(a_directory.child(L"Assets\\Source\\Keep.bin"), oldBytes))
    {
        return false;
    }
    filePath = cue::RelativePath::parse("Keep.bin", a_assertContext);
    auto fileConflict = service.create_file(cue::project_files::ProjectFileArea::SourceAssets,
                                            std::move(*filePath.try_value()), newBytes);
    if (!fileConflict ||
        fileConflict.try_value()->outcome() != cue::project_files::ProjectFileOperationOutcome::NotCommitted ||
        !has_project_file_error(fileConflict.try_value()->try_primary_error(),
                                cue::project_files::ProjectFileError::Conflict) ||
        read_file(a_directory.child(L"Assets\\Source\\Keep.bin")) !=
            std::vector<std::byte>(oldBytes.begin(), oldBytes.end()))
    {
        return false;
    }

    void *protectedMemory = VirtualAlloc(nullptr, 4096U, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (protectedMemory == nullptr)
    {
        return false;
    }
    auto *unreadableByte = std::construct_at(static_cast<std::byte *>(protectedMemory), std::byte{0x7f});
    DWORD oldProtection = 0U;
    if (VirtualProtect(protectedMemory, 4096U, PAGE_NOACCESS, &oldProtection) == FALSE)
    {
        VirtualFree(protectedMemory, 0U, MEM_RELEASE);
        return false;
    }
    filePath = cue::RelativePath::parse("WriteFailure.bin", a_assertContext);
    auto writeFailure =
        service.create_file(cue::project_files::ProjectFileArea::SourceAssets, std::move(*filePath.try_value()),
                            std::span<const std::byte>(unreadableByte, 1U));
    DWORD ignoredProtection = 0U;
    VirtualProtect(protectedMemory, 4096U, PAGE_READWRITE, &ignoredProtection);
    std::destroy_at(unreadableByte);
    VirtualFree(protectedMemory, 0U, MEM_RELEASE);
    if (!writeFailure ||
        writeFailure.try_value()->outcome() != cue::project_files::ProjectFileOperationOutcome::NotCommitted ||
        !has_project_file_error(writeFailure.try_value()->try_primary_error(),
                                cue::project_files::ProjectFileError::StorageFailure) ||
        GetFileAttributesW(a_directory.child(L"Assets\\Source\\WriteFailure.bin").c_str()) != INVALID_FILE_ATTRIBUTES ||
        !has_no_staging_entries(a_directory))
    {
        return false;
    }

    auto protectedPath = cue::RelativePath::parse("Blocked.bin", a_assertContext);
    auto protectedResult = service.create_file(cue::project_files::ProjectFileArea::RuntimeAssets,
                                               std::move(*protectedPath.try_value()), newBytes);
    bool wrongThreadRejected = false;
    std::thread wrongThread(
        /// @brief Owner Thread外からのService呼出し拒否を検証する
        [&]()
        {
            auto wrongThreadPath = cue::RelativePath::parse("WrongThread.bin", a_assertContext);
            auto wrongThreadResult = service.create_file(cue::project_files::ProjectFileArea::SourceAssets,
                                                         std::move(*wrongThreadPath.try_value()), newBytes);
            wrongThreadRejected =
                !wrongThreadResult && has_project_file_error(wrongThreadResult.try_error(),
                                                             cue::project_files::ProjectFileError::InvalidRequest);
        });
    wrongThread.join();

    return protectedResult && wrongThreadRejected &&
           protectedResult.try_value()->outcome() == cue::project_files::ProjectFileOperationOutcome::NotCommitted &&
           has_project_file_error(protectedResult.try_value()->try_primary_error(),
                                  cue::project_files::ProjectFileError::ProtectedEntry) &&
           GetFileAttributesW(a_directory.child(L"Assets\\Runtime\\Blocked.bin").c_str()) == INVALID_FILE_ATTRIBUTES &&
           has_no_staging_entries(a_directory);
}

/// @brief 二つのServiceによる同時Createが一方だけ成功することを検証する
[[nodiscard]] bool test_concurrent_conflict(const TestDirectory &a_directory,
                                            const cue::ProjectDescriptor &a_descriptor,
                                            const cue::AssertContext &a_assertContext)
{
    std::barrier start(2);
    std::array<int, 2U> outcomes{0, 0};
    const std::array<std::string_view, 2U> ids{"66666666-6666-4666-8666-666666666666",
                                               "77777777-7777-4777-8777-777777777777"};
    std::array<std::thread, 2U> workers;
    for (std::size_t index = 0U; index < workers.size(); ++index)
    {
        workers[index] = std::thread(
            /// @brief 同一Destinationへ独立Serviceから同時Createを実行する
            [&, index]()
            {
                auto workspace = cue::create_windows_workspace_filesystem(a_directory.utf8_path(), a_assertContext);
                std::vector<std::string> operationIds{std::string(ids[index])};
                if (!workspace)
                {
                    start.arrive_and_drop();
                    return;
                }
                auto service = cue::project_files::ProjectFileService::create(
                    a_descriptor, std::move(*workspace.try_value()),
                    make_id_source(a_assertContext, std::move(operationIds)), a_assertContext);
                auto locator = cue::RelativePath::parse("Concurrent.bin", a_assertContext);
                if (!service || !locator)
                {
                    start.arrive_and_drop();
                    return;
                }
                start.arrive_and_wait();
                const std::array<std::byte, 1U> content{std::byte{static_cast<unsigned char>('A' + index)}};
                auto result = service.try_value()->create_file(cue::project_files::ProjectFileArea::SourceAssets,
                                                               std::move(*locator.try_value()), content);
                if (!result)
                {
                    return;
                }
                if (result.try_value()->outcome() ==
                    cue::project_files::ProjectFileOperationOutcome::CommittedButDurabilityUnknown)
                {
                    outcomes[index] = 1;
                }
                else if (result.try_value()->outcome() ==
                             cue::project_files::ProjectFileOperationOutcome::NotCommitted &&
                         has_project_file_error(result.try_value()->try_primary_error(),
                                                cue::project_files::ProjectFileError::Conflict))
                {
                    outcomes[index] = 2;
                }
            });
    }
    for (std::thread &worker : workers)
    {
        worker.join();
    }
    return std::count(outcomes.begin(), outcomes.end(), 1) == 1 &&
           std::count(outcomes.begin(), outcomes.end(), 2) == 1 && has_no_staging_entries(a_directory);
}

/// @brief Source Assets Root欠落時にServiceを公開しないことを検証する
[[nodiscard]] bool test_missing_source_root(const cue::ProjectDescriptor &a_descriptor,
                                            const cue::AssertContext &a_assertContext)
{
    TestDirectory directory;
    if (!directory.is_created() || !write_project_descriptor(directory, a_descriptor, a_assertContext) ||
        RemoveDirectoryW(directory.child(L"Assets\\Source").c_str()) == FALSE)
    {
        return false;
    }
    auto workspace = cue::create_windows_workspace_filesystem(directory.utf8_path(), a_assertContext);
    if (!workspace)
    {
        return false;
    }
    std::vector<std::string> ids{"99999999-9999-4999-8999-999999999999"};
    auto service = cue::project_files::ProjectFileService::create(a_descriptor, std::move(*workspace.try_value()),
                                                                  make_id_source(a_assertContext, std::move(ids)),
                                                                  a_assertContext);
    return !service &&
           has_project_file_error(service.try_error(), cue::project_files::ProjectFileError::InvalidRequest);
}

/// @brief 不正Operation IDではMutationを開始しないことを検証する
[[nodiscard]] bool test_invalid_operation_id(const TestDirectory &a_directory,
                                             const cue::ProjectDescriptor &a_descriptor,
                                             const cue::AssertContext &a_assertContext)
{
    auto workspace = cue::create_windows_workspace_filesystem(a_directory.utf8_path(), a_assertContext);
    if (!workspace)
    {
        return false;
    }
    std::vector<std::string> ids{"not-an-operation-id"};
    auto service = cue::project_files::ProjectFileService::create(a_descriptor, std::move(*workspace.try_value()),
                                                                  make_id_source(a_assertContext, std::move(ids)),
                                                                  a_assertContext);
    auto destination = cue::RelativePath::parse("InvalidOperation.bin", a_assertContext);
    if (!service || !destination)
    {
        return false;
    }

    const std::array<std::byte, 1U> content{std::byte{0x01}};
    auto result = service.try_value()->create_file(cue::project_files::ProjectFileArea::SourceAssets,
                                                   std::move(*destination.try_value()), content);
    return !result &&
           has_project_file_error(result.try_error(), cue::project_files::ProjectFileError::InvalidRequest) &&
           GetFileAttributesW(a_directory.child(L"Assets\\Source\\InvalidOperation.bin").c_str()) ==
               INVALID_FILE_ATTRIBUTES &&
           has_no_staging_entries(a_directory);
}

/// @brief 親ComponentのPortable Case AliasをMutation前に拒否することを検証する
[[nodiscard]] bool test_portable_parent_alias(const cue::ProjectDescriptor &a_descriptor,
                                              const cue::AssertContext &a_assertContext)
{
    TestDirectory directory;
    if (!directory.is_created() || !write_project_descriptor(directory, a_descriptor, a_assertContext) ||
        !enable_case_sensitive_directory(directory.child(L"Assets\\Source")) ||
        CreateDirectoryW(directory.child(L"Assets\\Source\\Folder").c_str(), nullptr) == FALSE ||
        CreateDirectoryW(directory.child(L"Assets\\Source\\folder").c_str(), nullptr) == FALSE)
    {
        return false;
    }
    auto workspace = cue::create_windows_workspace_filesystem(directory.utf8_path(), a_assertContext);
    if (!workspace)
    {
        return false;
    }
    std::vector<std::string> ids{"aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"};
    auto service = cue::project_files::ProjectFileService::create(a_descriptor, std::move(*workspace.try_value()),
                                                                  make_id_source(a_assertContext, std::move(ids)),
                                                                  a_assertContext);
    auto destination = cue::RelativePath::parse("Folder/Alias.bin", a_assertContext);
    if (!service || !destination)
    {
        return false;
    }

    const std::array<std::byte, 1U> content{std::byte{0x01}};
    auto result = service.try_value()->create_file(cue::project_files::ProjectFileArea::SourceAssets,
                                                   std::move(*destination.try_value()), content);
    return result && result.try_value()->outcome() == cue::project_files::ProjectFileOperationOutcome::NotCommitted &&
           has_project_file_error(result.try_value()->try_primary_error(),
                                  cue::project_files::ProjectFileError::InvalidRequest) &&
           GetFileAttributesW(directory.child(L"Assets\\Source\\Folder\\Alias.bin").c_str()) ==
               INVALID_FILE_ATTRIBUTES &&
           GetFileAttributesW(directory.child(L"Assets\\Source\\folder\\Alias.bin").c_str()) == INVALID_FILE_ATTRIBUTES;
}

/// @brief Descriptorと異なるProject RootのWorkspaceをServiceが拒否することを検証する
[[nodiscard]] bool test_descriptor_workspace_mismatch(const cue::ProjectDescriptor &a_descriptor,
                                                      const cue::AssertContext &a_assertContext)
{
    TestDirectory directory;
    auto otherId = cue::ProjectId::parse("abcdefab-cdef-4abc-8def-abcdefabcdef", a_assertContext);
    if (!directory.is_created() || !otherId)
    {
        return false;
    }
    auto otherDescriptor = cue::create_blank_project_descriptor(
        *otherId.try_value(), "Other Project",
        cue::EngineCompatibility{cue::EngineVersion{1U, 0U, 0U}, cue::EngineVersion{2U, 0U, 0U}},
        "00000000-0000-4000-8000-000000000099", a_assertContext);
    if (!otherDescriptor || !write_project_descriptor(directory, *otherDescriptor.try_value(), a_assertContext))
    {
        return false;
    }
    auto workspace = cue::create_windows_workspace_filesystem(directory.utf8_path(), a_assertContext);
    if (!workspace)
    {
        return false;
    }
    std::vector<std::string> ids{"bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb"};
    auto service = cue::project_files::ProjectFileService::create(a_descriptor, std::move(*workspace.try_value()),
                                                                  make_id_source(a_assertContext, std::move(ids)),
                                                                  a_assertContext);
    return !service &&
           has_project_file_error(service.try_error(), cue::project_files::ProjectFileError::InvalidRequest);
}

/// @brief Source Root親ComponentのPortable Case AliasをService公開前に拒否することを検証する
[[nodiscard]] bool test_source_root_parent_alias(const cue::ProjectDescriptor &a_descriptor,
                                                 const cue::AssertContext &a_assertContext)
{
    TestDirectory directory(true);
    if (!directory.is_created() || !write_project_descriptor(directory, a_descriptor, a_assertContext) ||
        CreateDirectoryW(directory.child(L"assets").c_str(), nullptr) == FALSE)
    {
        return false;
    }
    auto workspace = cue::create_windows_workspace_filesystem(directory.utf8_path(), a_assertContext);
    if (!workspace)
    {
        return false;
    }
    std::vector<std::string> ids{"cccccccc-cccc-4ccc-8ccc-cccccccccccc"};
    auto service = cue::project_files::ProjectFileService::create(a_descriptor, std::move(*workspace.try_value()),
                                                                  make_id_source(a_assertContext, std::move(ids)),
                                                                  a_assertContext);
    return !service &&
           has_project_file_error(service.try_error(), cue::project_files::ProjectFileError::InvalidRequest);
}

/// @brief Source Root親Componentの大小文字Spelling不一致をService公開前に拒否することを検証する
[[nodiscard]] bool test_source_root_spelling_mismatch(const cue::ProjectDescriptor &a_descriptor,
                                                      const cue::AssertContext &a_assertContext)
{
    TestDirectory directory(true);
    if (!directory.is_created() || !write_project_descriptor(directory, a_descriptor, a_assertContext) ||
        MoveFileExW(directory.child(L"Assets").c_str(), directory.child(L"assets").c_str(), MOVEFILE_WRITE_THROUGH) ==
            FALSE)
    {
        return false;
    }
    auto workspace = cue::create_windows_workspace_filesystem(directory.utf8_path(), a_assertContext);
    if (!workspace)
    {
        return false;
    }
    std::vector<std::string> ids{"dddddddd-dddd-4ddd-8ddd-dddddddddddd"};
    auto service = cue::project_files::ProjectFileService::create(a_descriptor, std::move(*workspace.try_value()),
                                                                  make_id_source(a_assertContext, std::move(ids)),
                                                                  a_assertContext);
    return !service &&
           has_project_file_error(service.try_error(), cue::project_files::ProjectFileError::InvalidRequest);
}
} // namespace

/// @brief ProjectFilesの全Integration Caseを実行する
int main()
{
    TestFatalHandler fatalHandler;
    std::vector<std::unique_ptr<cue::LogSink>> sinks;
    cue::Logger logger(fatalHandler, std::move(sinks));
    cue::AssertContext assertContext(logger, fatalHandler);
    TestDirectory directory;
    auto descriptor = make_descriptor(assertContext);
    if (!directory.is_created() || !descriptor ||
        !write_project_descriptor(directory, *descriptor.try_value(), assertContext))
    {
        return 1;
    }
    if (!test_create_and_policy(directory, *descriptor.try_value(), assertContext))
    {
        return 2;
    }

    auto reserved = cue::RelativePath::parse("CON.txt", assertContext);
    auto escaped = cue::RelativePath::parse("../Escape.bin", assertContext);
    if (reserved || escaped)
    {
        return 3;
    }
    if (!test_concurrent_conflict(directory, *descriptor.try_value(), assertContext))
    {
        return 4;
    }
    if (!test_invalid_operation_id(directory, *descriptor.try_value(), assertContext))
    {
        return 5;
    }
    if (!test_missing_source_root(*descriptor.try_value(), assertContext))
    {
        return 6;
    }
    if (!test_portable_parent_alias(*descriptor.try_value(), assertContext))
    {
        return 7;
    }
    if (!test_descriptor_workspace_mismatch(*descriptor.try_value(), assertContext))
    {
        return 8;
    }
    if (!test_source_root_parent_alias(*descriptor.try_value(), assertContext))
    {
        return 9;
    }
    if (!test_source_root_spelling_mismatch(*descriptor.try_value(), assertContext))
    {
        return 10;
    }
    return test_transfer_operations(*descriptor.try_value(), assertContext) ? 0 : 11;
}
