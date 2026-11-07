#pragma once

#include <Runtime/WorldHandle.h>
#include <Scene/Scene.h>

namespace cue
{
class World;

/// @brief 検証済み Scene を新しい Instance として実体化し、成功時だけ Handle を返す
/// World の Owner Thread の通常操作または defer 内で呼ぶ。Scene は呼出中だけ借用し、結果は依存しない
/// 同じ Scene の再実体化も新しい Entity を作る。Transform 行列は次の World 初期化・更新境界で同期する
/// 検証失敗は World を変更しない。構造途中の終端失敗は World の規約に従い shutdown して破棄する
[[nodiscard]] Result<SceneInstanceHandle> instantiate_scene(const Scene &a_scene, World &a_world);
} // namespace cue
