#pragma once

#include <vector>

#include <Foundation/Path.h>
#include <Foundation/Result.h>
#include <SceneRender/SceneRenderer.h>

namespace cue
{
class AssetCatalog;
class IFileSystem;

/// @brief 描画登録前の Mesh・Material・Texture を同じ Asset 世代として所有する
struct SceneAssets final
{
    std::vector<SceneMeshDesc> meshes;
    std::vector<SceneMaterialDesc> materials;
};

/// @brief 制作時の Catalog を検証して、Owner Thread で描画用の所有値を用意する
[[nodiscard]] Result<SceneAssets> load_project_assets(IFileSystem &a_files, const Path &a_contentRoot);
/// @brief 検証済み Catalog の Model と Material を描画用に読み込む
[[nodiscard]] Result<SceneAssets> load_project_assets(IFileSystem &a_files, const AssetCatalog &a_catalog);

/// @brief 製品 App の配置先にある Pack を読む。旧 App では必要な場合だけ Pack の存在を要求する
[[nodiscard]] Result<SceneAssets> load_product_assets(IFileSystem &a_files, const Path &a_appDirectory,
                                                       bool a_requiresPack);
} // namespace cue
