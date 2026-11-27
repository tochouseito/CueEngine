#include <WindowsHost/SceneAssets.h>

#include <Assets/AssetPack.h>
#include <Platform/FileSystem.h>

#include "SceneAssetValues.h"

namespace cue
{
Result<SceneAssets> load_product_assets(IFileSystem &a_files, const Path &a_appDirectory, bool a_requiresPack)
{
    using AssetResult = Result<SceneAssets>;
    auto file = a_appDirectory.join(k_gameAssetPack);
    if (!file.has_value())
        return AssetResult::failure(*file.try_error());
    auto info = a_files.stat(*file.try_value());
    if (!info.has_value())
        return AssetResult::failure(*info.try_error());
    if (!*info.try_value())
        return a_requiresPack ? AssetResult::failure({ErrorCategory::InvalidArgument, "GameApp.asset_pack_missing"})
                              : AssetResult::success({});
    auto pack = AssetPack::open(a_files, *file.try_value());
    if (!pack.has_value())
        return AssetResult::failure(*pack.try_error());
    auto meshes = detail::collect_scene_meshes(pack.try_value()->models(), [&](const ModelAsset &a_model)
                                               { return pack.try_value()->read_model(a_model.assetId); });
    if (!meshes.has_value())
        return AssetResult::failure(*meshes.try_error());
    auto materials = detail::collect_scene_materials(pack.try_value()->materials(), [&](const std::string &a_id)
                                                     { return pack.try_value()->read_texture(a_id); });
    if (!materials.has_value())
        return AssetResult::failure(*materials.try_error());
    return AssetResult::success({meshes.take_value(), materials.take_value()});
}
} // namespace cue
