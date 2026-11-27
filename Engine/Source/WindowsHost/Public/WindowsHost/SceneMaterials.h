#pragma once

#include <vector>

#include <Foundation/Path.h>
#include <Foundation/Result.h>
#include <SceneRender/SceneRenderer.h>

namespace cue
{
class AssetCatalog;
class IFileSystem;

/// @brief 制作時の Material と PNG を Owner Thread で検証して描画用の所有値を用意する
[[nodiscard]] Result<std::vector<SceneMaterialDesc>> load_project_materials(IFileSystem &a_files,
                                                                            const Path &a_contentRoot);

/// @brief 検証済み Catalog から描画用 Material を更新する。失敗時は現在の GPU 登録を維持する
[[nodiscard]] Result<void> set_project_materials(ISceneRenderer &a_renderer, IFileSystem &a_files,
                                                 const AssetCatalog &a_catalog);

/// @brief 製品 App の配置先にある Pack を読む。旧 App では必要な場合だけ Pack の存在を要求する
[[nodiscard]] Result<std::vector<SceneMaterialDesc>> load_product_materials(IFileSystem &a_files,
                                                                            const Path &a_appDirectory,
                                                                            bool a_requiresPack);
} // namespace cue
