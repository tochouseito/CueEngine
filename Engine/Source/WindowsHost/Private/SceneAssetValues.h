#pragma once

#include <map>
#include <memory>
#include <new>
#include <span>
#include <string>
#include <utility>

#include <Assets/AssetCatalog.h>
#include <Assets/AssetPack.h>
#include <Assets/ModelAsset.h>
#include <WindowsHost/SceneAssets.h>

namespace cue::detail
{
/// @brief Authoring / Pack 両方で Model の所有データを MeshPool が受け取る登録値へ変換する
template <class F>
[[nodiscard]] Result<std::vector<SceneMeshDesc>> collect_scene_meshes(std::span<const ModelAsset> a_models,
                                                                      F &&a_readModel)
{
    using MeshResult = Result<std::vector<SceneMeshDesc>>;
    try
    {
        std::vector<SceneMeshDesc> meshes;
        meshes.reserve(a_models.size());
        for (const auto &asset : a_models)
        {
            auto data = a_readModel(asset);
            if (!data.has_value())
                return MeshResult::failure(*data.try_error());
            auto model = data.take_value();
            if (model.asset.assetId != asset.assetId || model.asset.materialAssetId != asset.materialAssetId)
                return MeshResult::failure({ErrorCategory::InvalidState, "SceneAssets.model_changed:" + asset.assetId});
            meshes.push_back({asset.assetId, std::make_shared<const MeshData>(std::move(model.mesh))});
        }
        return MeshResult::success(std::move(meshes));
    }
    catch (const std::bad_alloc &)
    {
        return MeshResult::failure({ErrorCategory::PlatformFailure, "SceneAssets.mesh_allocation"});
    }
}

/// @brief 複数の Material が同じ Texture Asset ID を使うとき、展開した画素を共有する
template <class F>
[[nodiscard]] Result<std::vector<SceneMaterialDesc>> collect_scene_materials(std::span<const MaterialAsset> a_materials,
                                                                             F &&a_readTexture)
{
    using MaterialResult = Result<std::vector<SceneMaterialDesc>>;
    try
    {
        std::vector<SceneMaterialDesc> materials;
        materials.reserve(a_materials.size());
        std::map<std::string, std::shared_ptr<const SceneTextureRgba8>, std::less<>> textures;
        for (const auto &asset : a_materials)
        {
            SceneMaterialDesc material;
            material.assetId = asset.assetId;
            material.baseColor = asset.baseColor;
            if (asset.baseColorTexture)
            {
                auto [entry, inserted] = textures.try_emplace(*asset.baseColorTexture);
                if (inserted)
                {
                    auto decoded = a_readTexture(entry->first);
                    if (!decoded.has_value())
                        return MaterialResult::failure(*decoded.try_error());
                    auto pixels = decoded.take_value();
                    entry->second = std::make_shared<SceneTextureRgba8>(
                        SceneTextureRgba8{pixels.width, pixels.height, pixels.colorSpace == TextureColorSpace::Srgb,
                                          std::move(pixels.rgba)});
                }
                material.texture = entry->second;
            }
            materials.push_back(std::move(material));
        }
        return MaterialResult::success(std::move(materials));
    }
    catch (const std::bad_alloc &)
    {
        return MaterialResult::failure({ErrorCategory::PlatformFailure, "SceneMaterials.allocation"});
    }
}
} // namespace cue::detail
