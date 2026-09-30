#include "DX12ResourcePool.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>
#include <string>
#include <utility>

namespace cue::detail
{
namespace
{
/// @brief Buffer と Texture の空でない共通 Resource Desc を作る
D3D12_RESOURCE_DESC buffer_desc(std::uint64_t a_size, bool a_allowUnorderedAccess = false)
{
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = a_size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = a_allowUnorderedAccess ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
                                       : D3D12_RESOURCE_FLAG_NONE;
    return desc;
}

/// @brief RHI Format を Texture と View で共通の DXGI Format に変換する
DXGI_FORMAT texture_format(GpuTextureFormat a_format) noexcept
{
    switch (a_format)
    {
    case GpuTextureFormat::Rgba8Unorm: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case GpuTextureFormat::Rgba8Srgb: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case GpuTextureFormat::Bc6hUf16: return DXGI_FORMAT_BC6H_UF16;
    case GpuTextureFormat::Bc7Unorm: return DXGI_FORMAT_BC7_UNORM;
    case GpuTextureFormat::Bc7UnormSrgb: return DXGI_FORMAT_BC7_UNORM_SRGB;
    case GpuTextureFormat::R32Uint: return DXGI_FORMAT_R32_UINT;
    case GpuTextureFormat::Depth32Float: return DXGI_FORMAT_D32_FLOAT;
    case GpuTextureFormat::Depth24Stencil8: return DXGI_FORMAT_D24_UNORM_S8_UINT;
    case GpuTextureFormat::R24UnormX8Typeless: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

bool is_depth(GpuTextureFormat a_format) noexcept
{
    return a_format == GpuTextureFormat::Depth32Float || a_format == GpuTextureFormat::Depth24Stencil8;
}

bool is_block_compressed(GpuTextureFormat a_format) noexcept
{
    return a_format == GpuTextureFormat::Bc6hUf16 || a_format == GpuTextureFormat::Bc7Unorm ||
           a_format == GpuTextureFormat::Bc7UnormSrgb;
}
} // namespace

/// @brief Pool 終了時に永続 Map を解除してから Resource を解放する
DX12ResourcePool::~DX12ResourcePool()
{
    for (auto& record : m_resources)
    {
        unmap_slices(record);
    }
}

/// @brief 単独利用時も同じ Allocator 契約で三種類の Heap を所有する
Result<std::unique_ptr<DX12ResourcePool>> DX12ResourcePool::create(DX12RenderDevice& a_device,
                                                                       DX12QueuePool& a_queues,
                                                                       UINT a_viewCapacity)
{
    using PoolResult = Result<std::unique_ptr<DX12ResourcePool>>;
    if (a_viewCapacity == 0)
    {
        return PoolResult::failure({ErrorCategory::InvalidArgument, "DX12ResourcePool.create.capacity"});
    }
    auto allocatorResult = DescriptorAllocator::create(a_device, a_viewCapacity, a_viewCapacity, a_viewCapacity);
    if (!allocatorResult.has_value())
    {
        return PoolResult::failure(*allocatorResult.try_error());
    }
    auto pool = std::make_unique<DX12ResourcePool>();
    pool->m_device = a_device.device();
    pool->m_queues = &a_queues;
    pool->m_ownedAllocator = allocatorResult.take_value();
    pool->m_allocator = pool->m_ownedAllocator.get();
    for (std::size_t kind = 0; kind < pool->m_views.size(); ++kind)
    {
        pool->m_views[kind].resize(a_viewCapacity);
    }
    return PoolResult::success(std::move(pool));
}

/// @brief Backend 共通の Descriptor Allocator を使い、Heap の重複所有を避ける
Result<std::unique_ptr<DX12ResourcePool>> DX12ResourcePool::create(DX12RenderDevice& a_device,
                                                                       DX12QueuePool& a_queues,
                                                                       DescriptorAllocator& a_allocator)
{
    using PoolResult = Result<std::unique_ptr<DX12ResourcePool>>;
    auto pool = std::make_unique<DX12ResourcePool>();
    pool->m_device = a_device.device();
    pool->m_queues = &a_queues;
    pool->m_allocator = &a_allocator;
    for (std::size_t kind = 0; kind < pool->m_views.size(); ++kind)
    {
        pool->m_views[kind].resize(a_allocator.capacity(static_cast<DescriptorHeapKind>(kind)));
    }
    return PoolResult::success(std::move(pool));
}

/// @brief Memory 種別に合わせた Heap と初期状態で Buffer を生成する
Result<GpuResourceHandle> DX12ResourcePool::create_buffer(GpuBufferDesc a_desc)
{
    const std::array counts{a_desc.defaultHeapCount, a_desc.uploadHeapCount, a_desc.readbackHeapCount};
    const bool explicitSlices = counts[0] != 0 || counts[1] != 0 || counts[2] != 0;
    if (a_desc.size == 0 || (a_desc.memory != GpuMemory::Device && a_desc.memory != GpuMemory::Upload &&
                             a_desc.memory != GpuMemory::Readback) ||
        (a_desc.allowUnorderedAccess && !explicitSlices && a_desc.memory != GpuMemory::Device) ||
        (a_desc.alignment != 0 && (a_desc.stride == 0 || a_desc.elementCount == 0)) ||
        (a_desc.stride != 0 && a_desc.elementCount != 0 &&
         (a_desc.alignment > std::numeric_limits<std::uint64_t>::max() - a_desc.stride ||
          (static_cast<std::uint64_t>(a_desc.stride) + a_desc.alignment - 1) /
                  (a_desc.alignment == 0 ? 1 : a_desc.alignment) *
                  (a_desc.alignment == 0 ? 1 : a_desc.alignment) >
              a_desc.size / a_desc.elementCount)))
    {
        return Result<GpuResourceHandle>::failure({ErrorCategory::InvalidArgument,
                                                    "DX12ResourcePool.create_buffer"});
    }
    std::array<std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>, 3> slices;
    const auto primaryIndex = static_cast<std::size_t>(a_desc.memory);
    for (std::size_t kind = 0; kind < slices.size(); ++kind)
    {
        const auto count = explicitSlices ? counts[kind] : (kind == primaryIndex ? 1u : 0u);
        const auto memory = static_cast<GpuMemory>(kind);
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = memory == GpuMemory::Device ? D3D12_HEAP_TYPE_DEFAULT
                    : memory == GpuMemory::Upload ? D3D12_HEAP_TYPE_UPLOAD : D3D12_HEAP_TYPE_READBACK;
        const auto state = memory == GpuMemory::Upload ? D3D12_RESOURCE_STATE_GENERIC_READ
                           : memory == GpuMemory::Readback ? D3D12_RESOURCE_STATE_COPY_DEST
                                                            : D3D12_RESOURCE_STATE_COMMON;
        const auto desc = buffer_desc(a_desc.size, a_desc.allowUnorderedAccess && memory == GpuMemory::Device);
        for (std::uint32_t index = 0; index < count; ++index)
        {
            Microsoft::WRL::ComPtr<ID3D12Resource> resource;
            const HRESULT result = m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                                      state, nullptr, IID_PPV_ARGS(&resource));
            if (FAILED(result))
            {
                return Result<GpuResourceHandle>::failure(
                    gpu_error("ID3D12Device.CreateCommittedResource.Buffer", result));
            }
            const HRESULT nameResult = resource->SetName(L"CueEngine GPU Buffer");
            if (FAILED(nameResult))
            {
                return Result<GpuResourceHandle>::failure(gpu_error("ID3D12Resource.SetName.Buffer", nameResult));
            }
            slices[kind].push_back(std::move(resource));
        }
    }
    std::size_t selected = primaryIndex;
    if (slices[selected].empty())
    {
        selected = !slices[0].empty() ? 0 : !slices[1].empty() ? 1 : 2;
    }
    auto handle = store(slices[selected][0], false, GpuTextureFormat::Rgba8Unorm,
                        static_cast<GpuMemory>(selected), a_desc.allowUnorderedAccess);
    auto& record = m_resources[handle.index];
    record.bufferSlices = std::move(slices);
    record.alignment = a_desc.alignment == 0 ? 1 : a_desc.alignment;
    record.stride = a_desc.stride;
    record.elementCount = a_desc.elementCount;
    return Result<GpuResourceHandle>::success(handle);
}

/// @brief Texture 種別、Mip、Array と Buffer 数に合わせて物理資源を生成する
Result<GpuResourceHandle> DX12ResourcePool::create_texture(GpuTextureDesc a_desc)
{
    const bool isDepth = is_depth(a_desc.format);
    const bool compressed = is_block_compressed(a_desc.format);
    const auto kind = a_desc.kind == GpuTextureKind::Automatic
                          ? (isDepth ? GpuTextureKind::DepthStencil
                                     : compressed ? GpuTextureKind::Default : GpuTextureKind::RenderTarget)
                          : a_desc.kind;
    if (a_desc.width == 0 || a_desc.height == 0 || a_desc.bufferCount == 0 ||
        a_desc.mipLevels == 0 || a_desc.arraySize == 0 || a_desc.sampleCount == 0 ||
        texture_format(a_desc.format) == DXGI_FORMAT_UNKNOWN ||
        a_desc.format == GpuTextureFormat::R24UnormX8Typeless ||
        (isDepth != (kind == GpuTextureKind::DepthStencil)) ||
        (compressed && (kind != GpuTextureKind::Default || a_desc.allowUnorderedAccess)) ||
        (a_desc.allowUnorderedAccess && (isDepth || a_desc.sampleCount != 1 ||
                                          a_desc.format == GpuTextureFormat::Rgba8Srgb ||
                                          a_desc.format == GpuTextureFormat::Bc7UnormSrgb)) ||
        (a_desc.type == GpuTextureType::CubeMap && (a_desc.arraySize != 6 || a_desc.width != a_desc.height ||
                                                    a_desc.sampleCount != 1)) ||
        (a_desc.type == GpuTextureType::Texture3D &&
         (kind == GpuTextureKind::DepthStencil || a_desc.sampleCount != 1)) ||
        (a_desc.sampleCount != 1 && a_desc.mipLevels != 1))
    {
        return Result<GpuResourceHandle>::failure({ErrorCategory::InvalidArgument,
                                                    "DX12ResourcePool.create_texture"});
    }
    if (a_desc.type != GpuTextureType::Texture2D && a_desc.type != GpuTextureType::Texture3D &&
        a_desc.type != GpuTextureType::CubeMap)
    {
        return Result<GpuResourceHandle>::failure({ErrorCategory::InvalidArgument,
                                                    "DX12ResourcePool.create_texture.type"});
    }
    a_desc.kind = kind;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = a_desc.type == GpuTextureType::Texture3D ? D3D12_RESOURCE_DIMENSION_TEXTURE3D
                                                               : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = a_desc.width;
    desc.Height = a_desc.height;
    desc.DepthOrArraySize = a_desc.arraySize;
    desc.MipLevels = a_desc.mipLevels;
    // Depth は DSV と SRV の両方を作れるよう物理 Format を Typeless にする
    desc.Format = a_desc.format == GpuTextureFormat::Depth32Float ? DXGI_FORMAT_R32_TYPELESS
                : a_desc.format == GpuTextureFormat::Depth24Stencil8 ? DXGI_FORMAT_R24G8_TYPELESS
                                                                      : texture_format(a_desc.format);
    desc.SampleDesc.Count = a_desc.sampleCount;
    desc.Flags = kind == GpuTextureKind::DepthStencil ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL
                 : kind == GpuTextureKind::RenderTarget ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET
                                                        : D3D12_RESOURCE_FLAG_NONE;
    if (a_desc.allowUnorderedAccess)
    {
        desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    }
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_CLEAR_VALUE clear{};
    clear.Format = texture_format(a_desc.format);
    if (kind == GpuTextureKind::DepthStencil)
    {
        clear.DepthStencil.Depth = a_desc.clearDepth;
        clear.DepthStencil.Stencil = a_desc.clearStencil;
    }
    else
    {
        for (std::size_t index = 0; index < 4; ++index)
        {
            clear.Color[index] = a_desc.clearColor[index];
        }
    }
    const D3D12_CLEAR_VALUE* clearValue = kind == GpuTextureKind::Default ? nullptr : &clear;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> slices;
    slices.reserve(a_desc.bufferCount);
    for (std::uint32_t index = 0; index < a_desc.bufferCount; ++index)
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        const HRESULT result = m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COMMON, clearValue, IID_PPV_ARGS(&resource));
        if (FAILED(result))
        {
            return Result<GpuResourceHandle>::failure(
                gpu_error("ID3D12Device.CreateCommittedResource.Texture", result));
        }
        const HRESULT nameResult = resource->SetName(isDepth ? L"CueEngine GPU Depth Texture"
                                                           : L"CueEngine GPU Color Texture");
        if (FAILED(nameResult))
        {
            return Result<GpuResourceHandle>::failure(gpu_error("ID3D12Resource.SetName.Texture", nameResult));
        }
        slices.push_back(std::move(resource));
    }
    auto handle = store(slices[0], true, a_desc.format, GpuMemory::Device, a_desc.allowUnorderedAccess);
    auto& record = m_resources[handle.index];
    record.textureSlices = std::move(slices);
    record.textureDesc = std::move(a_desc);
    return Result<GpuResourceHandle>::success(handle);
}

