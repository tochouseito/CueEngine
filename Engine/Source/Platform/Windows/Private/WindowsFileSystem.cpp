#include <Platform/Windows/WindowsFileSystem.h>

#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <string>
#include <utility>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <shlobj.h>
#include <windows.h>

#include <Foundation/Windows/UtfConversion.h>
#include <Platform/Diagnostics.h>

namespace cue
{
namespace
{
/// @brief Native 診断を Portable Error の所有値へ移す
Error file_error(std::string a_operation, DWORD a_code = GetLastError())
{
    return {ErrorCategory::PlatformFailure, std::move(a_operation), a_code};
}

/// @brief Windows の Device 名、ADS、末尾の曖昧な要素を通常 File Path として受け入れない
bool is_valid_component(std::wstring_view a_name)
{
    if (a_name.empty() || a_name == L"." || a_name == L"..")
    {
        return a_name == L"." || a_name == L"..";
    }
    if (a_name.back() == L'.' || a_name.back() == L' ')
    {
        return false;
    }
    for (auto character : a_name)
    {
        if (character < 32 || std::wstring_view(L"<>:\"|?*").find(character) != std::wstring_view::npos)
        {
            return false;
        }
    }
    auto base = std::wstring(a_name.substr(0, a_name.find('.')));
    for (auto &character : base)
    {
        if (character >= L'a' && character <= L'z')
        {
            character -= L'a' - L'A';
        }
    }
    if (base == L"CON" || base == L"PRN" || base == L"AUX" || base == L"NUL" || base == L"CONIN$" || base == L"CONOUT$")
    {
        return false;
    }
    return !(base.size() == 4 && (base.starts_with(L"COM") || base.starts_with(L"LPT")) &&
             ((base[3] >= L'1' && base[3] <= L'9') || base[3] == L'¹' || base[3] == L'²' || base[3] == L'³'));
}

/// @brief 通常 Path を検証し、長い File 名に対応する絶対 Native Path を返す
Result<std::wstring> native_path(const Path &a_path)
{
    if (a_path.is_empty())
    {
        return Result<std::wstring>::failure({ErrorCategory::InvalidArgument, "WindowsFileSystem.path.empty"});
    }
    auto converted = utf8_to_utf16(a_path.utf8());
    if (!converted.has_value())
    {
        return Result<std::wstring>::failure(*converted.try_error());
    }
    auto text = converted.take_value();
    std::replace(text.begin(), text.end(), L'/', L'\\');
    const auto begin = text.size() >= 3 && text[1] == L':' ? 3u : text.starts_with(L"\\\\") ? 2u : 0u;
    for (std::size_t offset = begin; offset < text.size();)
    {
        const auto found = text.find(L'\\', offset);
        const auto end = found == std::wstring::npos ? text.size() : found;
        if (end != offset && !is_valid_component(std::wstring_view(text).substr(offset, end - offset)))
        {
            return Result<std::wstring>::failure({ErrorCategory::InvalidArgument, "WindowsFileSystem.path.component"});
        }
        offset = end + 1;
    }
    std::wstring full(32768, L'\0');
    const auto length = GetFullPathNameW(text.c_str(), static_cast<DWORD>(full.size()), full.data(), nullptr);
    if (!length || length >= full.size() - 8)
    {
        return Result<std::wstring>::failure(
            file_error("GetFullPathNameW", length ? ERROR_FILENAME_EXCED_RANGE : GetLastError()));
    }
    full.resize(length);
    if (full.starts_with(L"\\\\"))
    {
        full = L"\\\\?\\UNC\\" + full.substr(2);
    }
    else
    {
        full = L"\\\\?\\" + full;
    }
    return Result<std::wstring>::success(std::move(full));
}

/// @brief Path 未存在だけを空の問い合わせ成功へ変換する
bool is_missing(DWORD a_error) noexcept
{
    return a_error == ERROR_FILE_NOT_FOUND || a_error == ERROR_PATH_NOT_FOUND;
}

/// @brief Native File Handle を一意所有し、同期 Cursor を持つ
class WindowsFile final : public IFile
{
  public:
    /// @brief Factory の生成済み Handle を所有する
    explicit WindowsFile(HANDLE a_handle) noexcept : m_handle(a_handle)
    {
    }
    /// @brief 未所有の Native Handle を Factory から一度だけ受け取る
    void adopt(HANDLE a_handle) noexcept
    {
        m_handle = a_handle;
    }
    /// @brief 明示 Close されなかった Handle の最後の回収を行う
    ~WindowsFile() override
    {
        if (m_handle != INVALID_HANDLE_VALUE && !CloseHandle(m_handle))
        {
            report_message("WindowsFile", "CloseHandle failed", DiagnosticSeverity::Error);
        }
    }
    /// @brief DWORD 範囲の部分 Read を行い、EOF は 0 を返す
    Result<std::size_t> read(std::span<std::byte> a_destination) override
    {
        if (m_handle == INVALID_HANDLE_VALUE)
        {
            return Result<std::size_t>::failure({ErrorCategory::InvalidState, "WindowsFile.read.closed"});
        }
        DWORD count = 0;
        const auto size = static_cast<DWORD>(std::min(a_destination.size(), std::size_t(MAXDWORD)));
        if (!ReadFile(m_handle, a_destination.data(), size, &count, nullptr))
        {
            return Result<std::size_t>::failure(file_error("ReadFile"));
        }
        return Result<std::size_t>::success(count);
    }
    /// @brief DWORD 範囲の部分 Write を行う
    Result<std::size_t> write(std::span<const std::byte> a_source) override
    {
        if (m_handle == INVALID_HANDLE_VALUE)
        {
            return Result<std::size_t>::failure({ErrorCategory::InvalidState, "WindowsFile.write.closed"});
        }
        DWORD count = 0;
        const auto size = static_cast<DWORD>(std::min(a_source.size(), std::size_t(MAXDWORD)));
        if (!WriteFile(m_handle, a_source.data(), size, &count, nullptr))
        {
            return Result<std::size_t>::failure(file_error("WriteFile"));
        }
        return Result<std::size_t>::success(count);
    }
    /// @brief Cursor 移動に失敗した場合は Native Error を返す
    Result<std::uint64_t> seek(std::int64_t a_offset, SeekOrigin a_origin) override
    {
        if (m_handle == INVALID_HANDLE_VALUE)
        {
            return Result<std::uint64_t>::failure({ErrorCategory::InvalidState, "WindowsFile.seek.closed"});
        }
        DWORD origin = 0;
        switch (a_origin)
        {
        case SeekOrigin::Begin:
            origin = FILE_BEGIN;
            break;
        case SeekOrigin::Current:
            origin = FILE_CURRENT;
            break;
        case SeekOrigin::End:
            origin = FILE_END;
            break;
        default:
            return Result<std::uint64_t>::failure({ErrorCategory::InvalidArgument, "WindowsFile.seek.origin"});
        }
        LARGE_INTEGER offset;
        offset.QuadPart = a_offset;
        LARGE_INTEGER position;
        if (!SetFilePointerEx(m_handle, offset, &position, origin))
        {
            return Result<std::uint64_t>::failure(file_error("SetFilePointerEx"));
        }
        return Result<std::uint64_t>::success(static_cast<std::uint64_t>(position.QuadPart));
    }
    /// @brief Cursor を移動せず位置を返す
    Result<std::uint64_t> tell() override
    {
        return seek(0, SeekOrigin::Current);
    }
    /// @brief Handle が参照する現在の Size を返す
    Result<std::uint64_t> size() override
    {
        if (m_handle == INVALID_HANDLE_VALUE)
        {
            return Result<std::uint64_t>::failure({ErrorCategory::InvalidState, "WindowsFile.size.closed"});
        }
        LARGE_INTEGER size;
        if (!GetFileSizeEx(m_handle, &size))
        {
            return Result<std::uint64_t>::failure(file_error("GetFileSizeEx"));
        }
        return Result<std::uint64_t>::success(static_cast<std::uint64_t>(size.QuadPart));
    }
    /// @brief OS Buffer の Flush 完了を確認する
    Result<void> flush() override
    {
        if (m_handle == INVALID_HANDLE_VALUE)
        {
            return Result<void>::failure({ErrorCategory::InvalidState, "WindowsFile.flush.closed"});
        }
        return FlushFileBuffers(m_handle) ? Result<void>::success()
                                          : Result<void>::failure(file_error("FlushFileBuffers"));
    }
    /// @brief 成功後は Handle を失効させ、重複 Close は成功する
    Result<void> close() override
    {
        if (m_handle == INVALID_HANDLE_VALUE)
        {
            return Result<void>::success();
        }
        if (!CloseHandle(m_handle))
        {
            return Result<void>::failure(file_error("CloseHandle"));
        }
        m_handle = INVALID_HANDLE_VALUE;
        return Result<void>::success();
    }

