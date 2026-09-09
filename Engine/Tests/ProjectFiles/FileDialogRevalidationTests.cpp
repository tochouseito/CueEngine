#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Fatal.h>
#include <Cue/Foundation/Log.h>
#include <Cue/IO/Windows/WindowsWorkspaceFilesystem.h>
#include <Cue/Platform/FileDialog.h>
#include <Cue/Project/Descriptor.h>
#include <Cue/ProjectFiles/Error.h>
#include <Cue/ProjectFiles/Service.h>

#include <Windows.h>
#include <winioctl.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
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
        std::_Exit(92);
    }

    /// @brief Message付きFatalをTest終了Codeへ変換する
    [[noreturn]] void terminate(std::string_view) noexcept override
    {
        std::_Exit(93);
    }
};

class EmptyOperationIdSource final : public cue::project_files::ProjectFileOperationIdSource
{
  public:
    /// @brief 診断Contextを非所有で保持する
    explicit EmptyOperationIdSource(const cue::AssertContext &a_assertContext) noexcept
        : m_assertContext(&a_assertContext)
    {
    }
    /// @brief Test SourceのResourceを解放する
    ~EmptyOperationIdSource() override = default;

    /// @brief 本TestでMutationが誤って開始されたことを失敗として返す
    [[nodiscard]] cue::Result<std::string> next_operation_id() noexcept override
    {
        return cue::Result<std::string>::failure(cue::project_files::make_project_file_error(
            *m_assertContext, cue::project_files::ProjectFileError::InvalidRequest,
            "File Dialog revalidation test must not start a mutation"));
    }

  private:
    const cue::AssertContext *m_assertContext;
};

/// @brief Area親Directoryで大文字小文字だけ異なるEntryを個別作成できるようにする
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

class TestDirectory final
{
  public:
    /// @brief Project File Dialog再検証用のProject Rootを作成する
    TestDirectory()
    {
        std::array<wchar_t, MAX_PATH> temporary{};
        const DWORD length = GetTempPathW(static_cast<DWORD>(temporary.size()), temporary.data());
        if (length == 0U || length >= temporary.size())
        {
            return;
        }
        static std::atomic_uint64_t sequence{0U};
        m_path = temporary.data();
        m_path += L"CueProjectFileDialogTests-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                  std::to_wstring(GetTickCount64()) + L"-" +
                  std::to_wstring(sequence.fetch_add(1U, std::memory_order_relaxed));
        m_created = CreateDirectoryW(m_path.c_str(), nullptr) != FALSE &&
                    CreateDirectoryW(child(L"Assets").c_str(), nullptr) != FALSE &&
                    enable_case_sensitive_directory(child(L"Assets")) &&
                    CreateDirectoryW(child(L"Assets\\Source").c_str(), nullptr) != FALSE &&
                    enable_case_sensitive_directory(child(L"Assets\\Source")) &&
                    CreateDirectoryW(child(L"Assets\\Runtime").c_str(), nullptr) != FALSE &&
                    CreateDirectoryW(child(L"Generated").c_str(), nullptr) != FALSE &&
                    CreateDirectoryW(child(L"Saved").c_str(), nullptr) != FALSE;
    }
    /// @brief Test Directoryの複製を禁止する
    TestDirectory(const TestDirectory &) = delete;
    /// @brief Test DirectoryのCopy代入を禁止する
    TestDirectory &operator=(const TestDirectory &) = delete;
    /// @brief JunctionをLinkとして除去してからTest Rootを削除する
    ~TestDirectory()
    {
        RemoveDirectoryW(child(L"Assets\\Source\\ParentLink").c_str());
        RemoveDirectoryW(child(L"Assets\\Source\\FinalLink").c_str());
        std::error_code error;
        std::filesystem::remove_all(m_path, error);
    }

    /// @brief Fixtureが完全に生成されたか返す
    [[nodiscard]] bool is_created() const noexcept
    {
        return m_created;
    }

    /// @brief Root配下のNative Pathを作る
    [[nodiscard]] std::wstring child(std::wstring_view a_relative) const
    {
        return m_path + L"\\" + std::wstring(a_relative);
    }

