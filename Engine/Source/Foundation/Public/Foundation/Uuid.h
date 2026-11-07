#pragma once

#include <cstddef>
#include <string_view>

namespace cue
{
/// @brief 小文字の 8-4-4-4-12 UUID 表記を検証する。全零を拒否し、Version / Variant は限定しない
/// 入力を呼出中だけ借用し、Allocation や Global 状態を使わず任意 Thread で利用できる
[[nodiscard]] inline bool is_valid_uuid_text(std::string_view a_text) noexcept
{
    if (a_text.size() != 36)
        return false;
    bool hasValue = false;
    for (std::size_t index = 0; index < a_text.size(); ++index)
    {
        const char value = a_text[index];
        if (index == 8 || index == 13 || index == 18 || index == 23)
        {
            if (value != '-')
                return false;
        }
        else
        {
            if (!((value >= '0' && value <= '9') || (value >= 'a' && value <= 'f')))
                return false;
            hasValue = hasValue || value != '0';
        }
    }
    return hasValue;
}
} // namespace cue
