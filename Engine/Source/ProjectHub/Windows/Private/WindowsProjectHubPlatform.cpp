#include <Cue/ProjectHub/Windows/WindowsProjectHubPlatform.h>

#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Windows/UtfConversion.h>
#include <Cue/IO/Error.h>
#include <Cue/IO/RelativePath.h>
#include <Cue/IO/Windows/WindowsFilesystem.h>
#include <Cue/ProjectHub/Error.h>

#include <Windows.h>
#include <bcrypt.h>
#include <shellapi.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cwctype>
#include <exception>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
constexpr std::size_t k_maxWindowsPathLength = 32767;
constexpr std::size_t k_maxProjectStorageEntryCount = 200000;
constexpr std::size_t k_maxProjectStorageDepth = 128;
constexpr std::uint64_t k_windowsToUnixEpochTicks = 116444736000000000ULL;

/// @brief Allocation失敗をProject Hub診断境界からFatal終端する
[[noreturn]] void terminate_allocation(const cue::AssertContext &a_context) noexcept
{
    a_context.fatal_handler().terminate("Windows Project Hub allocation failed");
    std::terminate();
}

/// @brief UTF-8 LocatorをStrict UTF-16へ変換する
[[nodiscard]] cue::Result<std::wstring> to_utf16(std::string_view a_text, const cue::AssertContext &a_context) noexcept
{
    std::wstring converted;
    const cue::WindowsUtfConversionResult result =
        cue::convert_utf8_to_windows_utf16(a_text, converted, a_context.fatal_handler());
    if (result.status != cue::WindowsUtfConversionStatus::Success || converted.find(L'\0') != std::wstring::npos)
    {
        return cue::Result<std::wstring>::failure(cue::project_hub::make_project_hub_error(
            a_context, cue::project_hub::ProjectHubError::InvalidLocator, "Project locator is not valid UTF-8"));
    }
    return cue::Result<std::wstring>::success(std::move(converted));
}

/// @brief UTF-16 LocatorをStrict UTF-8へ変換する
[[nodiscard]] cue::Result<std::string> to_utf8(std::wstring_view a_text, const cue::AssertContext &a_context) noexcept
{
    std::string converted;
    const cue::WindowsUtfConversionResult result =
        cue::convert_windows_utf16_to_utf8(a_text, converted, a_context.fatal_handler());
    if (result.status != cue::WindowsUtfConversionStatus::Success)
    {
        return cue::Result<std::string>::failure(cue::make_io_error(
            a_context, cue::IoError::IoFailure, "Project locator UTF-16 conversion failed", result.nativeCode));
    }
    return cue::Result<std::string>::success(std::move(converted));
}

