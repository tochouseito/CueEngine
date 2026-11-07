#pragma once

#include <optional>
#include <string>
#include <vector>

#include <Math/Matrix4x4.h>

namespace cue
{
class World;

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

/// @brief Owner Thread で同期済み World を読み取り、独立した Snapshot を生成する
/// 有効 Mesh があれば Camera は一つ必要。抽出失敗で World の状態は変更しない
/// Transform は World の initialize / update 境界で同期してから呼ぶ
[[nodiscard]] Result<SceneRenderSnapshot> extract_render_scene(World &a_world);
} // namespace cue
