#pragma once

#include <map>
#include <memory>
#include <new>
#include <span>
#include <string>
#include <utility>

#include <Assets/AssetCatalog.h>
#include <Assets/AssetPack.h>
#include <WindowsHost/SceneMaterials.h>

namespace cue::detail
{
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