    /// @brief Workspace Factory用のRoot UTF-8 Pathを返す
    [[nodiscard]] std::string utf8_path() const
    {
        return to_utf8(m_path);
    }

    /// @brief Native Child PathをFile Dialog形式のUTF-8 Absolute Pathへ変換する
    [[nodiscard]] std::string child_utf8(std::wstring_view a_relative) const
    {
        return to_utf8(child(a_relative));
    }

    /// @brief Root外Sibling PathをFile Dialog形式のUTF-8 Absolute Pathへ変換する
    [[nodiscard]] std::string outside_utf8() const
    {
        return to_utf8(m_path + L"-outside.bin");
    }

  private:
    /// @brief UTF-16 Absolute Pathを厳密なUTF-8へ変換する
    [[nodiscard]] static std::string to_utf8(std::wstring_view a_path)
    {
        const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, a_path.data(),
                                              static_cast<int>(a_path.size()), nullptr, 0, nullptr, nullptr);
        if (count <= 0)
        {
            return {};
        }
        std::string result(static_cast<std::size_t>(count), '\0');
        if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, a_path.data(), static_cast<int>(a_path.size()),
                                result.data(), count, nullptr, nullptr) != count)
        {
            return {};
        }
        return result;
    }

    std::wstring m_path;
    bool m_created = false;
};

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

/// @brief 特権不要のDirectory JunctionをReparse Point Fixtureとして作成する
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

/// @brief Test用Native Fileへ指定Byte列を書き込む
[[nodiscard]] bool write_file(std::wstring_view a_path, std::span<const std::byte> a_bytes) noexcept
{
    const std::wstring path(a_path);
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0U, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return false;
    }
    DWORD written = 0U;
    const bool succeeded =
        WriteFile(file, a_bytes.data(), static_cast<DWORD>(a_bytes.size()), &written, nullptr) != FALSE &&
        written == a_bytes.size();
    CloseHandle(file);
    return succeeded;
}

/// @brief File Dialog結果の未検証PathだけをProjectFileServiceへ渡して再検証する
[[nodiscard]] cue::Result<cue::RelativePath> revalidate_selected(
    cue::project_files::ProjectFileService &a_service, std::string a_absolutePath,
    cue::project_files::ProjectFileSelectionPurpose a_purpose,
    cue::project_files::ProjectFileArea a_area = cue::project_files::ProjectFileArea::SourceAssets) noexcept
{
    cue::FileDialogResult selected = cue::FileDialogResult::selected(std::move(a_absolutePath));
    const std::optional<std::string_view> path = selected.selected_path();
    if (!path.has_value())
    {
        std::abort();
    }
    return a_service.revalidate_external_selection(a_area, *path, a_purpose);
}

/// @brief Resultが指定ProjectFiles分類を保持するか判定する
[[nodiscard]] bool has_project_file_error(const cue::Result<cue::RelativePath> &a_result,
                                          cue::project_files::ProjectFileError a_code) noexcept
{
    return !a_result && a_result.try_error() != nullptr &&
           a_result.try_error()->code().domain() == "Cue.ProjectFiles" &&
           a_result.try_error()->code().value() == static_cast<std::int64_t>(a_code);
}

/// @brief Project Descriptorを生成してTest Rootへ保存する
[[nodiscard]] cue::Result<cue::ProjectDescriptor> create_project_fixture(
    const TestDirectory &a_directory, const cue::AssertContext &a_assertContext) noexcept
{
    cue::Result<cue::ProjectId> id = cue::ProjectId::parse("abcdefab-cdef-4abc-8def-abcdefabcdef", a_assertContext);
    if (!id)
    {
        return cue::Result<cue::ProjectDescriptor>::failure(std::move(*id.try_error()));
    }
    cue::Result<cue::ProjectDescriptor> descriptor = cue::create_blank_project_descriptor(
        *id.try_value(), "File Dialog Revalidation",
        cue::EngineCompatibility{cue::EngineVersion{1U, 0U, 0U}, cue::EngineVersion{2U, 0U, 0U}},
        "00000000-0000-4000-8000-000000000099", a_assertContext);
    if (!descriptor)
    {
        return descriptor;
    }
    cue::Result<std::string> serialized = cue::serialize_project_descriptor(*descriptor.try_value(), a_assertContext);
    if (!serialized ||
        !write_file(a_directory.child(L"CueProject.json"),
                    std::as_bytes(std::span(serialized.try_value()->data(), serialized.try_value()->size()))))
    {
        return cue::Result<cue::ProjectDescriptor>::failure(cue::project_files::make_project_file_error(
            a_assertContext, cue::project_files::ProjectFileError::StorageFailure,
            "File Dialog test project descriptor could not be written"));
    }
    return descriptor;
}

