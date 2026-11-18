#pragma once

#include <Foundation/Result.h>
#include <RenderData/RenderScene.h>

namespace cue
{
class World;

/// @brief Owner Thread で同期済み World を読み取り、独立した Snapshot を生成する
/// 有効 Mesh があれば Camera は一つ必要。抽出失敗で World の状態は変更しない
/// Transform は World の initialize / update 境界で同期してから呼ぶ
[[nodiscard]] Result<SceneRenderSnapshot> extract_render_scene(World &a_world);
} // namespace cue
