#include "DX12SceneMaterials.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>

#include <DX12/DX12Backend.h>
#include <DX12/DX12CommandPool.h>
#include <DX12/DX12RenderDevice.h>

namespace cue::dx12
{
namespace
{
constexpr std::size_t k_maxMaterialAssets = 4095;
constexpr std::uint64_t k_textureByteBudget = 256ull * 1024 * 1024;

/// @brief StructuredBuffer の各要素の Layout と対応する描画値
struct MaterialRecord final
{
    std::array<float, 4> baseColor;
    std::uint32_t textureIndex = 0;
};
static_assert(sizeof(MaterialRecord) == 20 && offsetof(MaterialRecord, textureIndex) == 16);

/// @brief 提出が始まった Copy の宛先と転送元を Command の完了まで維持する
struct MaterialTransfer final
{
    std::shared_ptr<SceneMaterialSet> materials;
    std::vector<std::unique_ptr<DX12GpuResource>> staging;
};

/// @brief 描画時の読み取りへ引き渡せるよう Copy の前後を COMMON で揃える
void transition(ID3D12GraphicsCommandList &a_list, ID3D12Resource &a_resource, D3D12_RESOURCE_STATES a_before,
                D3D12_RESOURCE_STATES a_after)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {&a_resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, a_before, a_after};
    a_list.ResourceBarrier(1, &barrier);
}

/// @brief 2D Texture の Row Pitch を埋めた転送を記録し、SRV を固定 Slot に登録する
Result<void> transfer_texture(ID3D12Device &a_device, ID3D12GraphicsCommandList &a_list, DX12GpuResource &a_staging,
                              const SceneTextureRgba8 &a_data, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT &a_layout,
                              ID3D12Resource &a_destination, D3D12_CPU_DESCRIPTOR_HANDLE a_view)
{
    const std::size_t rowSize = std::size_t(a_data.width) * 4;
    const std::size_t rowPitch = a_layout.Footprint.RowPitch;
    // Footprint が要求する Padding をまとめて初期化し、Map は Texture ごとに一度だけ呼ぶ
    std::vector<std::byte> padded(static_cast<std::size_t>(a_staging.buffer_size()));
    for (std::uint32_t row = 0; row < a_data.height; ++row)
    {
        std::memcpy(padded.data() + row * rowPitch, a_data.pixels.data() + row * rowSize, rowSize);
    }
    auto written = a_staging.write(0, padded);
    if (!written.has_value())
        return written;

    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = a_staging.resource();
    source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source.PlacedFootprint = a_layout;
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = &a_destination;
    destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    // Copy Queue の Texture layout は COMMON のまま暗黙に Copy 書込みへ昇格し、提出完了で復帰する
    a_list.CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);

    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = a_destination.GetDesc().Format;
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Texture2D.MipLevels = 1;
    a_device.CreateShaderResourceView(&a_destination, &view, a_view);
    return Result<void>::success();
}