/// @brief 16 Segment Source Rootを持つDescriptorとNative Fixtureを生成する
[[nodiscard]] cue::Result<cue::ProjectDescriptor> create_deep_root_fixture(
    const TestDirectory &a_directory, const cue::AssertContext &a_assertContext) noexcept
{
    constexpr std::string_view k_sourceRoot = "D/D/D/D/D/D/D/D/D/D/D/D/D/D/D/D";
    constexpr std::wstring_view k_nativeSourceRoot = L"D\\D\\D\\D\\D\\D\\D\\D\\D\\D\\D\\D\\D\\D\\D\\D";
    std::error_code directoryError;
    std::filesystem::create_directories(a_directory.child(k_nativeSourceRoot), directoryError);
    const std::array<std::byte, 3U> bytes{std::byte{'c'}, std::byte{'u'}, std::byte{'e'}};
    if (directoryError || !write_file(a_directory.child(std::wstring(k_nativeSourceRoot) + L"\\Tail.bin"), bytes))
    {
        return cue::Result<cue::ProjectDescriptor>::failure(cue::project_files::make_project_file_error(
            a_assertContext, cue::project_files::ProjectFileError::StorageFailure,
            "Deep root File Dialog fixture could not be created"));
    }

    std::string json =
        R"json({"schemaVersion":1,"projectId":"abcdefab-cdef-4abc-8def-abcdefabcdef","displayName":"Deep Root","engineCompatibility":{"minimum":"1.0.0","maximumExclusive":"2.0.0"},"roots":{"sourceAssets":")json";
    try
    {
        json.append(k_sourceRoot);
        json.append(
            R"json(","runtimeAssets":"Assets/Runtime","generated":"Generated","saved":"Saved"},"defaultScene":null,"requiredCapabilities":[],"extensions":{}})json");
    }
    catch (...)
    {
        std::abort();
    }
    cue::Result<cue::ProjectDescriptor> descriptor = cue::parse_project_descriptor(json, a_assertContext);
    if (!descriptor)
    {
        return descriptor;
    }
    cue::Result<std::string> serialized = cue::serialize_project_descriptor(*descriptor.try_value(), a_assertContext);
    if (!serialized ||
        !write_file(a_directory.child(L"CueProject.json"),
                    std::as_bytes(std::span(serialized.try_value()->data(), serialized.try_value()->size()))))
    {
        return cue::Result<cue::ProjectDescriptor>::failure(cue::project_files::make_project_file_error(
            a_assertContext, cue::project_files::ProjectFileError::StorageFailure,
            "Deep root project descriptor could not be written"));
    }
    return descriptor;
}

/// @brief 16 Segment Area Root直下のFileをArea相対Locatorとして再検証できるか検証する
[[nodiscard]] bool run_deep_root_revalidation_test(const cue::AssertContext &a_assertContext)
{
    TestDirectory directory;
    cue::Result<cue::ProjectDescriptor> descriptor = create_deep_root_fixture(directory, a_assertContext);
    if (!directory.is_created() || !descriptor)
    {
        return false;
    }
    cue::Result<std::unique_ptr<cue::WorkspaceFilesystem>> workspace =
        cue::create_windows_workspace_filesystem(directory.utf8_path(), a_assertContext);
    if (!workspace)
    {
        return false;
    }
    cue::Result<cue::project_files::ProjectFileService> service = cue::project_files::ProjectFileService::create(
        *descriptor.try_value(), std::move(*workspace.try_value()),
        std::make_unique<EmptyOperationIdSource>(a_assertContext), a_assertContext);
    if (!service)
    {
        return false;
    }
    auto result = revalidate_selected(*service.try_value(),
                                      directory.child_utf8(L"D\\D\\D\\D\\D\\D\\D\\D\\D\\D\\D\\D\\D\\D\\D\\D\\Tail.bin"),
                                      cue::project_files::ProjectFileSelectionPurpose::OpenExistingFile);
    return result && result.try_value()->text() == "Tail.bin";
}

