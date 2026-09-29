#include "D3D12ResourcePool.h"

#include <cstddef>
#include <cstring>
#include <limits>
#include <string>
#include <utility>

namespace cue::detail
{
namespace
{
constexpr std::array<D3D12_DESCRIPTOR_HEAP_TYPE, 3> k_heapTypes = {
    D3D12_DESCRIPTOR_HEAP_TYPE_RTV, D3D12_DESCRIPTOR_HEAP_TYPE_DSV,
    D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV};

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
} // namespace

/// @brief 三種類の Descriptor Heap を同じ容量で作る
Result<std::unique_ptr<D3D12ResourcePool>> D3D12ResourcePool::create(D3D12DeviceContext& a_device,
                                                                       D3D12QueuePool& a_queues,
                                                                       UINT a_viewCapacity)
{
    using PoolResult = Result<std::unique_ptr<D3D12ResourcePool>>;
    if (a_viewCapacity == 0)
    {
        return PoolResult::failure({ErrorCategory::InvalidArgument, "D3D12ResourcePool.create.capacity"});
    }
    auto pool = std::make_unique<D3D12ResourcePool>();
    pool->m_device = a_device.device();
    pool->m_queues = &a_queues;
    for (std::size_t kind = 0; kind < k_heapTypes.size(); ++kind)
    {
        D3D12_DESCRIPTOR_HEAP_DESC desc{};
        desc.Type = k_heapTypes[kind];
        desc.NumDescriptors = a_viewCapacity;
        desc.Flags = kind == 2
                         ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        const HRESULT result = pool->m_device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&pool->m_heaps[kind]));
        if (FAILED(result))
        {
            return PoolResult::failure(gpu_error("ID3D12Device.CreateDescriptorHeap.GpuResources", result));
        }
        const std::wstring name = L"CueEngine GPU Resource Views " + std::to_wstring(kind);
        const HRESULT nameResult = pool->m_heaps[kind]->SetName(name.c_str());
        if (FAILED(nameResult))
        {
            return PoolResult::failure(gpu_error("ID3D12DescriptorHeap.SetName.GpuResources", nameResult));
        }
        pool->m_strides[kind] = pool->m_device->GetDescriptorHandleIncrementSize(k_heapTypes[kind]);
        pool->m_views[kind].resize(a_viewCapacity);
    }
    return PoolResult::success(std::move(pool));
}

/// @brief Memory 種別に合わせた Heap と初期状態で Buffer を生成する
Result<GpuResourceHandle> D3D12ResourcePool::create_buffer(GpuBufferDesc a_desc)
{
    if (a_desc.size == 0 || (a_desc.memory != GpuMemory::Device && a_desc.memory != GpuMemory::Upload &&
                             a_desc.memory != GpuMemory::Readback) ||
        (a_desc.allowUnorderedAccess && a_desc.memory != GpuMemory::Device))
    {
        return Result<GpuResourceHandle>::failure({ErrorCategory::InvalidArgument,
                                                    "D3D12ResourcePool.create_buffer"});
    }
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = a_desc.memory == GpuMemory::Device ? D3D12_HEAP_TYPE_DEFAULT
                : a_desc.memory == GpuMemory::Upload ? D3D12_HEAP_TYPE_UPLOAD : D3D12_HEAP_TYPE_READBACK;
    const auto state = a_desc.memory == GpuMemory::Upload ? D3D12_RESOURCE_STATE_GENERIC_READ
                       : a_desc.memory == GpuMemory::Readback ? D3D12_RESOURCE_STATE_COPY_DEST
                                                                : D3D12_RESOURCE_STATE_COMMON;
    const auto desc = buffer_desc(a_desc.size, a_desc.allowUnorderedAccess);
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    const HRESULT result = m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                              state, nullptr, IID_PPV_ARGS(&resource));
    if (FAILED(result))
    {
        return Result<GpuResourceHandle>::failure(gpu_error("ID3D12Device.CreateCommittedResource.Buffer", result));
    }
    const HRESULT nameResult = resource->SetName(L"CueEngine GPU Buffer");
    if (FAILED(nameResult))
    {
        return Result<GpuResourceHandle>::failure(gpu_error("ID3D12Resource.SetName.Buffer", nameResult));
    }
    return Result<GpuResourceHandle>::success(store(std::move(resource), false,
                                                    GpuTextureFormat::Rgba8Unorm, a_desc.memory,
                                                    a_desc.allowUnorderedAccess));
}

