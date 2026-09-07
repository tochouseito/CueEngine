#pragma once

#include <Cue/Foundation/Result.h>

#include <string>
#include <string_view>

namespace cue
{
class AssertContext;

/// @brief Engine 内部の UTF-8 文字列を Win32 W API 用の UTF-16 へ変換する
///
/// Windows 固有の文字表現をこの境界へ閉じ込め、Platform 非依存 API の文字コードを UTF-8 に保つ
[[nodiscard]] Result<std::wstring> utf8_to_utf16(std::string_view a_text,
                                                 const AssertContext &a_assertContext) noexcept;

/// @brief Win32 W APIが返したUTF-16をEngine内部のUTF-8文字列へ変換する
[[nodiscard]] Result<std::string> utf16_to_utf8(std::wstring_view a_text,
                                                const AssertContext &a_assertContext) noexcept;
} // namespace cue