/// @brief File、Folder、Save先、Area、Alias、Reparse、TOCTOU再検証を検証する
[[nodiscard]] bool run_revalidation_tests(const cue::AssertContext &a_assertContext)
{
    TestDirectory directory;
    const std::array<std::byte, 3U> bytes{std::byte{'c'}, std::byte{'u'}, std::byte{'e'}};
    cue::Result<cue::ProjectDescriptor> descriptor = create_project_fixture(directory, a_assertContext);
    if (!directory.is_created() || !descriptor ||
        CreateDirectoryW(directory.child(L"Assets\\Source\\Folder").c_str(), nullptr) == FALSE ||
        CreateDirectoryW(directory.child(L"Assets\\Source\\Target").c_str(), nullptr) == FALSE ||
        !write_file(directory.child(L"Assets\\Source\\Existing.txt"), bytes) ||
        !write_file(directory.child(L"Assets\\Source\\Temporary.txt"), bytes) ||
        !write_file(directory.child(L"Assets\\Source\\Target\\Inside.txt"), bytes) ||
        !write_file(directory.child(L"Assets\\Runtime\\Runtime.bin"), bytes) ||
        !create_directory_junction(directory.child(L"Assets\\Source\\FinalLink"),
                                   directory.child(L"Assets\\Source\\Target")) ||
        !create_directory_junction(directory.child(L"Assets\\Source\\ParentLink"),
                                   directory.child(L"Assets\\Source\\Target")))
    {
        return false;
    }

    cue::Result<std::unique_ptr<cue::WorkspaceFilesystem>> workspace =
        cue::create_windows_workspace_filesystem(directory.utf8_path(), a_assertContext);
    if (!workspace)
    {
        return false;
    }
    cue::Result<cue::project_files::ProjectFileService> service = cue::project_files::ProjectFileService::create(
        *descriptor.try_value(), std::move(*workspace.try_value()),
        std::make_unique<EmptyOperationIdSource>(a_assertContext), a_assertContext);
    if (!service)
    {
        return false;
    }

    using cue::project_files::ProjectFileSelectionPurpose;
    cue::Result<cue::RelativePath> existing =
        revalidate_selected(*service.try_value(), directory.child_utf8(L"Assets\\Source\\Existing.txt"),
                            ProjectFileSelectionPurpose::OpenExistingFile);
    cue::Result<cue::RelativePath> folder =
        revalidate_selected(*service.try_value(), directory.child_utf8(L"Assets\\Source\\Folder"),
                            ProjectFileSelectionPurpose::SelectExistingDirectory);
    cue::Result<cue::RelativePath> saveExisting =
        revalidate_selected(*service.try_value(), directory.child_utf8(L"Assets\\Source\\Existing.txt"),
                            ProjectFileSelectionPurpose::SaveFileDestination);
    cue::Result<cue::RelativePath> saveNew =
        revalidate_selected(*service.try_value(), directory.child_utf8(L"Assets\\Source\\NewScene.cuescene"),
                            ProjectFileSelectionPurpose::SaveFileDestination);
    cue::Result<cue::RelativePath> wrongOpenType =
        revalidate_selected(*service.try_value(), directory.child_utf8(L"Assets\\Source\\Folder"),
                            ProjectFileSelectionPurpose::OpenExistingFile);
    cue::Result<cue::RelativePath> wrongFolderType =
        revalidate_selected(*service.try_value(), directory.child_utf8(L"Assets\\Source\\Existing.txt"),
                            ProjectFileSelectionPurpose::SelectExistingDirectory);
    cue::Result<cue::RelativePath> alias =
        revalidate_selected(*service.try_value(), directory.child_utf8(L"Assets\\Source\\existing.txt"),
                            ProjectFileSelectionPurpose::OpenExistingFile);
    cue::Result<cue::RelativePath> outside = revalidate_selected(*service.try_value(), directory.outside_utf8(),
                                                                 ProjectFileSelectionPurpose::OpenExistingFile);
    cue::Result<cue::RelativePath> otherArea =
        revalidate_selected(*service.try_value(), directory.child_utf8(L"Assets\\Runtime\\Runtime.bin"),
                            ProjectFileSelectionPurpose::OpenExistingFile);
    cue::Result<cue::RelativePath> finalReparse =
        revalidate_selected(*service.try_value(), directory.child_utf8(L"Assets\\Source\\FinalLink"),
                            ProjectFileSelectionPurpose::SelectExistingDirectory);
    cue::Result<cue::RelativePath> parentReparse =
        revalidate_selected(*service.try_value(), directory.child_utf8(L"Assets\\Source\\ParentLink\\Inside.txt"),
                            ProjectFileSelectionPurpose::OpenExistingFile);
    cue::Result<cue::RelativePath> temporary =
        revalidate_selected(*service.try_value(), directory.child_utf8(L"Assets\\Source\\Temporary.txt"),
                            ProjectFileSelectionPurpose::OpenExistingFile);
    const std::string wrongThreadPath = directory.child_utf8(L"Assets\\Source\\Existing.txt");
    bool rejectedWrongThread = false;
    /// @brief Service Owner以外のThreadからの再検証がWorkspaceへ触れる前に拒否されることを検証する
    std::thread worker(
        [&service, &wrongThreadPath, &rejectedWrongThread]() noexcept
        {
            cue::Result<cue::RelativePath> result = service.try_value()->revalidate_external_selection(
                cue::project_files::ProjectFileArea::SourceAssets, wrongThreadPath,
                ProjectFileSelectionPurpose::OpenExistingFile);
            rejectedWrongThread = has_project_file_error(result, cue::project_files::ProjectFileError::InvalidRequest);
        });
    worker.join();
    if (DeleteFileW(directory.child(L"Assets\\Source\\Temporary.txt").c_str()) == FALSE)
    {
        return false;
    }
    cue::Result<cue::RelativePath> deletedBeforeUse =
        revalidate_selected(*service.try_value(), directory.child_utf8(L"Assets\\Source\\Temporary.txt"),
                            ProjectFileSelectionPurpose::OpenExistingFile);
    if (!write_file(directory.child(L"Assets\\Source\\existing.txt"), bytes) ||
        CreateDirectoryW(directory.child(L"Assets\\source").c_str(), nullptr) == FALSE ||
        !write_file(directory.child(L"Assets\\source\\Existing.txt"), bytes))
    {
        return false;
    }
    cue::Result<cue::RelativePath> collision =
        revalidate_selected(*service.try_value(), directory.child_utf8(L"Assets\\Source\\Existing.txt"),
                            ProjectFileSelectionPurpose::OpenExistingFile);
    cue::Result<cue::RelativePath> areaAlias =
        revalidate_selected(*service.try_value(), directory.child_utf8(L"Assets\\source\\Existing.txt"),
                            ProjectFileSelectionPurpose::OpenExistingFile);

    if (!existing || existing.try_value()->text() != "Existing.txt")
    {
        return false;
    }
    if (!folder || folder.try_value()->text() != "Folder")
    {
        return false;
    }
    if (!saveExisting || saveExisting.try_value()->text() != "Existing.txt")
    {
        return false;
    }
    if (!saveNew || saveNew.try_value()->text() != "NewScene.cuescene")
    {
        return false;
    }
    if (wrongOpenType || wrongFolderType || alias || collision || areaAlias || outside || otherArea || finalReparse ||
        parentReparse)
    {
        return false;
    }
    if (!temporary || !rejectedWrongThread || deletedBeforeUse)
    {
        return false;
    }
    return true;
}
} // namespace

/// @brief Native File Dialog結果からProjectFileServiceまでの再検証Contractを実行する
int main()
{
    TestFatalHandler fatalHandler;
    std::vector<std::unique_ptr<cue::LogSink>> sinks;
    cue::Logger logger(fatalHandler, std::move(sinks));
    cue::AssertContext assertContext(logger, fatalHandler);
    if (!run_revalidation_tests(assertContext))
    {
        return 1;
    }
    return run_deep_root_revalidation_test(assertContext) ? 0 : 2;
}