/// @brief 新規 Texture の Subresource を Upload Buffer から Graphics Queue で転送する
Result<GpuResourceHandle> DX12ResourcePool::create_texture(
    GpuTextureDesc a_desc, std::span<const GpuTextureSubresourceData> a_initialData)
{
    using TextureResult = Result<GpuResourceHandle>;
    const std::uint32_t subresourceCount = a_desc.mipLevels *
        (a_desc.type == GpuTextureType::Texture3D ? 1u : a_desc.arraySize);
    if (a_initialData.empty() || a_initialData.size() != subresourceCount || a_desc.sampleCount != 1)
    {
        return TextureResult::failure({ErrorCategory::InvalidArgument,
                                       "DX12ResourcePool.create_texture.initialData"});
    }
    auto textureResult = create_texture(std::move(a_desc));
    if (!textureResult.has_value())
    {
        return textureResult;
    }
    const auto handle = textureResult.take_value();
    auto& record = m_resources[handle.index];
    const auto resourceDesc = record.resource->GetDesc();
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts(subresourceCount);
    std::vector<UINT> rowCounts(subresourceCount);
    std::vector<UINT64> rowSizes(subresourceCount);
    UINT64 uploadSize = 0;
    m_device->GetCopyableFootprints(&resourceDesc, 0, subresourceCount, 0,
                                    layouts.data(), rowCounts.data(), rowSizes.data(), &uploadSize);
    for (std::uint32_t index = 0; index < subresourceCount; ++index)
    {
        const auto& source = a_initialData[index];
        const auto depth = layouts[index].Footprint.Depth;
        if (!source.data || source.rowPitch < rowSizes[index] ||
            source.slicePitch < static_cast<std::uint64_t>(source.rowPitch) * rowCounts[index] ||
            source.dataSize < static_cast<std::uint64_t>(source.slicePitch) * depth)
        {
            [[maybe_unused]] auto cleanup = destroy(handle);
            return TextureResult::failure({ErrorCategory::InvalidArgument,
                                           "DX12ResourcePool.create_texture.initialPitch"});
        }
    }
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    const auto stagingDesc = buffer_desc(uploadSize);
    Microsoft::WRL::ComPtr<ID3D12Resource> staging;
    HRESULT result = m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &stagingDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&staging));
    if (FAILED(result))
    {
        [[maybe_unused]] auto cleanup = destroy(handle);
        return TextureResult::failure(gpu_error("ID3D12Device.CreateCommittedResource.TextureUpload", result));
    }
    D3D12_RANGE readRange{0, 0};
    void* mapped = nullptr;
    result = staging->Map(0, &readRange, &mapped);
    if (FAILED(result))
    {
        [[maybe_unused]] auto cleanup = destroy(handle);
        return TextureResult::failure(gpu_error("ID3D12Resource.Map.TextureUpload", result));
    }
    for (std::uint32_t index = 0; index < subresourceCount; ++index)
    {
        const auto& source = a_initialData[index];
        const auto& footprint = layouts[index].Footprint;
        auto* target = static_cast<std::byte*>(mapped) + layouts[index].Offset;
        for (UINT depth = 0; depth < footprint.Depth; ++depth)
        {
            for (UINT row = 0; row < rowCounts[index]; ++row)
            {
                std::memcpy(target + static_cast<std::size_t>(depth) * footprint.RowPitch * rowCounts[index] +
                                static_cast<std::size_t>(row) * footprint.RowPitch,
                            source.data + static_cast<std::size_t>(depth) * source.slicePitch +
                                static_cast<std::size_t>(row) * source.rowPitch,
                            static_cast<std::size_t>(rowSizes[index]));
            }
        }
    }
    staging->Unmap(0, nullptr);
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    result = m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
    if (FAILED(result))
    {
        [[maybe_unused]] auto cleanup = destroy(handle);
        return TextureResult::failure(gpu_error("ID3D12Device.CreateCommandAllocator.TextureUpload", result));
    }
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    result = m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                         IID_PPV_ARGS(&list));
    if (FAILED(result))
    {
        [[maybe_unused]] auto cleanup = destroy(handle);
        return TextureResult::failure(gpu_error("ID3D12Device.CreateCommandList.TextureUpload", result));
    }
    for (const auto& texture : record.textureSlices)
    {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = texture.Get();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        list->ResourceBarrier(1, &barrier);
        for (std::uint32_t index = 0; index < subresourceCount; ++index)
        {
            D3D12_TEXTURE_COPY_LOCATION target{};
            target.pResource = texture.Get();
            target.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            target.SubresourceIndex = index;
            D3D12_TEXTURE_COPY_LOCATION source{};
            source.pResource = staging.Get();
            source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            source.PlacedFootprint = layouts[index];
            list->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
        }
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
        list->ResourceBarrier(1, &barrier);
    }
    result = list->Close();
    if (FAILED(result))
    {
        [[maybe_unused]] auto cleanup = destroy(handle);
        return TextureResult::failure(gpu_error("ID3D12GraphicsCommandList.Close.TextureUpload", result));
    }
    ID3D12CommandList* submitted[] = {list.Get()};
    auto& queue = graphics_queue();
    queue.queue()->ExecuteCommandLists(1, submitted);
    auto fence = queue.signal();
    if (!fence.has_value())
    {
        [[maybe_unused]] auto cleanup = destroy(handle);
        return TextureResult::failure(*fence.try_error());
    }
    auto waited = queue.wait_for(fence.take_value());
    if (!waited.has_value())
    {
        [[maybe_unused]] auto cleanup = destroy(handle);
        return TextureResult::failure(*waited.try_error());
    }
    return TextureResult::success(handle);
}