/// @brief 読取専用の RGBA8 Texture と対応する転送資源を作り、Copy の参照へ追加する
Result<void> upload_texture(ID3D12Device &a_device, ID3D12GraphicsCommandList &a_list, MaterialTransfer &a_transfer,
                            const SceneTextureRgba8 &a_texture, D3D12_CPU_DESCRIPTOR_HANDLE a_view,
                            std::uint64_t &a_allocatedTextureBytes)
{
    using R = Result<void>;
    if (a_texture.width == 0 || a_texture.height == 0 ||
        a_texture.width > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        a_texture.height > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        a_texture.pixels.size() != std::uint64_t(a_texture.width) * a_texture.height * 4)
        return R::failure({ErrorCategory::InvalidArgument, "SceneMaterials.texture_size"});

    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = a_texture.width;
    description.Height = a_texture.height;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.SampleDesc.Count = 1;
    description.Format = a_texture.isSrgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    std::uint64_t size = 0;
    a_device.GetCopyableFootprints(&description, 0, 1, 0, &footprint, nullptr, nullptr, &size);
    if (size == 0 || size > k_textureByteBudget - a_allocatedTextureBytes)
        return R::failure({ErrorCategory::InvalidArgument, "SceneMaterials.texture_budget"});

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    Microsoft::WRL::ComPtr<ID3D12Resource> texture;
    const auto created = a_device.CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
                                                           D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                           IID_PPV_ARGS(&texture));
    if (FAILED(created))
        return R::failure({ErrorCategory::PlatformFailure, "SceneMaterials.create_texture", created});
    texture->SetName(L"CueEngine Base Color Texture");
    auto staging = DX12GpuResource::create_buffer(a_device, {size, GpuMemoryUsage::Upload},
                                                  L"CueEngine Base Color Upload");
    if (!staging.has_value())
        return R::failure(*staging.try_error());
    a_transfer.materials->textures.push_back(std::move(texture));
    a_transfer.staging.push_back(staging.take_value());
    auto uploaded = transfer_texture(a_device, a_list, *a_transfer.staging.back(), a_texture, footprint,
                                     *a_transfer.materials->textures.back().Get(), a_view);
    if (uploaded.has_value())
        a_allocatedTextureBytes += size;
    return uploaded;
}