/// @brief Absolute Windows Pathを長いPathにも対応するExtended-length表現へ変換する
[[nodiscard]] cue::Result<std::wstring> make_extended_path(std::wstring a_path,
                                                           const cue::AssertContext &a_context) noexcept
{
    try
    {
        if (!a_path.starts_with(L"\\\\?\\"))
        {
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
    }
    catch (...)
    {
        terminate_allocation(a_context);
    }
    if (a_path.size() >= k_maxWindowsPathLength)
    {
        return cue::Result<std::wstring>::failure(
            cue::make_io_error(a_context, cue::IoError::CapacityExceeded, "Project locator exceeds Windows limit"));
    }
    return cue::Result<std::wstring>::success(std::move(a_path));
}

/// @brief Win32 Path失敗をProject Hubが扱えるIO分類へ変換する
[[nodiscard]] cue::Error make_path_error(const cue::AssertContext &a_context, DWORD a_nativeCode,
                                         std::string_view a_summary) noexcept
{
    cue::IoError code = cue::IoError::IoFailure;
    if (a_nativeCode == ERROR_ACCESS_DENIED || a_nativeCode == ERROR_SHARING_VIOLATION)
    {
        code = cue::IoError::PermissionDenied;
    }
    else if (a_nativeCode == ERROR_FILE_NOT_FOUND || a_nativeCode == ERROR_PATH_NOT_FOUND)
    {
        code = cue::IoError::NotFound;
    }
    else if (a_nativeCode == ERROR_FILENAME_EXCED_RANGE)
    {
        code = cue::IoError::CapacityExceeded;
    }
    return cue::make_io_error(a_context, code, a_summary, static_cast<std::int64_t>(a_nativeCode));
}

/// @brief Win32 Find HandleをScope終了時に閉じる
class FindHandle final
{
  public:
    explicit FindHandle(HANDLE a_handle) noexcept : m_handle(a_handle)
    {
    }
    FindHandle(const FindHandle &) = delete;
    FindHandle &operator=(const FindHandle &) = delete;
    ~FindHandle()
    {
        if (m_handle != INVALID_HANDLE_VALUE)
        {
            FindClose(m_handle);
        }
    }

    [[nodiscard]] HANDLE get() const noexcept
    {
        return m_handle;
    }

  private:
    HANDLE m_handle;
};

/// @brief FILETIMEをUTC Unix Millisecondsへ変換する
[[nodiscard]] std::uint64_t unix_milliseconds(const FILETIME &a_time) noexcept
{
    ULARGE_INTEGER ticks{};
    ticks.LowPart = a_time.dwLowDateTime;
    ticks.HighPart = a_time.dwHighDateTime;
    return ticks.QuadPart <= k_windowsToUnixEpochTicks ? 0U : (ticks.QuadPart - k_windowsToUnixEpochTicks) / 10000U;
}

/// @brief DirectoryへChild名を結合しWindows上限を検証する
[[nodiscard]] cue::Result<std::wstring> append_child(std::wstring_view a_directory, std::wstring_view a_name,
                                                     const cue::AssertContext &a_context) noexcept
{
    try
    {
        std::wstring child(a_directory);
        if (!child.empty() && child.back() != L'\\')
        {
            child.push_back(L'\\');
        }
        child.append(a_name);
        if (child.size() >= k_maxWindowsPathLength)
        {
            return cue::Result<std::wstring>::failure(
                cue::make_io_error(a_context, cue::IoError::CapacityExceeded, "Project metadata path is too long"));
        }
        return cue::Result<std::wstring>::success(std::move(child));
    }
    catch (...)
    {
        terminate_allocation(a_context);
    }
}

/// @brief UUID Byte列をRFC 4122 Version 4文字列へ変換する
[[nodiscard]] std::string format_project_id(const std::array<std::uint8_t, 16> &a_bytes,
                                            const cue::AssertContext &a_context) noexcept
{
    constexpr char k_hex[] = "0123456789abcdef";
    constexpr std::size_t k_dashPositions[] = {8, 13, 18, 23};
    std::string text;
    try
    {
        text.reserve(36);
        std::size_t dashIndex = 0;
        for (std::uint8_t byte : a_bytes)
        {
            if (dashIndex < std::size(k_dashPositions) && text.size() == k_dashPositions[dashIndex])
            {
                text.push_back('-');
                ++dashIndex;
            }
            text.push_back(k_hex[byte >> 4]);
            text.push_back(k_hex[byte & 0x0f]);
        }
    }
    catch (...)
    {
        terminate_allocation(a_context);
    }
    return text;
}

/// @brief Cryptographic Random Sourceからcanonical UUID Version 4文字列を生成する
[[nodiscard]] cue::Result<std::string> generate_identity_text(const cue::AssertContext &a_assertContext) noexcept
{
    std::array<std::uint8_t, 16> bytes{};
    const NTSTATUS status =
        BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (status < 0)
    {
        return cue::Result<std::string>::failure(cue::make_io_error(a_assertContext, cue::IoError::IoFailure,
                                                                    "Identity random generation failed",
                                                                    static_cast<std::int64_t>(status)));
    }
    bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0fU) | 0x40U);
    bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3fU) | 0x80U);
    return cue::Result<std::string>::success(format_project_id(bytes, a_assertContext));
}

/// @brief Windows APIをProject HubのPlatform非依存Service境界へ接続する
class WindowsProjectHubPlatform final : public cue::project_hub::ProjectHubPlatform
{
  public:
    /// @brief 診断Contextを非所有で保持するWindows Adapterを生成する
    explicit WindowsProjectHubPlatform(const cue::AssertContext &a_assertContext) noexcept
        : m_assertContext(&a_assertContext)
    {
    }