/// @brief Upload Buffer の境界を検証して CPU Data を書く
Result<void> DX12ResourcePool::write_buffer(GpuResourceHandle a_buffer, std::uint64_t a_offset,
                                             const void* a_data, std::uint64_t a_size)
{
    if (!owns(a_buffer) || !a_data || a_size == 0)
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "DX12ResourcePool.write_buffer"});
    }
    auto& record = m_resources[a_buffer.index];
    auto& slices = record.bufferSlices[static_cast<std::size_t>(GpuMemory::Upload)];
    if (record.isTexture || slices.empty())
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "DX12ResourcePool.write_buffer.range"});
    }
    const auto width = slices[0]->GetDesc().Width;
    if (a_offset > width || a_size > width - a_offset)
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "DX12ResourcePool.write_buffer.range"});
    }
    D3D12_RANGE readRange{0, 0};
    void* mapped = nullptr;
    const auto& mappedSlices = record.mappedSlices[static_cast<std::size_t>(GpuMemory::Upload)];
    if (!mappedSlices.empty())
    {
        std::memcpy(mappedSlices[0] + a_offset, a_data, static_cast<std::size_t>(a_size));
        return Result<void>::success();
    }
    const HRESULT result = slices[0]->Map(0, &readRange, &mapped);
    if (FAILED(result))
    {
        return Result<void>::failure(gpu_error("ID3D12Resource.Map.Upload", result));
    }
    std::memcpy(static_cast<std::byte*>(mapped) + a_offset, a_data, static_cast<std::size_t>(a_size));
    D3D12_RANGE writtenRange{static_cast<SIZE_T>(a_offset), static_cast<SIZE_T>(a_offset + a_size)};
    slices[0]->Unmap(0, &writtenRange);
    return Result<void>::success();
}

