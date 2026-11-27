#pragma once

#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <DX12/DX12GpuResource.h>
#include <SceneRender/SceneRenderer.h>

namespace cue::dx12
{
class DX12Backend;

// HLSL の Texture Table と同じ上限。0 は未指定のために予約する
inline constexpr std::uint32_t k_sceneTextureCount = 64;

/// @brief Descriptor と GPU データを同時に固定し、参照する Frame が完了するまで所有する
struct SceneMaterialSet final
{
    std::map<std::string, std::uint32_t, std::less<>> ids;
    std::unique_ptr<DX12GpuResource> records;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> textures;
};

/// @brief Texture と Material Buffer の Copy 完了後に、変更不可の描画世代を返す
[[nodiscard]] Result<std::shared_ptr<const SceneMaterialSet>> create_scene_materials(
    DX12Backend &a_backend, std::span<const SceneMaterialDesc> a_materials);
} // namespace cue::dx12