    /// @brief UTF-8 LocatorをProcess間受け渡し可能な絶対Windows Pathへ正規化する
    [[nodiscard]] cue::Result<std::string> normalize_project_locator(std::string_view a_locator) noexcept override
    {
        cue::Result<std::wstring> converted = to_utf16(a_locator, *m_assertContext);
        if (!converted || converted.try_value()->empty())
        {
            return converted ? cue::Result<std::string>::failure(cue::project_hub::make_project_hub_error(
                                   *m_assertContext, cue::project_hub::ProjectHubError::InvalidLocator,
                                   "Project locator is empty"))
                             : cue::Result<std::string>::failure(std::move(*converted.try_error()));
        }

        const DWORD required = GetFullPathNameW(converted.try_value()->c_str(), 0, nullptr, nullptr);
        if (required == 0 || required >= k_maxWindowsPathLength)
        {
            const DWORD code = required == 0 ? GetLastError() : ERROR_FILENAME_EXCED_RANGE;
            return cue::Result<std::string>::failure(
                make_path_error(*m_assertContext, code, "Project locator normalization failed"));
        }
        std::wstring absolute;
        try
        {
            absolute.resize(required);
        }
        catch (...)
        {
            terminate_allocation(*m_assertContext);
        }
        const DWORD written = GetFullPathNameW(converted.try_value()->c_str(), required, absolute.data(), nullptr);
        if (written == 0 || written >= required)
        {
            return cue::Result<std::string>::failure(
                make_path_error(*m_assertContext, GetLastError(), "Project locator normalization failed"));
        }
        absolute.resize(written);
        while (absolute.size() > 3 && (absolute.back() == L'\\' || absolute.back() == L'/'))
        {
            absolute.pop_back();
        }
        return to_utf8(absolute, *m_assertContext);
    }

    /// @brief 親Locatorと検証済みProject名からProject Root Locatorを構成する
    [[nodiscard]] cue::Result<std::string> compose_project_locator(std::string_view a_parentLocator,
                                                                   std::string_view a_projectName) noexcept override
    {
        cue::Result<cue::RelativePath> name = cue::RelativePath::parse(a_projectName, *m_assertContext);
        if (!name || name.try_value()->text().find('/') != std::string_view::npos)
        {
            return cue::Result<std::string>::failure(cue::project_hub::make_project_hub_error(
                *m_assertContext, cue::project_hub::ProjectHubError::InvalidLocator,
                "Project name is not a valid path segment"));
        }
        cue::Result<std::string> parent = normalize_project_locator(a_parentLocator);
        if (!parent)
        {
            return parent;
        }
        std::string composed;
        try
        {
            composed = *parent.try_value();
            composed.push_back('\\');
            composed.append(a_projectName);
        }
        catch (...)
        {
            terminate_allocation(*m_assertContext);
        }
        return normalize_project_locator(composed);
    }

    /// @brief Project Root LocatorへCueProject.jsonを結合する
    [[nodiscard]] cue::Result<std::string> compose_descriptor_locator(
        std::string_view a_projectLocator) noexcept override
    {
        cue::Result<std::string> project = normalize_project_locator(a_projectLocator);
        if (!project)
        {
            return project;
        }
        std::string descriptor;
        try
        {
            descriptor = *project.try_value();
            descriptor.append("\\CueProject.json");
        }
        catch (...)
        {
            terminate_allocation(*m_assertContext);
        }
        return cue::Result<std::string>::success(std::move(descriptor));
    }