/// @brief Readback Buffer の GPU 完了後だけ CPU Data を読む
Result<void> DX12ResourcePool::read_buffer(GpuResourceHandle a_buffer, std::uint64_t a_offset,
                                            void* a_data, std::uint64_t a_size)
{
    if (!owns(a_buffer) || !a_data || a_size == 0)
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "DX12ResourcePool.read_buffer"});
    }
    auto& record = m_resources[a_buffer.index];
    auto& slices = record.bufferSlices[static_cast<std::size_t>(GpuMemory::Readback)];
    if (record.isTexture || slices.empty())
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "DX12ResourcePool.read_buffer.range"});
    }
    const auto width = slices[0]->GetDesc().Width;
    if (a_offset > width || a_size > width - a_offset)
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "DX12ResourcePool.read_buffer.range"});
    }
    auto idle = m_queues->wait_idle();
    if (!idle.has_value())
    {
        return idle;
    }
    const auto& mappedSlices = record.mappedSlices[static_cast<std::size_t>(GpuMemory::Readback)];
    if (!mappedSlices.empty())
    {
        std::memcpy(a_data, mappedSlices[0] + a_offset, static_cast<std::size_t>(a_size));
        return Result<void>::success();
    }
    D3D12_RANGE readRange{static_cast<SIZE_T>(a_offset), static_cast<SIZE_T>(a_offset + a_size)};
    void* mapped = nullptr;
    const HRESULT result = slices[0]->Map(0, &readRange, &mapped);
    if (FAILED(result))
    {
        return Result<void>::failure(gpu_error("ID3D12Resource.Map.Readback", result));
    }
    std::memcpy(a_data, static_cast<const std::byte*>(mapped) + a_offset, static_cast<std::size_t>(a_size));
    D3D12_RANGE writtenRange{0, 0};
    slices[0]->Unmap(0, &writtenRange);
    return Result<void>::success();
}

