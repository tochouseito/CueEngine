#pragma once

#include <optional>
#include <string>
#include <vector>

#include <Math/Matrix4x4.h>
#include <Math/Scalar.h>

namespace cue
{
/// @brief 描画 Frame が所有する Camera の値。Aspect は描画先の寸法から決める
struct RenderCameraData final
{
    math::float4x4 view;
    float verticalFovRadians = math::degrees_to_radians(60.0f);
    float nearClip = 0.1f;
    float farClip = 1000.0f;
};

/// @brief World の Component を参照せず、行列と永続 Mesh ID を独立して保持する
struct RenderObjectData final
{
    math::float4x4 world;
    std::string meshId;
};

/// @brief 更新側から描画側へ Move する所有値。Entity / Component / Window の Pointer は持たない
struct SceneRenderSnapshot final
{
    std::optional<RenderCameraData> camera;
    std::vector<RenderObjectData> objects;
};

} // namespace cue
