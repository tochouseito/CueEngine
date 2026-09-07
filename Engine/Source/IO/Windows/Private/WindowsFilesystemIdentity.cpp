#include "WindowsFilesystemIdentity.h"

#include <Cue/IO/Error.h>

#include <array>
#include <string>
#include <string_view>
#include <utility>

namespace
{
/// @brief Windows Error Code付きIO Errorを構築する
[[nodiscard]] cue::Error make_identity_error(const cue::AssertContext &a_assertContext, DWORD a_nativeCode,
                                             std::string_view a_message) noexcept
{
    return cue::make_io_error(a_assertContext, cue::IoError::OutsideRoot, a_message,
                              static_cast<std::int64_t>(a_nativeCode));
}

/// @brief ASCII Hex文字をNibbleへ変換する
[[nodiscard]] bool parse_hex_nibble(wchar_t a_character, std::uint8_t &a_value) noexcept
{
    if (a_character >= L'0' && a_character <= L'9')
    {
        a_value = static_cast<std::uint8_t>(a_character - L'0');
        return true;
    }
    if (a_character >= L'a' && a_character <= L'f')
    {
        a_value = static_cast<std::uint8_t>(a_character - L'a' + 10);
        return true;
    }
    if (a_character >= L'A' && a_character <= L'F')
    {
        a_value = static_cast<std::uint8_t>(a_character - L'A' + 10);
        return true;
    }
    return false;
}

/// @brief Volume GUID Path先頭のGUIDを表示順の16 Byteへ変換する
[[nodiscard]] bool parse_volume_guid(std::wstring_view a_path, std::array<std::uint8_t, 16> &a_bytes) noexcept
{
    constexpr std::wstring_view k_prefix = LR"(\\?\Volume{)";
    if (!a_path.starts_with(k_prefix))
    {
        return false;
    }

    std::size_t byteIndex = 0U;
    bool hasHighNibble = false;
    std::uint8_t highNibble = 0U;
    for (std::size_t index = k_prefix.size(); index < a_path.size(); ++index)
    {
        const wchar_t character = a_path[index];
        if (character == L'}')
        {
            return byteIndex == a_bytes.size() && !hasHighNibble && index + 1U < a_path.size() &&
                   a_path[index + 1U] == L'\\';
        }
        if (character == L'-')
        {
            continue;
        }

        std::uint8_t nibble = 0U;
        if (!parse_hex_nibble(character, nibble) || byteIndex >= a_bytes.size())
        {
            return false;
        }
        if (!hasHighNibble)
        {
            highNibble = nibble;
            hasHighNibble = true;
            continue;
        }
        a_bytes[byteIndex] = static_cast<std::uint8_t>((highNibble << 4U) | nibble);
        ++byteIndex;
        hasHighNibble = false;
    }
    return false;
}

/// @brief 8 Byteを表示順の64-bit値へ変換する
[[nodiscard]] std::uint64_t pack_bytes(const std::uint8_t *a_bytes) noexcept
{
    std::uint64_t value = 0U;
    for (std::size_t index = 0U; index < 8U; ++index)
    {
        value = (value << 8U) | static_cast<std::uint64_t>(a_bytes[index]);
    }
    return value;
}
} // namespace

namespace cue::windows_io
{
Result<NativeFilesystemIdentity> inspect_native_filesystem_identity(HANDLE a_handle,
                                                                    const AssertContext &a_assertContext) noexcept
{
    FILE_ID_INFO fileIdentity{};
    if (GetFileInformationByHandleEx(a_handle, FileIdInfo, &fileIdentity, sizeof(fileIdentity)) == FALSE)
    {
        return Result<NativeFilesystemIdentity>::failure(
            make_identity_error(a_assertContext, GetLastError(), "Filesystem 128-bit file identity query failed"));
    }

    const DWORD required = GetFinalPathNameByHandleW(a_handle, nullptr, 0U, FILE_NAME_OPENED | VOLUME_NAME_GUID);
    if (required == 0U)
    {
        return Result<NativeFilesystemIdentity>::failure(
            make_identity_error(a_assertContext, GetLastError(), "Filesystem volume object query failed"));
    }

    std::wstring finalPath;
    try
    {
        finalPath.resize(required);
    }
    catch (...)
    {
        report_assert_failure(a_assertContext, "Filesystem identity path allocation failed");
    }
    const DWORD written =
        GetFinalPathNameByHandleW(a_handle, finalPath.data(), required, FILE_NAME_OPENED | VOLUME_NAME_GUID);
    if (written == 0U || written >= required)
    {
        return Result<NativeFilesystemIdentity>::failure(
            make_identity_error(a_assertContext, GetLastError(), "Filesystem volume object query failed"));
    }
    finalPath.resize(written);

    std::array<std::uint8_t, 16> volumeGuid{};
    if (!parse_volume_guid(finalPath, volumeGuid))
    {
        return Result<NativeFilesystemIdentity>::failure(
            make_identity_error(a_assertContext, ERROR_INVALID_DATA, "Filesystem volume GUID path was invalid"));
    }

    NativeFilesystemIdentity identity;
    identity.volumeHigh = pack_bytes(volumeGuid.data());
    identity.volumeLow = pack_bytes(volumeGuid.data() + 8U);
    identity.entryHigh = pack_bytes(fileIdentity.FileId.Identifier);
    identity.entryLow = pack_bytes(fileIdentity.FileId.Identifier + 8U);
    return Result<NativeFilesystemIdentity>::success(std::move(identity));
}
} // namespace cue::windows_io