/// @brief Resource 種別と範囲を検証して、適合する Heap の空き Slot に Descriptor を作る
Result<GpuViewHandle> DX12ResourcePool::create_view(GpuResourceHandle a_resource, GpuViewDesc a_desc)
{
    if (!owns(a_resource) || !is_view_kind(a_desc.kind))
    {
        return Result<GpuViewHandle>::failure({ErrorCategory::InvalidArgument, "DX12ResourcePool.create_view"});
    }
    const auto& record = m_resources[a_resource.index];
    ID3D12Resource* viewResource = record.resource.Get();
    GpuMemory viewMemory = record.memory;
    if (record.isTexture)
    {
        if (a_desc.resourceIndex >= record.textureSlices.size() || a_desc.memory != GpuMemory::Device)
        {
            return Result<GpuViewHandle>::failure({ErrorCategory::InvalidArgument,
                                                    "DX12ResourcePool.create_view.textureSlice"});
        }
        viewResource = record.textureSlices[a_desc.resourceIndex].Get();
    }
    else
    {
        const auto memoryIndex = static_cast<std::size_t>(a_desc.memory);
        if (memoryIndex >= record.bufferSlices.size())
        {
            return Result<GpuViewHandle>::failure({ErrorCategory::InvalidArgument,
                                                    "DX12ResourcePool.create_view.memory"});
        }
        const auto& slices = record.bufferSlices[memoryIndex];
        if (a_desc.resourceIndex < slices.size())
        {
            viewResource = slices[a_desc.resourceIndex].Get();
            viewMemory = a_desc.memory;
        }
        else if (a_desc.resourceIndex != 0 || a_desc.memory != GpuMemory::Device)
        {
            return Result<GpuViewHandle>::failure({ErrorCategory::InvalidArgument,
                                                    "DX12ResourcePool.create_view.slice"});
        }
    }
    const bool isDepth = record.isTexture && is_depth(record.format);
    const bool isBufferView = a_desc.kind == GpuViewKind::ConstantBuffer ||
                              (a_desc.kind == GpuViewKind::ShaderResource && !record.isTexture) ||
                              (a_desc.kind == GpuViewKind::UnorderedAccess && !record.isTexture);
    if (record.isTexture)
    {
        const bool depthSrvOverride = record.format == GpuTextureFormat::Depth24Stencil8 &&
            a_desc.kind == GpuViewKind::ShaderResource &&
            a_desc.format == GpuTextureFormat::R24UnormX8Typeless;
        if (a_desc.mipSlice >= record.textureDesc.mipLevels ||
            (a_desc.mipLevels != 0 && a_desc.mipLevels > record.textureDesc.mipLevels - a_desc.mipSlice) ||
            (a_desc.kind != GpuViewKind::ShaderResource && a_desc.mipLevels > 1) ||
            (a_desc.format && *a_desc.format != record.format && !depthSrvOverride) ||
            (record.textureDesc.sampleCount > 1 && (a_desc.mipSlice != 0 || a_desc.mipLevels > 1)))
        {
            return Result<GpuViewHandle>::failure({ErrorCategory::InvalidArgument,
                                                    "DX12ResourcePool.create_view.mipFormat"});
        }
    }
    else if (a_desc.numElements != 0)
    {
        if (a_desc.bufferSize != 0 || a_desc.bufferOffset != 0)
        {
            return Result<GpuViewHandle>::failure({ErrorCategory::InvalidArgument,
                                                    "DX12ResourcePool.create_view.elementRange"});
        }
        const std::uint64_t elementSize = a_desc.structureStride == 0 ? sizeof(std::uint32_t)
                                                                       : a_desc.structureStride;
        a_desc.bufferOffset = static_cast<std::uint64_t>(a_desc.firstElement) * elementSize;
        a_desc.bufferSize = static_cast<std::uint64_t>(a_desc.numElements) * elementSize;
    }
    if ((record.isTexture && (a_desc.bufferOffset != 0 || a_desc.bufferSize != 0 ||
                              a_desc.structureStride != 0 || a_desc.numElements != 0 ||
                              a_desc.firstElement != 0 || a_desc.kind == GpuViewKind::ConstantBuffer)) ||
        (!record.isTexture && (!isBufferView || viewMemory == GpuMemory::Readback)) ||
        (isDepth && a_desc.kind != GpuViewKind::DepthStencil && a_desc.kind != GpuViewKind::ShaderResource) ||
        (!isDepth && a_desc.kind == GpuViewKind::DepthStencil) ||
        (record.isTexture && a_desc.kind == GpuViewKind::RenderTarget &&
         record.textureDesc.kind != GpuTextureKind::RenderTarget) ||
        (record.isTexture && a_desc.kind == GpuViewKind::UnorderedAccess &&
         record.textureDesc.sampleCount != 1) ||
        (a_desc.kind == GpuViewKind::UnorderedAccess &&
         (!record.allowUnorderedAccess || viewMemory != GpuMemory::Device)))
    {
        return Result<GpuViewHandle>::failure({ErrorCategory::InvalidArgument,
                                                "DX12ResourcePool.create_view.format"});
    }
    const auto size = viewResource->GetDesc().Width;
    if (isBufferView && (a_desc.bufferSize == 0 || a_desc.bufferOffset > size ||
                         a_desc.bufferSize > size - a_desc.bufferOffset))
    {
        return Result<GpuViewHandle>::failure({ErrorCategory::InvalidArgument,
                                                "DX12ResourcePool.create_view.range"});
    }
    if (a_desc.kind == GpuViewKind::ConstantBuffer &&
        (a_desc.bufferOffset % D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT != 0 ||
         a_desc.bufferSize % D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT != 0 ||
         a_desc.bufferSize > std::numeric_limits<UINT>::max()))
    {
        return Result<GpuViewHandle>::failure({ErrorCategory::InvalidArgument,
                                                "DX12ResourcePool.create_view.cbvAlignment"});
    }
    if (isBufferView && a_desc.kind != GpuViewKind::ConstantBuffer)
    {
        const std::uint32_t stride = a_desc.structureStride == 0 ? sizeof(std::uint32_t)
                                                                   : a_desc.structureStride;
        if (a_desc.bufferOffset % stride != 0 || a_desc.bufferSize % stride != 0 ||
            a_desc.bufferOffset / stride > std::numeric_limits<UINT>::max() ||
            a_desc.bufferSize / stride > std::numeric_limits<UINT>::max() ||
            (a_desc.structureStride != 0 && a_desc.structureStride % sizeof(std::uint32_t) != 0))
        {
            return Result<GpuViewHandle>::failure({ErrorCategory::InvalidArgument,
                                                    "DX12ResourcePool.create_view.elements"});
        }
    }
    auto slotResult = record.isTexture && a_desc.kind == GpuViewKind::ShaderResource
        ? m_allocator->allocate_texture() : m_allocator->allocate(heap_kind(a_desc.kind));
    if (!slotResult.has_value())
    {
        return Result<GpuViewHandle>::failure(*slotResult.try_error());
    }
    const DescriptorSlot slot = slotResult.take_value();
    auto handleResult = m_allocator->cpu_handle(slot);
    if (!handleResult.has_value())
    {
        [[maybe_unused]] auto releaseResult = m_allocator->release(slot);
        return Result<GpuViewHandle>::failure(*handleResult.try_error());
    }
    const auto handle = handleResult.take_value();
    switch (a_desc.kind)
    {
        case GpuViewKind::RenderTarget:
        {
            D3D12_RENDER_TARGET_VIEW_DESC desc{};
            desc.Format = texture_format(record.format);
            const auto& texture = record.textureDesc;
            if (texture.type == GpuTextureType::Texture3D)
            {
                desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE3D;
                desc.Texture3D.MipSlice = a_desc.mipSlice;
                desc.Texture3D.WSize = std::max(1u, static_cast<unsigned>(texture.arraySize) >> a_desc.mipSlice);
            }
            else if (texture.arraySize > 1)
            {
                desc.ViewDimension = texture.sampleCount > 1 ? D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY
                                                            : D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
                if (texture.sampleCount > 1)
                {
                    desc.Texture2DMSArray.ArraySize = texture.arraySize;
                }
                else
                {
                    desc.Texture2DArray.ArraySize = texture.arraySize;
                    desc.Texture2DArray.MipSlice = a_desc.mipSlice;
                }
            }
            else
            {
                desc.ViewDimension = texture.sampleCount > 1 ? D3D12_RTV_DIMENSION_TEXTURE2DMS
                                                            : D3D12_RTV_DIMENSION_TEXTURE2D;
                if (texture.sampleCount == 1)
                {
                    desc.Texture2D.MipSlice = a_desc.mipSlice;
                }
            }
            m_device->CreateRenderTargetView(viewResource, &desc, handle);
            break;
        }
        case GpuViewKind::DepthStencil:
        {
            D3D12_DEPTH_STENCIL_VIEW_DESC desc{};
            const auto& texture = record.textureDesc;
            desc.Format = texture_format(record.format);
            if (texture.arraySize > 1)
            {
                desc.ViewDimension = texture.sampleCount > 1 ? D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY
                                                            : D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
                if (texture.sampleCount > 1)
                {
                    desc.Texture2DMSArray.ArraySize = texture.arraySize;
                }
                else
                {
                    desc.Texture2DArray.ArraySize = texture.arraySize;
                    desc.Texture2DArray.MipSlice = a_desc.mipSlice;
                }
            }
            else
            {
                desc.ViewDimension = texture.sampleCount > 1 ? D3D12_DSV_DIMENSION_TEXTURE2DMS
                                                            : D3D12_DSV_DIMENSION_TEXTURE2D;
                if (texture.sampleCount == 1)
                {
                    desc.Texture2D.MipSlice = a_desc.mipSlice;
                }
            }
            m_device->CreateDepthStencilView(viewResource, &desc, handle);
            break;
        }
        case GpuViewKind::ShaderResource:
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
            desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            if (record.isTexture)
            {
                const auto& texture = record.textureDesc;
                desc.Format = record.format == GpuTextureFormat::Depth32Float ? DXGI_FORMAT_R32_FLOAT
                            : record.format == GpuTextureFormat::Depth24Stencil8
                                  ? DXGI_FORMAT_R24_UNORM_X8_TYPELESS : texture_format(record.format);
                if (texture.type == GpuTextureType::Texture3D)
                {
                    desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
                    desc.Texture3D.MostDetailedMip = a_desc.mipSlice;
                    desc.Texture3D.MipLevels = a_desc.mipLevels == 0
                        ? texture.mipLevels - a_desc.mipSlice : a_desc.mipLevels;
                }
                else if (texture.type == GpuTextureType::CubeMap)
                {
                    desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
                    desc.TextureCube.MostDetailedMip = a_desc.mipSlice;
                    desc.TextureCube.MipLevels = a_desc.mipLevels == 0
                        ? texture.mipLevels - a_desc.mipSlice : a_desc.mipLevels;
                }
                else if (texture.arraySize > 1)
                {
                    desc.ViewDimension = texture.sampleCount > 1 ? D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY
                                                                : D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
                    if (texture.sampleCount > 1)
                    {
                        desc.Texture2DMSArray.ArraySize = texture.arraySize;
                    }
                    else
                    {
                        desc.Texture2DArray.ArraySize = texture.arraySize;
                        desc.Texture2DArray.MostDetailedMip = a_desc.mipSlice;
                        desc.Texture2DArray.MipLevels = a_desc.mipLevels == 0
                            ? texture.mipLevels - a_desc.mipSlice : a_desc.mipLevels;
                    }
                }
                else
                {
                    desc.ViewDimension = texture.sampleCount > 1 ? D3D12_SRV_DIMENSION_TEXTURE2DMS
                                                                : D3D12_SRV_DIMENSION_TEXTURE2D;
                    if (texture.sampleCount == 1)
                    {
                        desc.Texture2D.MostDetailedMip = a_desc.mipSlice;
                        desc.Texture2D.MipLevels = a_desc.mipLevels == 0
                            ? texture.mipLevels - a_desc.mipSlice : a_desc.mipLevels;
                    }
                }
            }
            else
            {
                desc.Format = a_desc.structureStride == 0 ? DXGI_FORMAT_R32_TYPELESS : DXGI_FORMAT_UNKNOWN;
                desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
                const auto stride = a_desc.structureStride == 0 ? sizeof(std::uint32_t)
                                                                : a_desc.structureStride;
                desc.Buffer.FirstElement = a_desc.bufferOffset / stride;
                desc.Buffer.NumElements = static_cast<UINT>(a_desc.bufferSize / stride);
                desc.Buffer.StructureByteStride = a_desc.structureStride;
                desc.Buffer.Flags = a_desc.structureStride == 0 ? D3D12_BUFFER_SRV_FLAG_RAW
                                                                : D3D12_BUFFER_SRV_FLAG_NONE;
            }
            m_device->CreateShaderResourceView(viewResource, &desc, handle);
            break;
        }
        case GpuViewKind::ConstantBuffer:
        {
            D3D12_CONSTANT_BUFFER_VIEW_DESC desc{};
            desc.BufferLocation = viewResource->GetGPUVirtualAddress() + a_desc.bufferOffset;
            desc.SizeInBytes = static_cast<UINT>(a_desc.bufferSize);
            m_device->CreateConstantBufferView(&desc, handle);
            break;
        }
        case GpuViewKind::UnorderedAccess:
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
            if (record.isTexture)
            {
                const auto& texture = record.textureDesc;
                desc.Format = texture_format(record.format);
                if (texture.type == GpuTextureType::Texture3D)
                {
                    desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
                    desc.Texture3D.MipSlice = a_desc.mipSlice;
                    desc.Texture3D.WSize = std::max(1u, static_cast<unsigned>(texture.arraySize) >> a_desc.mipSlice);
                }
                else if (texture.arraySize > 1)
                {
                    desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
                    desc.Texture2DArray.ArraySize = texture.arraySize;
                    desc.Texture2DArray.MipSlice = a_desc.mipSlice;
                }
                else
                {
                    desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                    desc.Texture2D.MipSlice = a_desc.mipSlice;
                }
            }
            else
            {
                desc.Format = a_desc.structureStride == 0 ? DXGI_FORMAT_R32_TYPELESS : DXGI_FORMAT_UNKNOWN;
                desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
                const auto stride = a_desc.structureStride == 0 ? sizeof(std::uint32_t)
                                                                : a_desc.structureStride;
                desc.Buffer.FirstElement = a_desc.bufferOffset / stride;
                desc.Buffer.NumElements = static_cast<UINT>(a_desc.bufferSize / stride);
                desc.Buffer.StructureByteStride = a_desc.structureStride;
                desc.Buffer.Flags = a_desc.structureStride == 0 ? D3D12_BUFFER_UAV_FLAG_RAW
                                                                : D3D12_BUFFER_UAV_FLAG_NONE;
            }
            m_device->CreateUnorderedAccessView(viewResource, nullptr, &desc, handle);
            break;
        }
    }
    if (heap_index(a_desc.kind) == 2)
    {
        auto syncResult = m_allocator->sync_shader_descriptor(slot);
        if (!syncResult.has_value())
        {
            [[maybe_unused]] auto releaseResult = m_allocator->release(slot);
            return Result<GpuViewHandle>::failure(*syncResult.try_error());
        }
    }
    auto& view = m_views[heap_index(a_desc.kind)][slot.index];
    view.isAllocated = true;
    view.resource = a_resource;
    view.physicalResource = viewResource;
    view.kind = a_desc.kind;
    view.generation = slot.generation;
    return Result<GpuViewHandle>::success({a_desc.kind, slot.index, slot.generation, this});
}

