#pragma once

#include <Cue/Foundation/Result.h>

#include <string>
#include <string_view>

namespace cue
{
static_assert(sizeof(wchar_t) == 2, "Cue.Foundation.Windows requires UTF-16 wchar_t");

/// @brief UTF-8のCode Unit列をWindows UTF-16へ厳密に変換する
///
/// 入力は呼出中だけ借用し、出力はResultが所有する。埋め込みNULは保持する
/// 不正Sequence・長さ超過ではErrorを返し、部分出力を公開しない
/// 複数Threadから独立した入力で同時に呼べる。Allocation失敗は例外として伝播する
[[nodiscard]] Result<std::wstring> utf8_to_utf16(std::string_view a_text);

/// @brief Windows UTF-16のCode Unit列をUTF-8へ厳密に変換する
///
/// 入力は呼出中だけ借用し、出力はResultが所有する。埋め込みNULは保持する
/// 不正Sequence・長さ超過ではErrorを返し、部分出力を公開しない
/// 複数Threadから独立した入力で同時に呼べる。Allocation失敗は例外として伝播する
[[nodiscard]] Result<std::string> utf16_to_utf8(std::wstring_view a_text);
} // namespace cue