/// @brief Color または Depth の独立した Default Heap Texture を生成する
Result<GpuResourceHandle> D3D12ResourcePool::create_texture(GpuTextureDesc a_desc)
{
    if (a_desc.width == 0 || a_desc.height == 0 ||
        (a_desc.allowUnorderedAccess && a_desc.format == GpuTextureFormat::Depth32Float) ||
        (a_desc.format != GpuTextureFormat::Rgba8Unorm && a_desc.format != GpuTextureFormat::Depth32Float))
    {
        return Result<GpuResourceHandle>::failure({ErrorCategory::InvalidArgument,
                                                    "D3D12ResourcePool.create_texture"});
    }
    const bool isDepth = a_desc.format == GpuTextureFormat::Depth32Float;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = a_desc.width;
    desc.Height = a_desc.height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    // Depth SRV と DSV が同一 Resource を参照できるよう物理 Format を Typeless にする
    desc.Format = isDepth ? DXGI_FORMAT_R32_TYPELESS : DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Flags = isDepth ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if (a_desc.allowUnorderedAccess)
    {
        desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    }
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_CLEAR_VALUE clear{};
    clear.Format = isDepth ? DXGI_FORMAT_D32_FLOAT : desc.Format;
    if (isDepth)
    {
        clear.DepthStencil.Depth = 1.0f;
    }
    else
    {
        clear.Color[0] = 0.07f;
        clear.Color[1] = 0.13f;
        clear.Color[2] = 0.25f;
        clear.Color[3] = 1.0f;
    }
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    const HRESULT result = m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_COMMON, &clear, IID_PPV_ARGS(&resource));
    if (FAILED(result))
    {
        return Result<GpuResourceHandle>::failure(gpu_error("ID3D12Device.CreateCommittedResource.Texture", result));
    }
    const HRESULT nameResult = resource->SetName(isDepth ? L"CueEngine GPU Depth Texture"
                                                       : L"CueEngine GPU Color Texture");
    if (FAILED(nameResult))
    {
        return Result<GpuResourceHandle>::failure(gpu_error("ID3D12Resource.SetName.Texture", nameResult));
    }
    return Result<GpuResourceHandle>::success(store(std::move(resource), true, a_desc.format, GpuMemory::Device,
                                                    a_desc.allowUnorderedAccess));
}

/// @brief Upload Buffer の境界を検証して CPU Data を書く
Result<void> D3D12ResourcePool::write_buffer(GpuResourceHandle a_buffer, std::uint64_t a_offset,
                                             const void* a_data, std::uint64_t a_size)
{
    if (!owns(a_buffer) || !a_data || a_size == 0)
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "D3D12ResourcePool.write_buffer"});
    }
    auto& record = m_resources[a_buffer.index];
    const auto width = record.resource->GetDesc().Width;
    if (record.isTexture || record.memory != GpuMemory::Upload || a_offset > width || a_size > width - a_offset)
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "D3D12ResourcePool.write_buffer.range"});
    }
    D3D12_RANGE readRange{0, 0};
    void* mapped = nullptr;
    const HRESULT result = record.resource->Map(0, &readRange, &mapped);
    if (FAILED(result))
    {
        return Result<void>::failure(gpu_error("ID3D12Resource.Map.Upload", result));
    }
    std::memcpy(static_cast<std::byte*>(mapped) + a_offset, a_data, static_cast<std::size_t>(a_size));
    D3D12_RANGE writtenRange{static_cast<SIZE_T>(a_offset), static_cast<SIZE_T>(a_offset + a_size)};
    record.resource->Unmap(0, &writtenRange);
    return Result<void>::success();
}

/// @brief Readback Buffer の GPU 完了後だけ CPU Data を読む
Result<void> D3D12ResourcePool::read_buffer(GpuResourceHandle a_buffer, std::uint64_t a_offset,
                                            void* a_data, std::uint64_t a_size)
{
    if (!owns(a_buffer) || !a_data || a_size == 0)
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "D3D12ResourcePool.read_buffer"});
    }
    auto& record = m_resources[a_buffer.index];
    const auto width = record.resource->GetDesc().Width;
    if (record.isTexture || record.memory != GpuMemory::Readback || a_offset > width || a_size > width - a_offset)
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "D3D12ResourcePool.read_buffer.range"});
    }
    auto idle = m_queues->wait_idle();
    if (!idle.has_value())
    {
        return idle;
    }
    D3D12_RANGE readRange{static_cast<SIZE_T>(a_offset), static_cast<SIZE_T>(a_offset + a_size)};
    void* mapped = nullptr;
    const HRESULT result = record.resource->Map(0, &readRange, &mapped);
    if (FAILED(result))
    {
        return Result<void>::failure(gpu_error("ID3D12Resource.Map.Readback", result));
    }
    std::memcpy(a_data, static_cast<const std::byte*>(mapped) + a_offset, static_cast<std::size_t>(a_size));
    D3D12_RANGE writtenRange{0, 0};
    record.resource->Unmap(0, &writtenRange);
    return Result<void>::success();
}

