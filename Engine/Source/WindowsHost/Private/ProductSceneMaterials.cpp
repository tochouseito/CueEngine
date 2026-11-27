#include <WindowsHost/SceneMaterials.h>

#include <Assets/AssetPack.h>
#include <Platform/FileSystem.h>

#include "SceneMaterialValues.h"

namespace cue
{
Result<std::vector<SceneMaterialDesc>> load_product_materials(IFileSystem &a_files, const Path &a_appDirectory,
                                                              bool a_requiresPack)
{
    using MaterialResult = Result<std::vector<SceneMaterialDesc>>;
    auto file = a_appDirectory.join(k_gameAssetPack);
    if (!file.has_value())
        return MaterialResult::failure(*file.try_error());
    auto info = a_files.stat(*file.try_value());
    if (!info.has_value())
        return MaterialResult::failure(*info.try_error());
    if (!*info.try_value())
        return a_requiresPack ? MaterialResult::failure({ErrorCategory::InvalidArgument, "GameApp.asset_pack_missing"})
                              : MaterialResult::success({});
    auto pack = AssetPack::open(a_files, *file.try_value());
    if (!pack.has_value())
        return MaterialResult::failure(*pack.try_error());
    return detail::collect_scene_materials(pack.try_value()->materials(), [&](const std::string &a_id)
                                           { return pack.try_value()->read_texture(a_id); });
}
} // namespace cue
