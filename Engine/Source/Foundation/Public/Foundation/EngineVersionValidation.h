#pragma once

#include <algorithm>
#include <string_view>

namespace cue
{
namespace detail::engine_version
{
/// @brief SemVer の識別子で使える ASCII だけを許可する
inline bool identifier_character(char a_value) noexcept
{
    return (a_value >= 'a' && a_value <= 'z') || (a_value >= 'A' && a_value <= 'Z') ||
           (a_value >= '0' && a_value <= '9') || a_value == '-';
}

/// @brief 数値 Version の先頭 Zero と空要素を拒否する
inline bool numeric_identifier(std::string_view a_value) noexcept
{
    return !a_value.empty() && (a_value.size() == 1 || a_value.front() != '0') &&
           std::all_of(a_value.begin(), a_value.end(), [](char a_digit) { return a_digit >= '0' && a_digit <= '9'; });
}

/// @brief Dot 区切りの識別子を検証し、Pre-release の数値だけ先頭 Zero を拒否する
inline bool identifier_list(std::string_view a_text, bool a_isPrerelease) noexcept
{
    if (a_text.empty())
        return false;
    while (!a_text.empty())
    {
        const auto dot = a_text.find('.');
        const auto part = a_text.substr(0, dot);
        if (part.empty() || !std::all_of(part.begin(), part.end(), identifier_character))
            return false;
        const bool isNumeric =
            std::all_of(part.begin(), part.end(), [](char a_value) { return a_value >= '0' && a_value <= '9'; });
        if (a_isPrerelease && isNumeric && !numeric_identifier(part))
            return false;
        if (dot == std::string_view::npos)
            return true;
        a_text.remove_prefix(dot + 1);
    }
    return false;
}

} // namespace detail::engine_version

/// @brief Project と配布 Descriptor に保存できる最大 128 Byte の SemVer を検証する
/// 入力は呼出中だけ借用し、Allocation と共有状態を使わず任意 Thread で呼べる
[[nodiscard]] inline bool is_valid_engine_version(std::string_view a_version) noexcept
{
    if (a_version.empty() || a_version.size() > 128)
        return false;
    const auto plus = a_version.find('+');
    if (plus != std::string_view::npos &&
        !detail::engine_version::identifier_list(a_version.substr(plus + 1), false))
        return false;
    auto main = a_version.substr(0, plus);
    const auto dash = main.find('-');
    if (dash != std::string_view::npos && !detail::engine_version::identifier_list(main.substr(dash + 1), true))
        return false;
    auto core = main.substr(0, dash);
    for (int index = 0; index < 3; ++index)
    {
        const auto dot = core.find('.');
        if (!detail::engine_version::numeric_identifier(core.substr(0, dot)) ||
            (index == 2) != (dot == std::string_view::npos))
            return false;
        if (dot != std::string_view::npos)
            core.remove_prefix(dot + 1);
    }
    return true;
}
} // namespace cue
