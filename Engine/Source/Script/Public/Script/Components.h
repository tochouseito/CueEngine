#pragma once

#include <string>

#include <Math/Transform.h>

namespace cue
{
/// @brief Camera の投影設定を読み取る SDK の所有値
struct ScriptCamera final
{
    float verticalFovRadians;
    float nearClip;
    float farClip;
};

/// @brief Mesh Renderer が参照している Asset ID を複写した SDK の所有値
struct ScriptMeshRenderer final
{
    std::string assetId;
};
} // namespace cue