/// @brief Queue 完了後に View Slot を無効化する
Result<void> DX12ResourcePool::destroy_view(GpuViewHandle a_view)
{
    if (!owns(a_view))
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "DX12ResourcePool.destroy_view"});
    }
    auto idle = m_queues->wait_idle();
    if (!idle.has_value())
    {
        return idle;
    }
    auto releaseResult = m_allocator->release({heap_kind(a_view.kind), a_view.index, a_view.generation,
                                               m_allocator->owner_id()});
    if (!releaseResult.has_value())
    {
        return releaseResult;
    }
    m_views[heap_index(a_view.kind)][a_view.index].isAllocated = false;
    m_views[heap_index(a_view.kind)][a_view.index].physicalResource = nullptr;
    return Result<void>::success();
}

/// @brief View が残っていない資源だけを Queue 完了後に解放する
Result<void> DX12ResourcePool::destroy(GpuResourceHandle a_resource)
{
    if (!owns(a_resource))
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "DX12ResourcePool.destroy"});
    }
    for (const auto& views : m_views)
    {
        for (const auto& view : views)
        {
            if (view.isAllocated && view.resource.index == a_resource.index &&
                view.resource.generation == a_resource.generation)
            {
                return Result<void>::failure({ErrorCategory::InvalidState,
                                              "DX12ResourcePool.destroy.activeView"});
            }
        }
    }
    auto idle = m_queues->wait_idle();
    if (!idle.has_value())
    {
        return idle;
    }
    auto& record = m_resources[a_resource.index];
    unmap_slices(record);
    for (auto& slices : record.bufferSlices)
    {
        slices.clear();
    }
    record.textureSlices.clear();
    record.resource.Reset();
    return Result<void>::success();
}

