#include <Cue/Foundation/Windows/UtfConversion.h>

#include <cstddef>
#include <limits>
#include <utility>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace cue
{
namespace
{
/// @brief Win32変換失敗を不正SequenceまたはPlatform失敗へ分類する
Error conversion_error(const char* a_operation, DWORD a_nativeCode)
{
    // 不正な文字列は入力 Error とし、その他の Win32 失敗は Native 診断値を残す
    const auto category = a_nativeCode == ERROR_NO_UNICODE_TRANSLATION
        ? ErrorCategory::InvalidArgument
        : ErrorCategory::PlatformFailure;
    return {category, a_operation, a_nativeCode};
}
} // namespace

/// @brief UTF-8を代替文字なしでWindows UTF-16へ変換する
Result<std::wstring> utf8_to_utf16(std::string_view a_text)
{
    using ConversionResult = Result<std::wstring>;
    // 空入力は Win32 変換 API へ渡さず空の成功値にする
    if (a_text.empty())
    {
        return ConversionResult::success({});
    }
    // Win32 API が受け取る文字数は int で表す
    if (a_text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
    {
        return ConversionResult::failure({ErrorCategory::InvalidArgument, "utf8_to_utf16.length"});
    }

    // 必要な UTF-16 Code Unit 数を先に取得し、不正な UTF-8 を拒否する
    const int sourceLength = static_cast<int>(a_text.size());
    const int convertedLength = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, a_text.data(),
                                                    sourceLength, nullptr, 0);
    if (convertedLength == 0)
    {
        return ConversionResult::failure(conversion_error("MultiByteToWideChar", GetLastError()));
    }

    // 明示した入力長で埋め込み NUL も含めて変換する
    std::wstring converted(static_cast<std::size_t>(convertedLength), L'\0');
    const int writtenLength = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, a_text.data(),
                                                  sourceLength, converted.data(), convertedLength);
    if (writtenLength != convertedLength)
    {
        return ConversionResult::failure(conversion_error("MultiByteToWideChar", GetLastError()));
    }
    return ConversionResult::success(std::move(converted));
}

/// @brief Windows UTF-16を代替文字なしでUTF-8へ変換する
Result<std::string> utf16_to_utf8(std::wstring_view a_text)
{
    using ConversionResult = Result<std::string>;
    // 空入力は Win32 変換 API へ渡さず空の成功値にする
    if (a_text.empty())
    {
        return ConversionResult::success({});
    }
    // Win32 API が受け取る文字数は int で表す
    if (a_text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
    {
        return ConversionResult::failure({ErrorCategory::InvalidArgument, "utf16_to_utf8.length"});
    }

    // 必要な UTF-8 Byte 数を先に取得し、不正な Surrogate を拒否する
    const int sourceLength = static_cast<int>(a_text.size());
    const int convertedLength = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, a_text.data(),
                                                    sourceLength, nullptr, 0, nullptr, nullptr);
    if (convertedLength == 0)
    {
        return ConversionResult::failure(conversion_error("WideCharToMultiByte", GetLastError()));
    }

    // 明示した入力長で埋め込み NUL も含めて変換する
    std::string converted(static_cast<std::size_t>(convertedLength), '\0');
    const int writtenLength = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, a_text.data(),
                                                  sourceLength, converted.data(), convertedLength,
                                                  nullptr, nullptr);
    if (writtenLength != convertedLength)
    {
        return ConversionResult::failure(conversion_error("WideCharToMultiByte", GetLastError()));
    }
    return ConversionResult::success(std::move(converted));
}
} // namespace cue