  private:
    HANDLE m_handle;
};

/// @brief 同期 FileSystem を無状態で実装する
class WindowsFileSystem final : public IFileSystem
{
  public:
    /// @brief 配置 Path を UTF-8 に変換し、親を返す
    Result<Path> executable_directory() override
    {
        std::wstring name(32768, L'\0');
        const auto length = GetModuleFileNameW(nullptr, name.data(), static_cast<DWORD>(name.size()));
        if (!length || length >= name.size())
        {
            return Result<Path>::failure(file_error("GetModuleFileNameW"));
        }
        auto text = utf16_to_utf8(std::wstring_view(name).substr(0, length));
        if (!text.has_value())
        {
            return Result<Path>::failure(*text.try_error());
        }
        auto path = Path::create(*text.try_value());
        return path.has_value() ? Result<Path>::success(path.try_value()->parent()) : std::move(path);
    }
    /// @brief OS の Known Folder を利用し、設定された LocalAppData を返す
    Result<Path> local_data_directory() override
    {
        PWSTR directory = nullptr;
        const auto hr = SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &directory);
        // 変換の Allocation 失敗でも COM Task Memory を回収する
        const auto release = [](wchar_t *a_memory) { CoTaskMemFree(a_memory); };
        std::unique_ptr<wchar_t, decltype(release)> owned(directory, release);
        if (FAILED(hr))
        {
            return Result<Path>::failure({ErrorCategory::PlatformFailure, "SHGetKnownFolderPath", hr});
        }
        auto text = utf16_to_utf8(directory);
        return text.has_value() ? Path::create(*text.try_value()) : Result<Path>::failure(*text.try_error());
    }
    /// @brief Reparse Point は Link として報告し、問い合わせ失敗を存在なしにしない
    Result<std::optional<FileInfo>> stat(const Path &a_path) override
    {
        auto path = native_path(a_path);
        if (!path.has_value())
        {
            return Result<std::optional<FileInfo>>::failure(*path.try_error());
        }
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (!GetFileAttributesExW(path.try_value()->c_str(), GetFileExInfoStandard, &data))
        {
            const auto error = GetLastError();
            return is_missing(error)
                       ? Result<std::optional<FileInfo>>::success(std::nullopt)
                       : Result<std::optional<FileInfo>>::failure(file_error("GetFileAttributesExW", error));
        }
        FileInfo info;
        info.type = data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT ? FileType::Link
                    : data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY   ? FileType::Directory
                                                                         : FileType::Regular;
        info.size = (std::uint64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
        const auto ticks =
            (std::uint64_t(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
        constexpr std::uint64_t k_epoch = 116444736000000000;
        const auto delta = ticks >= k_epoch ? ticks - k_epoch : k_epoch - ticks;
        const auto bounded = std::min(delta, std::uint64_t((std::numeric_limits<std::int64_t>::max)() / 100));
        info.modifiedNanoseconds = static_cast<std::int64_t>(bounded * 100) * (ticks >= k_epoch ? 1 : -1);
        return Result<std::optional<FileInfo>>::success(info);
    }
    /// @brief Root から各親を作成し、File と衝突した場合は失敗する
    Result<void> create_directories(const Path &a_path) override
    {
        auto path = native_path(a_path);
        if (!path.has_value())
        {
            return Result<void>::failure(*path.try_error());
        }
        auto text = path.take_value();
        std::size_t rootEnd = 7;
        if (text.starts_with(L"\\\\?\\UNC\\"))
        {
            rootEnd = text.find(L'\\', text.find(L'\\', 8) + 1);
            if (rootEnd == std::wstring::npos)
            {
                rootEnd = text.size();
            }
        }
        for (std::size_t offset = rootEnd; offset <= text.size(); ++offset)
        {
            if (offset != text.size() && text[offset] != L'\\')
            {
                continue;
            }
            const auto prefix = text.substr(0, offset);
            if (CreateDirectoryW(prefix.c_str(), nullptr))
            {
                continue;
            }
            const auto error = GetLastError();
            const auto attributes = GetFileAttributesW(prefix.c_str());
            if (error != ERROR_ALREADY_EXISTS || attributes == INVALID_FILE_ATTRIBUTES ||
                !(attributes & FILE_ATTRIBUTE_DIRECTORY))
            {
                return Result<void>::failure(file_error("CreateDirectoryW", error));
            }
        }
        return Result<void>::success();
    }
    /// @brief 列挙 Handle を必ず閉じ、UTF-8 の直下 Path を所有値で返す
    Result<std::vector<Path>> list_directory(const Path &a_path) override
    {
        using listResult = Result<std::vector<Path>>;
        auto path = native_path(a_path);
        if (!path.has_value())
        {
            return listResult::failure(*path.try_error());
        }
        auto info = stat(a_path);
        if (!info.has_value() || !*info.try_value() || (**info.try_value()).type != FileType::Directory)
        {
            return listResult::failure(
                info.has_value() ? Error{ErrorCategory::InvalidArgument, "WindowsFileSystem.list_directory.type"}
                                 : *info.try_error());
        }
        auto pattern = path.take_value();
        if (pattern.back() != L'\\')
        {
            pattern += L'\\';
        }
        pattern += L'*';
        WIN32_FIND_DATAW data{};
        const auto handle = FindFirstFileW(pattern.c_str(), &data);
        if (handle == INVALID_HANDLE_VALUE)
        {
            const auto error = GetLastError();
            return error == ERROR_FILE_NOT_FOUND ? listResult::success({})
                                                 : listResult::failure(file_error("FindFirstFileW", error));
        }
        struct FindOwner final
        {
            HANDLE handle;
            /// @brief 途中 return / Exception でも検索 Handle を解放する
            ~FindOwner()
            {
                FindClose(handle);
            }
        } owned{handle};
        std::vector<Path> paths;
        do
        {
            const std::wstring_view name(data.cFileName);
            if (name == L"." || name == L"..")
            {
                continue;
            }
            auto text = utf16_to_utf8(name);
            if (!text.has_value())
            {
                return listResult::failure(*text.try_error());
            }
            auto child = a_path.join(*text.try_value());
            if (!child.has_value())
            {
                return listResult::failure(*child.try_error());
            }
            paths.push_back(child.take_value());
        } while (FindNextFileW(handle, &data));
        const auto error = GetLastError();
        return error == ERROR_NO_MORE_FILES ? listResult::success(std::move(paths))
                                            : listResult::failure(file_error("FindNextFileW", error));
    }
    /// @brief Directory Link はリンク自体を削除し、子には触れない
    Result<bool> remove(const Path &a_path) override
    {
        auto path = native_path(a_path);
        if (!path.has_value())
        {
            return Result<bool>::failure(*path.try_error());
        }
        const auto &text = *path.try_value();
        const auto attributes = GetFileAttributesW(text.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES)
        {
            const auto error = GetLastError();
            return is_missing(error) ? Result<bool>::success(false)
                                     : Result<bool>::failure(file_error("GetFileAttributesW", error));
        }
        const auto removed =
            attributes & FILE_ATTRIBUTE_DIRECTORY ? RemoveDirectoryW(text.c_str()) : DeleteFileW(text.c_str());
        if (!removed)
        {
            const auto error = GetLastError();
            return is_missing(error) ? Result<bool>::success(false)
                                     : Result<bool>::failure(file_error("WindowsFileSystem.remove", error));
        }
        return Result<bool>::success(true);
    }
    /// @brief 上書きと Cross Volume Copy を許可せず移動する
    Result<void> rename(const Path &a_from, const Path &a_to) override
    {
        auto from = native_path(a_from);
        auto to = native_path(a_to);
        if (!from.has_value() || !to.has_value())
        {
            return Result<void>::failure(!from.has_value() ? *from.try_error() : *to.try_error());
        }
        return MoveFileExW(from.try_value()->c_str(), to.try_value()->c_str(), 0)
                   ? Result<void>::success()
                   : Result<void>::failure(file_error("MoveFileExW.rename"));
    }
    /// @brief Native Copy の上書き可否を呼出側の設定へ合わせる
    Result<void> copy_file(const Path &a_from, const Path &a_to, bool a_overwrite) override
    {
        auto from = native_path(a_from);
        auto to = native_path(a_to);
        if (!from.has_value() || !to.has_value())
        {
            return Result<void>::failure(!from.has_value() ? *from.try_error() : *to.try_error());
        }
        return CopyFileW(from.try_value()->c_str(), to.try_value()->c_str(), !a_overwrite)
                   ? Result<void>::success()
                   : Result<void>::failure(file_error("CopyFileW"));
    }
    /// @brief 設定を検証し、Disk File の Native Handle だけを公開する
    Result<std::unique_ptr<IFile>> open(const Path &a_path, const FileOpenDesc &a_desc) override
    {
        using openResult = Result<std::unique_ptr<IFile>>;
        auto path = native_path(a_path);
        if (!path.has_value())
        {
            return openResult::failure(*path.try_error());
        }
        DWORD access = 0;
        switch (a_desc.access)
        {
        case FileAccess::Read:
            access = GENERIC_READ;
            break;
        case FileAccess::Write:
            access = GENERIC_WRITE;
            break;
        case FileAccess::ReadWrite:
            access = GENERIC_READ | GENERIC_WRITE;
            break;
        default:
            return openResult::failure({ErrorCategory::InvalidArgument, "WindowsFileSystem.open.access"});
        }
        DWORD creation = 0;
        switch (a_desc.creation)
        {
        case FileCreation::OpenExisting:
            creation = OPEN_EXISTING;
            break;
        case FileCreation::OpenAlways:
            creation = OPEN_ALWAYS;
            break;
        case FileCreation::CreateNew:
            creation = CREATE_NEW;
            break;
        case FileCreation::CreateAlways:
            creation = CREATE_ALWAYS;
            break;
        case FileCreation::TruncateExisting:
            creation = TRUNCATE_EXISTING;
            break;
        default:
            return openResult::failure({ErrorCategory::InvalidArgument, "WindowsFileSystem.open.creation"});
        }
        if (a_desc.access == FileAccess::Read && a_desc.creation != FileCreation::OpenExisting)
        {
            return openResult::failure({ErrorCategory::InvalidArgument, "WindowsFileSystem.open.read_creation"});
        }
        const DWORD share = (a_desc.isReadShared ? FILE_SHARE_READ : 0) |
                            (a_desc.isWriteShared ? FILE_SHARE_WRITE : 0) |
                            (a_desc.isDeleteShared ? FILE_SHARE_DELETE : 0);
        // Allocation を先に行い、Handle 取得後の Exception でリークさせない
        auto file = std::make_unique<WindowsFile>(INVALID_HANDLE_VALUE);
        const auto handle =
            CreateFileW(path.try_value()->c_str(), access, share, nullptr, creation, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
        {
            return openResult::failure(file_error("CreateFileW"));
        }
        // Disk 以外の Device Handle を通常 File として公開しない
        file->adopt(handle);
        if (GetFileType(handle) != FILE_TYPE_DISK)
        {
            return openResult::failure({ErrorCategory::InvalidArgument, "WindowsFileSystem.open.type"});
        }
        return openResult::success(std::move(file));
    }
    /// @brief 同じ親の一時 File を Flush / Close 後に一回の Native Rename で公開する
    Result<FileSaveOutcome> replace_file(const Path &a_path, std::span<const std::byte> a_data) override
    {
        using saveResult = Result<FileSaveOutcome>;
        auto target = native_path(a_path);
        if (!target.has_value() || a_path.filename().empty())
        {
            return saveResult::failure(
                target.has_value() ? Error{ErrorCategory::InvalidArgument, "WindowsFileSystem.replace_file.path"}
                                   : *target.try_error());
        }
        GUID guid{};
        const auto hr = CoCreateGuid(&guid);
        if (FAILED(hr))
        {
            return saveResult::failure({ErrorCategory::PlatformFailure, "CoCreateGuid", hr});
        }
        std::array<wchar_t, 40> identifier{};
        if (!StringFromGUID2(guid, identifier.data(), static_cast<int>(identifier.size())))
        {
            return saveResult::failure({ErrorCategory::PlatformFailure, "StringFromGUID2"});
        }
        auto text = utf16_to_utf8(identifier.data());
        if (!text.has_value())
        {
            return saveResult::failure(*text.try_error());
        }
        auto temporary = a_path.parent().join(".cue-" + text.take_value() + ".tmp");
        if (!temporary.has_value())
        {
            return saveResult::failure(*temporary.try_error());
        }
        auto nativeTemporary = native_path(*temporary.try_value());
        if (!nativeTemporary.has_value())
        {
            return saveResult::failure(*nativeTemporary.try_error());
        }
        auto opened = open(*temporary.try_value(), {FileAccess::Write, FileCreation::CreateNew, false});
        if (!opened.has_value())
        {
            return saveResult::failure(*opened.try_error());
        }
        auto file = opened.take_value();
        struct TemporaryOwner final
        {
            const std::wstring &path;
            bool isPublished = false;
            /// @brief Exception 経路でも一時 File を削除し、回収失敗は緊急診断する
            ~TemporaryOwner()
            {
                if (!isPublished && !DeleteFileW(path.c_str()))
                {
                    report_message("FileSystem.replace_file", "temporary cleanup failed", DiagnosticSeverity::Error);
                }
            }
        } cleanup{*nativeTemporary.try_value()};
        // File Owner を Cleanup より後に置き、Exception 時も Close してから削除する
        auto writing = std::move(file);
        const auto fail = [&](Error a_error)
        {
            auto closed = writing->close();
            if (!closed.has_value())
            {
                a_error.operation += "; close failed: " + closed.try_error()->operation;
            }
            writing.reset();
            if (!DeleteFileW(nativeTemporary.try_value()->c_str()))
            {
                a_error.operation += "; temporary cleanup failed: " + std::to_string(GetLastError());
            }
            cleanup.isPublished = true;
            return saveResult::failure(std::move(a_error));
        };
        std::size_t offset = 0;
        while (offset < a_data.size())
        {
            auto written = writing->write(a_data.subspan(offset));
            if (!written.has_value())
            {
                return fail(*written.try_error());
            }
            if (*written.try_value() == 0)
            {
                return fail({ErrorCategory::PlatformFailure, "WindowsFileSystem.replace_file.progress"});
            }
            offset += *written.try_value();
        }
        auto flushed = writing->flush();
        if (!flushed.has_value())
        {
            return fail(*flushed.try_error());
        }
        auto closed = writing->close();
        if (!closed.has_value())
        {
            return fail(*closed.try_error());
        }
        if (!MoveFileExW(nativeTemporary.try_value()->c_str(), target.try_value()->c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            return fail(file_error("MoveFileExW.publish"));
        }
        cleanup.isPublished = true;
        // Directory Entry の電源断耐性はこの API では保証しない。公開成功を曖昧な通常 Error にしない
        return saveResult::success({false});
    }
};
} // namespace

Result<std::unique_ptr<IFileSystem>> create_windows_file_system()
{
    try
    {
        return Result<std::unique_ptr<IFileSystem>>::success(std::make_unique<WindowsFileSystem>());
    }
    catch (const std::bad_alloc &)
    {
        return Result<std::unique_ptr<IFileSystem>>::failure(
            {ErrorCategory::PlatformFailure, "WindowsFileSystem.create.allocation"});
    }
}
} // namespace cue