/// @brief 所有権検証後に物理資源を借用する
Result<ID3D12Resource*> DX12ResourcePool::resource(GpuResourceHandle a_resource) const
{
    if (!owns(a_resource))
    {
        return Result<ID3D12Resource*>::failure({ErrorCategory::InvalidState, "DX12ResourcePool.resource"});
    }
    return Result<ID3D12Resource*>::success(m_resources[a_resource.index].resource.Get());
}

/// @brief 複数 Heap または Frame 用 Slice の範囲を検証して返す
Result<ID3D12Resource*> DX12ResourcePool::resource(GpuResourceHandle a_resource, GpuMemory a_memory,
                                                     std::uint32_t a_index) const
{
    if (!owns(a_resource) || m_resources[a_resource.index].isTexture ||
        a_memory != GpuMemory::Device && a_memory != GpuMemory::Upload && a_memory != GpuMemory::Readback)
    {
        return Result<ID3D12Resource*>::failure({ErrorCategory::InvalidArgument,
                                                   "DX12ResourcePool.resource.slice"});
    }
    const auto& slices = m_resources[a_resource.index].bufferSlices[static_cast<std::size_t>(a_memory)];
    if (a_index >= slices.size())
    {
        return Result<ID3D12Resource*>::failure({ErrorCategory::InvalidArgument,
                                                   "DX12ResourcePool.resource.sliceIndex"});
    }
    return Result<ID3D12Resource*>::success(slices[a_index].Get());
}

/// @brief Texture の複数 Buffer に対する物理資源を借用する
Result<ID3D12Resource*> DX12ResourcePool::texture_resource(GpuResourceHandle a_texture,
                                                             std::uint32_t a_index) const
{
    if (!owns(a_texture) || !m_resources[a_texture.index].isTexture ||
        a_index >= m_resources[a_texture.index].textureSlices.size())
    {
        return Result<ID3D12Resource*>::failure({ErrorCategory::InvalidArgument,
                                                   "DX12ResourcePool.texture_resource"});
    }
    return Result<ID3D12Resource*>::success(m_resources[a_texture.index].textureSlices[a_index].Get());
}

/// @brief Texture の生成時 Descriptor を Handle の世代検証後に返す
Result<GpuTextureDesc> DX12ResourcePool::texture_desc(GpuResourceHandle a_texture) const
{
    if (!owns(a_texture) || !m_resources[a_texture.index].isTexture)
    {
        return Result<GpuTextureDesc>::failure({ErrorCategory::InvalidArgument,
                                                 "DX12ResourcePool.texture_desc"});
    }
    return Result<GpuTextureDesc>::success(m_resources[a_texture.index].textureDesc);
}

/// @brief Map した Pointer は Resource 破棄時または Pool 終了時に Unmap する
Result<GpuBufferCpuView> DX12ResourcePool::buffer_cpu_view(GpuResourceHandle a_buffer, GpuMemory a_memory)
{
    using ViewResult = Result<GpuBufferCpuView>;
    if (!owns(a_buffer) || m_resources[a_buffer.index].isTexture ||
        (a_memory != GpuMemory::Upload && a_memory != GpuMemory::Readback))
    {
        return ViewResult::failure({ErrorCategory::InvalidArgument, "DX12ResourcePool.buffer_cpu_view"});
    }
    auto& record = m_resources[a_buffer.index];
    const auto index = static_cast<std::size_t>(a_memory);
    auto& slices = record.bufferSlices[index];
    if (slices.empty())
    {
        return ViewResult::failure({ErrorCategory::InvalidArgument, "DX12ResourcePool.buffer_cpu_view.memory"});
    }
    if (a_memory == GpuMemory::Readback)
    {
        auto idle = m_queues->wait_idle();
        if (!idle.has_value())
        {
            return ViewResult::failure(*idle.try_error());
        }
    }
    auto& mapped = record.mappedSlices[index];
    if (mapped.empty())
    {
        D3D12_RANGE readRange{0, 0};
        for (auto& slice : slices)
        {
            void* pointer = nullptr;
            const HRESULT result = slice->Map(0, a_memory == GpuMemory::Upload ? &readRange : nullptr, &pointer);
            if (FAILED(result))
            {
                for (std::size_t i = 0; i < mapped.size(); ++i)
                {
                    slices[i]->Unmap(0, nullptr);
                }
                mapped.clear();
                return ViewResult::failure(gpu_error("ID3D12Resource.Map.BufferSlice", result));
            }
            mapped.push_back(static_cast<std::byte*>(pointer));
        }
    }
    return ViewResult::success({record.alignment, record.stride, record.elementCount,
                                slices[0]->GetDesc().Width, mapped});
}

/// @brief Buffer と Texture の Manager 境界で Handle 種類を確認する
Result<bool> DX12ResourcePool::is_texture(GpuResourceHandle a_resource) const
{
    if (!owns(a_resource))
    {
        return Result<bool>::failure({ErrorCategory::InvalidState, "DX12ResourcePool.is_texture"});
    }
    return Result<bool>::success(m_resources[a_resource.index].isTexture);
}

/// @brief 世代付き View から CPU Descriptor を得る
Result<D3D12_CPU_DESCRIPTOR_HANDLE> DX12ResourcePool::cpu_handle(GpuViewHandle a_view) const
{
    if (!owns(a_view))
    {
        return Result<D3D12_CPU_DESCRIPTOR_HANDLE>::failure({ErrorCategory::InvalidState,
                                                               "DX12ResourcePool.cpu_handle"});
    }
    return m_allocator->cpu_handle({heap_kind(a_view.kind), a_view.index, a_view.generation,
                                    m_allocator->owner_id()});
}

