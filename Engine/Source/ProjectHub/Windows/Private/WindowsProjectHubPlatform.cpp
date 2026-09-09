#include <Cue/ProjectHub/Windows/WindowsProjectHubPlatform.h>

#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Windows/UtfConversion.h>
#include <Cue/IO/Error.h>
#include <Cue/IO/RelativePath.h>
#include <Cue/IO/Windows/WindowsFilesystem.h>
#include <Cue/ProjectHub/Error.h>

#include <Windows.h>
#include <bcrypt.h>

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

namespace
{
constexpr std::size_t k_maxWindowsPathLength = 32767;

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