/// @brief Root SRV が参照する Material の連続レコードを Default Heap へ転送する
Result<void> upload_records(ID3D12Device &a_device, ID3D12GraphicsCommandList &a_list, MaterialTransfer &a_transfer,
                            std::span<const MaterialRecord> a_records)
{
    using R = Result<void>;
    const auto bytes = std::as_bytes(a_records);
    auto destination = DX12GpuResource::create_buffer(a_device, {bytes.size()}, L"CueEngine Material Table");
    if (!destination.has_value())
        return R::failure(*destination.try_error());
    auto staging = DX12GpuResource::create_buffer(a_device, {bytes.size(), GpuMemoryUsage::Upload},
                                                  L"CueEngine Material Table Upload");
    if (!staging.has_value())
        return R::failure(*staging.try_error());
    auto written = (*staging.try_value())->write(0, bytes);
    if (!written.has_value())
        return written;
    a_transfer.materials->records = destination.take_value();
    a_transfer.staging.push_back(staging.take_value());
    auto &target = *a_transfer.materials->records->resource();
    transition(a_list, target, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    a_list.CopyBufferRegion(&target, 0, a_transfer.staging.back()->resource(), 0, bytes.size());
    transition(a_list, target, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    return R::success();
}

/// @brief Tier 1 でも Table 全域を参照可能にし、未登録 Slot を Null Descriptor で埋める
Result<void> initialize_heap(ID3D12Device &a_device, SceneMaterialSet &a_materials)
{
    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    desc.NumDescriptors = k_sceneTextureCount;
    const auto created = a_device.CreateDescriptorHeap(&desc, IID_PPV_ARGS(&a_materials.heap));
    if (FAILED(created))
        return Result<void>::failure({ErrorCategory::PlatformFailure, "SceneMaterials.create_heap", created});
    a_materials.heap->SetName(L"CueEngine Scene Texture Descriptors");
    D3D12_SHADER_RESOURCE_VIEW_DESC emptyView{};
    emptyView.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    emptyView.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    emptyView.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    emptyView.Texture2D.MipLevels = 1;
    const auto stride = a_device.GetDescriptorHandleIncrementSize(desc.Type);
    auto handle = a_materials.heap->GetCPUDescriptorHandleForHeapStart();
    for (std::uint32_t index = 0; index < k_sceneTextureCount; ++index)
    {
        a_device.CreateShaderResourceView(nullptr, &emptyView, handle);
        handle.ptr += stride;
    }
    return Result<void>::success();
}
} // namespace

/// @brief Copy Queue の完了点で Texture と Material の同じ世代をまとめて公開する
Result<std::shared_ptr<const SceneMaterialSet>> create_scene_materials(DX12Backend &a_backend,
                                                                      std::span<const SceneMaterialDesc> a_materials)
{
    using R = Result<std::shared_ptr<const SceneMaterialSet>>;
    if (a_materials.size() > k_maxMaterialAssets)
        return R::failure({ErrorCategory::InvalidArgument, "SceneMaterials.capacity"});
    try
    {
        const auto &resources = *a_backend.get_resource_context();
        auto &device = *resources.device.device();
        auto transfer = std::make_shared<MaterialTransfer>();
        transfer->materials = std::make_shared<SceneMaterialSet>();
        auto heap = initialize_heap(device, *transfer->materials);
        if (!heap.has_value())
            return R::failure(*heap.try_error());
        auto queueResult = a_backend.get_queue_pool()->acquire(QueueType::Copy);
        if (!queueResult.has_value())
            return R::failure(*queueResult.try_error());
        auto queue = queueResult.take_value();
        auto &commands = *a_backend.get_command_pool();
        auto commandResult = commands.acquire(QueueType::Copy);
        if (!commandResult.has_value())
            return R::failure(*commandResult.try_error());
        auto command = commandResult.take_value();
        auto *native = dynamic_cast<DX12GpuCommandContext *>(command.get());
        if (!native)
            return R::failure({ErrorCategory::InvalidState, "SceneMaterials.command"});
        auto retained = native->retain(transfer);
        if (!retained.has_value())
            return R::failure(*retained.try_error());

        const auto stride = device.GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        const auto viewStart = transfer->materials->heap->GetCPUDescriptorHandleForHeapStart();
        std::uint64_t allocatedTextureBytes = 0;
        std::map<const SceneTextureRgba8 *, std::uint32_t> textureSlots;
        std::vector<MaterialRecord> records{{{1, 1, 1, 1}, 0}};
        records.reserve(a_materials.size() + 1);
        for (const auto &material : a_materials)
        {
            if (material.assetId.empty() || !std::ranges::all_of(material.baseColor, [](float a_value)
                { return std::isfinite(a_value) && a_value >= 0.0f && a_value <= 1.0f; }))
                return R::failure({ErrorCategory::InvalidArgument, "SceneMaterials.definition:" + material.assetId});
            if (!transfer->materials->ids.emplace(material.assetId, static_cast<std::uint32_t>(records.size())).second)
                return R::failure({ErrorCategory::InvalidArgument, "SceneMaterials.duplicate_id:" + material.assetId});

            std::uint32_t textureIndex = 0;
            if (material.texture)
            {
                const auto [slot, isNew] = textureSlots.try_emplace(
                    material.texture.get(), static_cast<std::uint32_t>(textureSlots.size()) + 1);
                textureIndex = slot->second;
                if (textureIndex >= k_sceneTextureCount)
                    return R::failure({ErrorCategory::InvalidArgument, "SceneMaterials.texture_capacity"});
                if (isNew)
                {
                    const D3D12_CPU_DESCRIPTOR_HANDLE view{viewStart.ptr + std::size_t(stride) * textureIndex};
                    auto uploaded = upload_texture(device, *native->command_list(), *transfer, *material.texture,
                                                   view, allocatedTextureBytes);
                    if (!uploaded.has_value())
                        return R::failure(*uploaded.try_error());
                }
            }
            records.push_back({material.baseColor, textureIndex});
        }
        auto uploaded = upload_records(device, *native->command_list(), *transfer, records);
        if (!uploaded.has_value())
            return R::failure(*uploaded.try_error());
        auto closed = command->close();
        if (!closed.has_value())
            return R::failure(*closed.try_error());
        auto submitted = commands.submit(*queue, *command);
        if (!submitted.has_value())
            return R::failure(*submitted.try_error());
        auto waited = (*submitted.try_value())->wait();
        if (!waited.has_value())
            return R::failure(*waited.try_error());
        transfer->staging.clear();
        return R::success(std::move(transfer->materials));
    }
    catch (const std::bad_alloc &)
    {
        return R::failure({ErrorCategory::PlatformFailure, "SceneMaterials.create_allocation"});
    }
}
} // namespace cue::dx12
