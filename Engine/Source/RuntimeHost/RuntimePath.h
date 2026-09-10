#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace cue::runtime_host::detail
{
/// @brief Engine内部のUTF-8 Path表現をWindows Native Pathへ明示的に変換する
[[nodiscard]] inline std::filesystem::path filesystem_path_from_utf8(std::string_view a_text)
{
    std::u8string encoded;
    encoded.reserve(a_text.size());
    for (const unsigned char byte : a_text)
    {
        encoded.push_back(static_cast<char8_t>(byte));
    }
    return std::filesystem::path(encoded);
}
} // namespace cue::runtime_host::detail