    /// @brief Locatorが存在する場合だけRoot-bound Windows Filesystemを開く
    [[nodiscard]] cue::Result<std::unique_ptr<cue::FilesystemRoot>> open_root(
        std::string_view a_locator) noexcept override
    {
        cue::Result<std::string> normalized = normalize_project_locator(a_locator);
        if (!normalized)
        {
            return cue::Result<std::unique_ptr<cue::FilesystemRoot>>::failure(std::move(*normalized.try_error()));
        }
        cue::Result<std::wstring> path = to_utf16(*normalized.try_value(), *m_assertContext);
        if (!path)
        {
            return cue::Result<std::unique_ptr<cue::FilesystemRoot>>::failure(std::move(*path.try_error()));
        }
        cue::Result<std::wstring> extended = make_extended_path(std::move(*path.try_value()), *m_assertContext);
        if (!extended)
        {
            return cue::Result<std::unique_ptr<cue::FilesystemRoot>>::failure(std::move(*extended.try_error()));
        }
        const DWORD attributes = GetFileAttributesW(extended.try_value()->c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES)
        {
            const DWORD code = GetLastError();
            if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND)
            {
                std::unique_ptr<cue::FilesystemRoot> missing;
                return cue::Result<std::unique_ptr<cue::FilesystemRoot>>::success(std::move(missing));
            }
            return cue::Result<std::unique_ptr<cue::FilesystemRoot>>::failure(
                make_path_error(*m_assertContext, code, "Project locator inspection failed"));
        }
        return cue::create_windows_filesystem_root(*normalized.try_value(), *m_assertContext);
    }

    /// @brief Project Treeを上限付きで走査しReparse Pointを追跡せず表示用Metadataを集計する
    [[nodiscard]] cue::Result<cue::project_hub::ProjectStorageMetadata> inspect_project_storage(
        std::string_view a_locator) noexcept override
    {
        cue::Result<std::string> normalized = normalize_project_locator(a_locator);
        if (!normalized)
        {
            return cue::Result<cue::project_hub::ProjectStorageMetadata>::failure(std::move(*normalized.try_error()));
        }
        cue::Result<std::wstring> path = to_utf16(*normalized.try_value(), *m_assertContext);
        if (!path)
        {
            return cue::Result<cue::project_hub::ProjectStorageMetadata>::failure(std::move(*path.try_error()));
        }
        cue::Result<std::wstring> extended = make_extended_path(std::move(*path.try_value()), *m_assertContext);
        if (!extended)
        {
            return cue::Result<cue::project_hub::ProjectStorageMetadata>::failure(std::move(*extended.try_error()));
        }

        WIN32_FILE_ATTRIBUTE_DATA rootData{};
        if (GetFileAttributesExW(extended.try_value()->c_str(), GetFileExInfoStandard, &rootData) == FALSE)
        {
            return cue::Result<cue::project_hub::ProjectStorageMetadata>::failure(
                make_path_error(*m_assertContext, GetLastError(), "Project metadata root inspection failed"));
        }
        if ((rootData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0U ||
            (rootData.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U)
        {
            return cue::Result<cue::project_hub::ProjectStorageMetadata>::failure(cue::make_io_error(
                *m_assertContext, cue::IoError::UnsupportedEntry, "Project metadata root is not a regular directory"));
        }

        struct PendingDirectory final
        {
            std::wstring path;
            std::size_t depth;
        };
        try
        {
            cue::project_hub::ProjectStorageMetadata metadata{unix_milliseconds(rootData.ftLastWriteTime), 0U};
            std::vector<PendingDirectory> pending;
            pending.push_back({std::move(*extended.try_value()), 0U});
            std::size_t entryCount = 0U;
            while (!pending.empty())
            {
                PendingDirectory current = std::move(pending.back());
                pending.pop_back();
                cue::Result<std::wstring> pattern = append_child(current.path, L"*", *m_assertContext);
                if (!pattern)
                {
                    return cue::Result<cue::project_hub::ProjectStorageMetadata>::failure(
                        std::move(*pattern.try_error()));
                }
                WIN32_FIND_DATAW data{};
                FindHandle search(FindFirstFileExW(pattern.try_value()->c_str(), FindExInfoBasic, &data,
                                                   FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH));
                if (search.get() == INVALID_HANDLE_VALUE)
                {
                    const DWORD code = GetLastError();
                    if (code == ERROR_FILE_NOT_FOUND)
                    {
                        continue;
                    }
                    return cue::Result<cue::project_hub::ProjectStorageMetadata>::failure(
                        make_path_error(*m_assertContext, code, "Project metadata enumeration failed"));
                }
                while (true)
                {
                    const bool isDot = data.cFileName[0] == L'.' && data.cFileName[1] == L'\0';
                    const bool isDotDot =
                        data.cFileName[0] == L'.' && data.cFileName[1] == L'.' && data.cFileName[2] == L'\0';
                    if (!isDot && !isDotDot)
                    {
                        ++entryCount;
                        if (entryCount > k_maxProjectStorageEntryCount)
                        {
                            return cue::Result<cue::project_hub::ProjectStorageMetadata>::failure(
                                cue::make_io_error(*m_assertContext, cue::IoError::CapacityExceeded,
                                                   "Project metadata entry limit was exceeded"));
                        }
                        metadata.latestWriteMilliseconds =
                            (std::max)(metadata.latestWriteMilliseconds, unix_milliseconds(data.ftLastWriteTime));
                        if ((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0U)
                        {
                            if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U)
                            {
                                if (current.depth >= k_maxProjectStorageDepth)
                                {
                                    return cue::Result<cue::project_hub::ProjectStorageMetadata>::failure(
                                        cue::make_io_error(*m_assertContext, cue::IoError::CapacityExceeded,
                                                           "Project metadata depth limit was exceeded"));
                                }
                                cue::Result<std::wstring> child =
                                    append_child(current.path, data.cFileName, *m_assertContext);
                                if (!child)
                                {
                                    return cue::Result<cue::project_hub::ProjectStorageMetadata>::failure(
                                        std::move(*child.try_error()));
                                }
                                pending.push_back({std::move(*child.try_value()), current.depth + 1U});
                            }
                            else
                            {
                                const std::uint64_t fileSize =
                                    (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32U) | data.nFileSizeLow;
                                if (fileSize > (std::numeric_limits<std::uint64_t>::max)() - metadata.byteSize)
                                {
                                    return cue::Result<cue::project_hub::ProjectStorageMetadata>::failure(
                                        cue::make_io_error(*m_assertContext, cue::IoError::CapacityExceeded,
                                                           "Project metadata size overflowed"));
                                }
                                metadata.byteSize += fileSize;
                            }
                        }
                    }
                    if (FindNextFileW(search.get(), &data) == FALSE)
                    {
                        const DWORD code = GetLastError();
                        if (code != ERROR_NO_MORE_FILES)
                        {
                            return cue::Result<cue::project_hub::ProjectStorageMetadata>::failure(
                                make_path_error(*m_assertContext, code, "Project metadata enumeration failed"));
                        }
                        break;
                    }
                }
            }
            return cue::Result<cue::project_hub::ProjectStorageMetadata>::success(std::move(metadata));
        }
        catch (...)
        {
            terminate_allocation(*m_assertContext);
        }
    }

    /// @brief Project FolderをShellへ値で渡しWindows Explorerで開く
    [[nodiscard]] cue::Result<void> open_project_folder(std::string_view a_locator) noexcept override
    {
        cue::Result<std::string> normalized = normalize_project_locator(a_locator);
        if (!normalized)
        {
            return cue::Result<void>::failure(std::move(*normalized.try_error()));
        }
        cue::Result<std::wstring> path = to_utf16(*normalized.try_value(), *m_assertContext);
        if (!path)
        {
            return cue::Result<void>::failure(std::move(*path.try_error()));
        }
        SHELLEXECUTEINFOW request{};
        request.cbSize = sizeof(request);
        request.fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;
        request.lpVerb = L"open";
        request.lpFile = path.try_value()->c_str();
        request.nShow = SW_SHOWNORMAL;
        if (ShellExecuteExW(&request) == FALSE)
        {
            return cue::Result<void>::failure(cue::project_hub::reclassify_project_hub_error(
                *m_assertContext, cue::project_hub::ProjectHubError::ProjectFolderOpenFailed,
                "Project folder could not be opened in Windows Explorer",
                make_path_error(*m_assertContext, GetLastError(), "Windows Shell folder open failed")));
        }
        return cue::Result<void>::success();
    }

    /// @brief Cryptographic Random SourceからRFC 4122 Version 4 ProjectIdを生成する
    [[nodiscard]] cue::Result<cue::ProjectId> next_project_id() noexcept override
    {
        cue::Result<std::string> identity = generate_identity_text(*m_assertContext);
        if (!identity)
        {
            return cue::Result<cue::ProjectId>::failure(std::move(*identity.try_error()));
        }
        return cue::ProjectId::parse(*identity.try_value(), *m_assertContext);
    }

    /// @brief Cryptographic Random SourceからDefault SceneAssetId文字列を生成する
    [[nodiscard]] cue::Result<std::string> next_scene_asset_id() noexcept override
    {
        return generate_identity_text(*m_assertContext);
    }

  private:
    const cue::AssertContext *m_assertContext;
};
} // namespace

namespace cue::project_hub
{
Result<std::unique_ptr<ProjectHubPlatform>> create_windows_project_hub_platform(
    const AssertContext &a_assertContext) noexcept
{
    try
    {
        std::unique_ptr<ProjectHubPlatform> platform = std::make_unique<WindowsProjectHubPlatform>(a_assertContext);
        return Result<std::unique_ptr<ProjectHubPlatform>>::success(std::move(platform));
    }
    catch (...)
    {
        terminate_allocation(a_assertContext);
    }
}
} // namespace cue::project_hub