/// @brief View が参照する Texture の Format を取得する
Result<GpuTextureFormat> DX12ResourcePool::view_texture_format(GpuViewHandle a_view) const
{
    if (!owns(a_view))
    {
        return Result<GpuTextureFormat>::failure({ErrorCategory::InvalidState,
                                                   "DX12ResourcePool.view_texture_format"});
    }
    const auto resourceHandle = m_views[heap_index(a_view.kind)][a_view.index].resource;
    if (!owns(resourceHandle) || !m_resources[resourceHandle.index].isTexture)
    {
        return Result<GpuTextureFormat>::failure({ErrorCategory::InvalidArgument,
                                                   "DX12ResourcePool.view_texture_format.resource"});
    }
    return Result<GpuTextureFormat>::success(m_resources[resourceHandle.index].format);
}

/// @brief 生存 View に固定された物理 Slice を返す
Result<ID3D12Resource*> DX12ResourcePool::view_resource(GpuViewHandle a_view) const
{
    if (!owns(a_view))
    {
        return Result<ID3D12Resource*>::failure({ErrorCategory::InvalidState,
                                                   "DX12ResourcePool.view_resource"});
    }
    return Result<ID3D12Resource*>::success(m_views[heap_index(a_view.kind)][a_view.index].physicalResource);
}

/// @brief CBV、SRV、UAV の GPU-visible Descriptor を返す
Result<D3D12_GPU_DESCRIPTOR_HANDLE> DX12ResourcePool::gpu_handle(GpuViewHandle a_view) const
{
    if (!owns(a_view) || heap_index(a_view.kind) != 2)
    {
        return Result<D3D12_GPU_DESCRIPTOR_HANDLE>::failure({ErrorCategory::InvalidState,
                                                               "DX12ResourcePool.gpu_handle"});
    }
    return m_allocator->gpu_handle({DescriptorHeapKind::ShaderResource, a_view.index, a_view.generation,
                                    m_allocator->owner_id()});
}

/// @brief 共通 CBV／SRV／UAV Heap を Backend の Bind 処理へ貸し出す
ID3D12DescriptorHeap* DX12ResourcePool::shader_heap() const noexcept
{
    return m_allocator->shader_heap();
}

/// @brief DescriptorAllocator が予約した Texture 範囲の上限を返す
UINT DX12ResourcePool::texture_table_capacity() const noexcept
{
    return m_allocator->texture_capacity();
}

/// @brief 資源の寿命を管理する Queue Pool の Graphics Queue を返す
DX12GpuCommandQueue& DX12ResourcePool::graphics_queue() const noexcept
{
    return m_queues->context(GpuQueueType::Graphics);
}

/// @brief 公開契約の View 種別を検証する
bool DX12ResourcePool::is_view_kind(GpuViewKind a_kind) noexcept
{
    switch (a_kind)
    {
    case GpuViewKind::RenderTarget:
    case GpuViewKind::DepthStencil:
    case GpuViewKind::ShaderResource:
    case GpuViewKind::ConstantBuffer:
    case GpuViewKind::UnorderedAccess:
        return true;
    default:
        return false;
    }
}

/// @brief Shader 用 Descriptor を単一 Heap に集約する
std::size_t DX12ResourcePool::heap_index(GpuViewKind a_kind) noexcept
{
    if (a_kind == GpuViewKind::RenderTarget)
    {
        return 0;
    }
    return a_kind == GpuViewKind::DepthStencil ? 1 : 2;
}

/// @brief View の Heap 選択を Allocator の Kind に変換する
DescriptorHeapKind DX12ResourcePool::heap_kind(GpuViewKind a_kind) noexcept
{
    return static_cast<DescriptorHeapKind>(heap_index(a_kind));
}

/// @brief 外部 Pool または古い世代の Resource Handle を拒否する
bool DX12ResourcePool::owns(GpuResourceHandle a_resource) const noexcept
{
    return a_resource.owner == this && a_resource.index < m_resources.size() &&
           m_resources[a_resource.index].generation == a_resource.generation &&
           m_resources[a_resource.index].resource;
}

/// @brief Map 状態を解除し、次回の Handle 再利用へ持ち越さない
void DX12ResourcePool::unmap_slices(ResourceRecord& a_record) noexcept
{
    for (std::size_t kind = 0; kind < a_record.mappedSlices.size(); ++kind)
    {
        auto& mapped = a_record.mappedSlices[kind];
        const auto& slices = a_record.bufferSlices[kind];
        for (std::size_t index = 0; index < mapped.size(); ++index)
        {
            slices[index]->Unmap(0, nullptr);
        }
        mapped.clear();
    }
}

/// @brief 外部 Pool または古い世代の View Handle を拒否する
bool DX12ResourcePool::owns(GpuViewHandle a_view) const noexcept
{
    if (a_view.owner != this || !is_view_kind(a_view.kind))
    {
        return false;
    }
    const auto heap = heap_index(a_view.kind);
    return a_view.index < m_views[heap].size() && m_views[heap][a_view.index].isAllocated &&
           m_views[heap][a_view.index].kind == a_view.kind &&
           m_views[heap][a_view.index].generation == a_view.generation;
}

/// @brief 解放済み Slot の世代を進めて古い Handle を無効化する
GpuResourceHandle DX12ResourcePool::store(Microsoft::WRL::ComPtr<ID3D12Resource> a_resource,
                                           bool a_isTexture, GpuTextureFormat a_format, GpuMemory a_memory,
                                           bool a_allowUnorderedAccess)
{
    for (std::uint32_t index = 0; index < m_resources.size(); ++index)
    {
        auto& record = m_resources[index];
        if (record.resource)
        {
            continue;
        }
        record.resource = std::move(a_resource);
        record.isTexture = a_isTexture;
        record.format = a_format;
        record.memory = a_memory;
        record.allowUnorderedAccess = a_allowUnorderedAccess;
        ++record.generation;
        if (record.generation == 0)
        {
            ++record.generation;
        }
        return {index, record.generation, this};
    }
    const auto index = static_cast<std::uint32_t>(m_resources.size());
    ResourceRecord record;
    record.resource = std::move(a_resource);
    record.isTexture = a_isTexture;
    record.format = a_format;
    record.memory = a_memory;
    record.allowUnorderedAccess = a_allowUnorderedAccess;
    record.generation = 1;
    m_resources.push_back(std::move(record));
    return {index, 1, this};
}
} // namespace cue::detail