/// @brief Resource 種別と範囲を検証して、適合する Heap の空き Slot に Descriptor を作る
Result<GpuViewHandle> D3D12ResourcePool::create_view(GpuResourceHandle a_resource, GpuViewDesc a_desc)
{
    if (!owns(a_resource) || !is_view_kind(a_desc.kind))
    {
        return Result<GpuViewHandle>::failure({ErrorCategory::InvalidArgument, "D3D12ResourcePool.create_view"});
    }
    const auto& record = m_resources[a_resource.index];
    const bool isDepth = record.isTexture && record.format == GpuTextureFormat::Depth32Float;
    const bool isBufferView = a_desc.kind == GpuViewKind::ConstantBuffer ||
                              (a_desc.kind == GpuViewKind::ShaderResource && !record.isTexture) ||
                              (a_desc.kind == GpuViewKind::UnorderedAccess && !record.isTexture);
    if ((record.isTexture && (a_desc.bufferOffset != 0 || a_desc.bufferSize != 0 ||
                              a_desc.structureStride != 0 || a_desc.kind == GpuViewKind::ConstantBuffer)) ||
        (!record.isTexture && (!isBufferView || record.memory == GpuMemory::Readback)) ||
        (isDepth && a_desc.kind != GpuViewKind::DepthStencil && a_desc.kind != GpuViewKind::ShaderResource) ||
        (!isDepth && a_desc.kind == GpuViewKind::DepthStencil) ||
        (a_desc.kind == GpuViewKind::UnorderedAccess && !record.allowUnorderedAccess))
    {
        return Result<GpuViewHandle>::failure({ErrorCategory::InvalidArgument,
                                                "D3D12ResourcePool.create_view.format"});
    }
    const auto size = record.resource->GetDesc().Width;
    if (isBufferView && (a_desc.bufferSize == 0 || a_desc.bufferOffset > size ||
                         a_desc.bufferSize > size - a_desc.bufferOffset))
    {
        return Result<GpuViewHandle>::failure({ErrorCategory::InvalidArgument,
                                                "D3D12ResourcePool.create_view.range"});
    }
    if (a_desc.kind == GpuViewKind::ConstantBuffer &&
        (a_desc.bufferOffset % D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT != 0 ||
         a_desc.bufferSize % D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT != 0 ||
         a_desc.bufferSize > std::numeric_limits<UINT>::max()))
    {
        return Result<GpuViewHandle>::failure({ErrorCategory::InvalidArgument,
                                                "D3D12ResourcePool.create_view.cbvAlignment"});
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
                                                    "D3D12ResourcePool.create_view.elements"});
        }
    }
    const auto heap = heap_index(a_desc.kind);
    for (std::uint32_t index = 0; index < m_views[heap].size(); ++index)
    {
        auto& view = m_views[heap][index];
        if (view.isAllocated)
        {
            continue;
        }
        auto handle = m_heaps[heap]->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(index) * m_strides[heap];
        switch (a_desc.kind)
        {
        case GpuViewKind::RenderTarget:
            m_device->CreateRenderTargetView(record.resource.Get(), nullptr, handle);
            break;
        case GpuViewKind::DepthStencil:
        {
            D3D12_DEPTH_STENCIL_VIEW_DESC desc{};
            desc.Format = DXGI_FORMAT_D32_FLOAT;
            desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
            m_device->CreateDepthStencilView(record.resource.Get(), &desc, handle);
            break;
        }
        case GpuViewKind::ShaderResource:
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
            desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            if (record.isTexture)
            {
                desc.Format = isDepth ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
                desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                desc.Texture2D.MipLevels = 1;
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
            m_device->CreateShaderResourceView(record.resource.Get(), &desc, handle);
            break;
        }
        case GpuViewKind::ConstantBuffer:
        {
            D3D12_CONSTANT_BUFFER_VIEW_DESC desc{};
            desc.BufferLocation = record.resource->GetGPUVirtualAddress() + a_desc.bufferOffset;
            desc.SizeInBytes = static_cast<UINT>(a_desc.bufferSize);
            m_device->CreateConstantBufferView(&desc, handle);
            break;
        }
        case GpuViewKind::UnorderedAccess:
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
            if (record.isTexture)
            {
                desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
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
            m_device->CreateUnorderedAccessView(record.resource.Get(), nullptr, &desc, handle);
            break;
        }
        }
        view.isAllocated = true;
        view.resource = a_resource;
        view.kind = a_desc.kind;
        ++view.generation;
        if (view.generation == 0)
        {
            ++view.generation;
        }
        return Result<GpuViewHandle>::success({a_desc.kind, index, view.generation, this});
    }
    return Result<GpuViewHandle>::failure({ErrorCategory::InvalidState, "D3D12ResourcePool.viewCapacity"});
}

