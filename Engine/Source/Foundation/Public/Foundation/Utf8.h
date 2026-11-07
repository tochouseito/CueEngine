#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace cue
{
/// @brief UTF-8 の最短表現・Unicode 範囲・継続 Byte と NUL 不在を Allocation なしで検証する
///
/// 入力は呼出中だけ借用する。空文字は有効で、独立した入力の同時検証も可能
[[nodiscard]] inline bool is_valid_utf8_text(std::string_view a_text) noexcept
{
    for (std::size_t offset = 0; offset < a_text.size();)
    {
        const auto first = static_cast<unsigned char>(a_text[offset++]);
        if (first == 0)
        {
            return false;
        }
        if (first < 0x80)
        {
            continue;
        }
        // 先頭 Byte から必要な継続数を求め、不正な先頭と途中終端を拒否する
        const unsigned count = first >= 0xc2 && first <= 0xdf   ? 1
                               : first >= 0xe0 && first <= 0xef ? 2
                               : first >= 0xf0 && first <= 0xf4 ? 3
                                                                : 0;
        if (count == 0 || count > a_text.size() - offset)
        {
            return false;
        }
        std::uint32_t code = first & (0x7f >> (count + 1));
        for (unsigned index = 0; index < count; ++index)
        {
            const auto next = static_cast<unsigned char>(a_text[offset++]);
            if ((next & 0xc0) != 0x80)
            {
                return false;
            }
            code = (code << 6) | (next & 0x3f);
        }
        // Overlong と UTF-16 Surrogate、Unicode の範囲外を受け入れない
        if ((count == 1 && code < 0x80) || (count == 2 && code < 0x800) || (count == 3 && code < 0x10000) ||
            code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff))
        {
            return false;
        }
    }
    return true;
}
} // namespace cue
