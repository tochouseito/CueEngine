#include "UtfConversion.h"

#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Error.h>
#include <Cue/Foundation/Windows/UtfConversion.h>
#include <Cue/Platform/Windows/WindowsPlatform.h>

#include <string_view>
#include <utility>

namespace
{
constexpr std::int64_t k_invalidUtf8 = 2;
constexpr std::int64_t k_invalidUtf16 = 11;

/// @brief Windows UTF 変換境界で使用する Conversion Error を生成し、呼び出し元へ返す
[[nodiscard]] cue::Error make_conversion_error(const cue::AssertContext &a_context, std::int64_t a_code,
                                               std::string_view a_summary, std::int64_t a_nativeCode) noexcept
{
    cue::ErrorCode code = cue::ErrorCode::create(a_context.fatal_handler(), "Cue.Platform.Windows", a_code);
    cue::NativeError nativeError = cue::NativeError::create(a_context.fatal_handler(), "Win32", a_nativeCode);
    return cue::Error::create(a_context.fatal_handler(), std::move(code), a_summary, std::move(nativeError));
}

/// @brief UTF-8 変換状態を Window Title 用の既存 Error Summary へ対応付ける
[[nodiscard]] std::string_view select_utf8_summary(cue::WindowsUtfConversionStatus a_status) noexcept
{
    switch (a_status)
    {
    case cue::WindowsUtfConversionStatus::InputTooLong:
        return "UTF-8 title is too long";
    case cue::WindowsUtfConversionStatus::InvalidSequence:
        return "UTF-8 title is invalid";
    case cue::WindowsUtfConversionStatus::NativeFailure:
        return "UTF-8 title conversion failed";
    case cue::WindowsUtfConversionStatus::Success:
        break;
    }

    return "UTF-8 title conversion returned an invalid status";
}

/// @brief UTF-16 変換状態を Command Line Argument 用の既存 Error Summary へ対応付ける
[[nodiscard]] std::string_view select_utf16_summary(cue::WindowsUtfConversionStatus a_status) noexcept
{
    switch (a_status)
    {
    case cue::WindowsUtfConversionStatus::InputTooLong:
        return "UTF-16 argument is too long";
    case cue::WindowsUtfConversionStatus::InvalidSequence:
        return "UTF-16 argument is invalid";
    case cue::WindowsUtfConversionStatus::NativeFailure:
        return "UTF-16 argument conversion failed";
    case cue::WindowsUtfConversionStatus::Success:
        break;
    }

    return "UTF-16 argument conversion returned an invalid status";
}
} // namespace

namespace cue
{
Result<std::wstring> utf8_to_utf16(std::string_view a_text, const AssertContext &a_assertContext) noexcept
{
    std::wstring result;
    const WindowsUtfConversionResult conversion =
        convert_utf8_to_windows_utf16(a_text, result, a_assertContext.fatal_handler());

    if (conversion.status != WindowsUtfConversionStatus::Success)
    {
        return Result<std::wstring>::failure(make_conversion_error(
            a_assertContext, k_invalidUtf8, select_utf8_summary(conversion.status), conversion.nativeCode));
    }

    return Result<std::wstring>::success(std::move(result));
}

/// @brief Win32 W APIが返したUTF-16をEngine内部のUTF-8文字列へ変換する
Result<std::string> utf16_to_utf8(std::wstring_view a_text, const AssertContext &a_assertContext) noexcept
{
    std::string result;
    const WindowsUtfConversionResult conversion =
        convert_windows_utf16_to_utf8(a_text, result, a_assertContext.fatal_handler());

    if (conversion.status != WindowsUtfConversionStatus::Success)
    {
        return Result<std::string>::failure(make_conversion_error(
            a_assertContext, k_invalidUtf16, select_utf16_summary(conversion.status), conversion.nativeCode));
    }

    return Result<std::string>::success(std::move(result));
}

Result<std::string> convert_windows_argument_to_utf8(std::wstring_view a_text,
                                                     const AssertContext &a_assertContext) noexcept
{
    return utf16_to_utf8(a_text, a_assertContext);
}
} // namespace cue
