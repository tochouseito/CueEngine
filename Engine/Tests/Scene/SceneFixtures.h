#pragma once

#include <string>
#include <string_view>

namespace cue::tests
{
inline constexpr std::string_view k_sceneText =
    "formatVersion=1\nsceneId=8d94c2b1-9b6a-435c-ade7-c01f473c3951\nname=RotateCube\n\n"
    "[object 9a00d731-7759-424c-9ade-fd086d624551]\nname=Cube\nactive=true\n"
    "transform.position=1,2,3\ntransform.rotation=0,0,0,1\ntransform.scale=2,3,4\n";
inline constexpr std::string_view k_cubeId = "9a00d731-7759-424c-9ade-fd086d624551";

/// @brief 有効な入力の一箇所だけを変更し、各検証分岐を独立させる
inline std::string scene_replaced(std::string_view a_from, std::string_view a_to)
{
    std::string text(k_sceneText);
    const auto position = text.find(a_from);
    if (position != std::string::npos)
        text.replace(position, a_from.size(), a_to);
    return text;
}
} // namespace cue::tests