/// @brief Queue 完了後に View Slot を無効化する
Result<void> D3D12ResourcePool::destroy_view(GpuViewHandle a_view)
{
    if (!owns(a_view))
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "D3D12ResourcePool.destroy_view"});
    }
    auto idle = m_queues->wait_idle();
    if (!idle.has_value())
    {
        return idle;
    }
    m_views[heap_index(a_view.kind)][a_view.index].isAllocated = false;
    return Result<void>::success();
}

/// @brief View が残っていない資源だけを Queue 完了後に解放する
Result<void> D3D12ResourcePool::destroy(GpuResourceHandle a_resource)
{
    if (!owns(a_resource))
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "D3D12ResourcePool.destroy"});
    }
    for (const auto& views : m_views)
    {
        for (const auto& view : views)
        {
            if (view.isAllocated && view.resource.index == a_resource.index &&
                view.resource.generation == a_resource.generation)
            {
                return Result<void>::failure({ErrorCategory::InvalidState,
                                              "D3D12ResourcePool.destroy.activeView"});
            }
        }
    }
    auto idle = m_queues->wait_idle();
    if (!idle.has_value())
    {
        return idle;
    }
    m_resources[a_resource.index].resource.Reset();
    return Result<void>::success();
}

/// @brief 所有権検証後に物理資源を借用する
Result<ID3D12Resource*> D3D12ResourcePool::resource(GpuResourceHandle a_resource) const
{
    if (!owns(a_resource))
    {
        return Result<ID3D12Resource*>::failure({ErrorCategory::InvalidState, "D3D12ResourcePool.resource"});
    }
    return Result<ID3D12Resource*>::success(m_resources[a_resource.index].resource.Get());
}

/// @brief 世代付き View から CPU Descriptor を得る
Result<D3D12_CPU_DESCRIPTOR_HANDLE> D3D12ResourcePool::cpu_handle(GpuViewHandle a_view) const
{
    if (!owns(a_view))
    {
        return Result<D3D12_CPU_DESCRIPTOR_HANDLE>::failure({ErrorCategory::InvalidState,
                                                               "D3D12ResourcePool.cpu_handle"});
    }
    const auto heap = heap_index(a_view.kind);
    auto handle = m_heaps[heap]->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(a_view.index) * m_strides[heap];
    return Result<D3D12_CPU_DESCRIPTOR_HANDLE>::success(handle);
}

/// @brief CBV、SRV、UAV の GPU-visible Descriptor を返す
Result<D3D12_GPU_DESCRIPTOR_HANDLE> D3D12ResourcePool::gpu_handle(GpuViewHandle a_view) const
{
    if (!owns(a_view) || heap_index(a_view.kind) != 2)
    {
        return Result<D3D12_GPU_DESCRIPTOR_HANDLE>::failure({ErrorCategory::InvalidState,
                                                               "D3D12ResourcePool.gpu_handle"});
    }
    auto handle = m_heaps[2]->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<UINT64>(a_view.index) * m_strides[2];
    return Result<D3D12_GPU_DESCRIPTOR_HANDLE>::success(handle);
}

/// @brief 共通 CBV／SRV／UAV Heap を Backend の Bind 処理へ貸し出す
ID3D12DescriptorHeap* D3D12ResourcePool::shader_heap() const noexcept
{
    return m_heaps[2].Get();
}

/// @brief 資源の寿命を管理する Queue Pool の Graphics Queue を返す
D3D12QueueContext& D3D12ResourcePool::graphics_queue() const noexcept
{
    return m_queues->context(GpuQueueType::Graphics);
}

/// @brief 公開契約の View 種別を検証する
bool D3D12ResourcePool::is_view_kind(GpuViewKind a_kind) noexcept
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
std::size_t D3D12ResourcePool::heap_index(GpuViewKind a_kind) noexcept
{
    if (a_kind == GpuViewKind::RenderTarget)
    {
        return 0;
    }
    return a_kind == GpuViewKind::DepthStencil ? 1 : 2;
}

/// @brief 外部 Pool または古い世代の Resource Handle を拒否する
bool D3D12ResourcePool::owns(GpuResourceHandle a_resource) const noexcept
{
    return a_resource.owner == this && a_resource.index < m_resources.size() &&
           m_resources[a_resource.index].generation == a_resource.generation &&
           m_resources[a_resource.index].resource;
}

/// @brief 外部 Pool または古い世代の View Handle を拒否する
bool D3D12ResourcePool::owns(GpuViewHandle a_view) const noexcept
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
GpuResourceHandle D3D12ResourcePool::store(Microsoft::WRL::ComPtr<ID3D12Resource> a_resource,
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
