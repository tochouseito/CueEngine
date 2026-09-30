#include "DescriptorAllocator.h"

#include <array>
#include <atomic>
#include <string>
#include <utility>

namespace cue::detail
{
namespace
{
constexpr std::array<D3D12_DESCRIPTOR_HEAP_TYPE, 3> k_heapTypes = {
    D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
    D3D12_DESCRIPTOR_HEAP_TYPE_DSV,
    D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV};

std::atomic<std::uint64_t> s_nextOwnerId = 1;
}

/// @brief 同じ Index と世代の別 Heap 割当を取り違えない識別子を設定する
DescriptorAllocator::DescriptorAllocator() : m_ownerId(s_nextOwnerId.fetch_add(1, std::memory_order_relaxed))
{
}

/// @brief Heap を Kind ごとに生成し、失敗時は未公開の Owner とともに破棄する
Result<std::unique_ptr<DescriptorAllocator>> DescriptorAllocator::create(
    DX12RenderDevice& a_device, UINT a_rtvCapacity, UINT a_dsvCapacity, UINT a_shaderCapacity)
{
    using AllocatorResult = Result<std::unique_ptr<DescriptorAllocator>>;
    if (a_rtvCapacity == 0 || a_dsvCapacity == 0 || a_shaderCapacity == 0)
    {
        return AllocatorResult::failure({ErrorCategory::InvalidArgument, "DescriptorAllocator.create.capacity"});
    }
    auto allocator = std::make_unique<DescriptorAllocator>();
    allocator->m_device = a_device.device();
    // 小容量 Heap は従来の共有 Pool とし、通常容量では Texture Table を分離する
    allocator->m_textureCapacity = a_shaderCapacity >= 8 ? (a_shaderCapacity + 1u) / 2u
                                                        : a_shaderCapacity;
    const std::array<UINT, 3> capacities = {a_rtvCapacity, a_dsvCapacity, a_shaderCapacity};
    for (std::size_t index = 0; index < k_heapTypes.size(); ++index)
    {
        D3D12_DESCRIPTOR_HEAP_DESC desc{};
        desc.Type = k_heapTypes[index];
        desc.NumDescriptors = capacities[index];
        desc.Flags = index == 2 ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
                                : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        auto& heap = allocator->m_heaps[index];
        const HRESULT result = a_device.device()->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap.heap));
        if (FAILED(result))
        {
            return AllocatorResult::failure(gpu_error("ID3D12Device.CreateDescriptorHeap", result));
        }
        const std::wstring name = L"CueEngine Descriptor Heap " + std::to_wstring(index);
        const HRESULT nameResult = heap.heap->SetName(name.c_str());
        if (FAILED(nameResult))
        {
            return AllocatorResult::failure(gpu_error("ID3D12DescriptorHeap.SetName", nameResult));
        }
        heap.stride = a_device.device()->GetDescriptorHandleIncrementSize(k_heapTypes[index]);
        heap.slots.resize(capacities[index]);
    }
    // UAV Clear の CPU Handle は非 Shader-visible Heap を要求する
    D3D12_DESCRIPTOR_HEAP_DESC shaderCpuDesc{};
    shaderCpuDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    shaderCpuDesc.NumDescriptors = a_shaderCapacity;
    shaderCpuDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    const HRESULT shaderCpuResult = a_device.device()->CreateDescriptorHeap(
        &shaderCpuDesc, IID_PPV_ARGS(&allocator->m_shaderCpuHeap));
    if (FAILED(shaderCpuResult))
    {
        return AllocatorResult::failure(gpu_error("ID3D12Device.CreateDescriptorHeap.Shadow", shaderCpuResult));
    }
    const HRESULT shaderCpuNameResult = allocator->m_shaderCpuHeap->SetName(
        L"CueEngine Shader CPU Descriptor Heap");
    if (FAILED(shaderCpuNameResult))
    {
        return AllocatorResult::failure(gpu_error("ID3D12DescriptorHeap.SetName.Shadow", shaderCpuNameResult));
    }
    // ImGui は自身の Shader-visible Heap を Bind するため Renderer の Table と分離する
    D3D12_DESCRIPTOR_HEAP_DESC imguiDesc{};
    imguiDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    imguiDesc.NumDescriptors = 1;
    imguiDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    const HRESULT imguiResult = a_device.device()->CreateDescriptorHeap(
        &imguiDesc, IID_PPV_ARGS(&allocator->m_imguiHeap));
    if (FAILED(imguiResult))
    {
        return AllocatorResult::failure(gpu_error("ID3D12Device.CreateDescriptorHeap.ImGui", imguiResult));
    }
    const HRESULT imguiNameResult = allocator->m_imguiHeap->SetName(L"CueEngine ImGui Descriptor Heap");
    if (FAILED(imguiNameResult))
    {
        return AllocatorResult::failure(gpu_error("ID3D12DescriptorHeap.SetName.ImGui", imguiNameResult));
    }
    return AllocatorResult::success(std::move(allocator));
}

/// @brief 空き Slot に新しい世代を割り当てる
Result<DescriptorSlot> DescriptorAllocator::allocate(DescriptorHeapKind a_kind)
{
    if (!is_kind(a_kind))
    {
        return Result<DescriptorSlot>::failure({ErrorCategory::InvalidArgument, "DescriptorAllocator.allocate.kind"});
    }
    auto& slots = m_heaps[static_cast<std::size_t>(a_kind)].slots;
    const UINT begin = a_kind == DescriptorHeapKind::ShaderResource && slots.size() >= 8
        ? m_textureCapacity : 0;
    for (UINT index = begin; index < slots.size(); ++index)
    {
        auto& slot = slots[index];
        if (slot.isAllocated)
        {
            continue;
        }
        slot.isAllocated = true;
        ++slot.generation;
        if (slot.generation == 0)
        {
            ++slot.generation;
        }
        return Result<DescriptorSlot>::success({a_kind, index, slot.generation, m_ownerId});
    }
    return Result<DescriptorSlot>::failure({ErrorCategory::InvalidState, "DescriptorAllocator.allocate.capacity"});
}

/// @brief Shader Heap 前半の連続した Texture Table から一つ確保する
Result<DescriptorSlot> DescriptorAllocator::allocate_texture()
{
    auto& slots = m_heaps[static_cast<std::size_t>(DescriptorHeapKind::ShaderResource)].slots;
    for (UINT index = 0; index < m_textureCapacity; ++index)
    {
        auto& slot = slots[index];
        if (slot.isAllocated)
        {
            continue;
        }
        slot.isAllocated = true;
        ++slot.generation;
        if (slot.generation == 0)
        {
            ++slot.generation;
        }
        return Result<DescriptorSlot>::success({DescriptorHeapKind::ShaderResource, index,
                                                slot.generation, m_ownerId});
    }
    return Result<DescriptorSlot>::failure({ErrorCategory::InvalidState,
                                            "DescriptorAllocator.allocate_texture.capacity"});
}

/// @brief 世代の一致を確認して Slot を空ける
Result<void> DescriptorAllocator::release(DescriptorSlot a_slot)
{
    if (!owns(a_slot))
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "DescriptorAllocator.release"});
    }
    m_heaps[static_cast<std::size_t>(a_slot.kind)].slots[a_slot.index].isAllocated = false;
    return Result<void>::success();
}

/// @brief Descriptor の CPU 側位置を計算する
Result<D3D12_CPU_DESCRIPTOR_HANDLE> DescriptorAllocator::cpu_handle(DescriptorSlot a_slot) const
{
    if (!owns(a_slot))
    {
        return Result<D3D12_CPU_DESCRIPTOR_HANDLE>::failure({ErrorCategory::InvalidState,
                                                               "DescriptorAllocator.cpu_handle"});
    }
    const auto& heap = m_heaps[static_cast<std::size_t>(a_slot.kind)];
    auto handle = a_slot.kind == DescriptorHeapKind::ShaderResource
        ? m_shaderCpuHeap->GetCPUDescriptorHandleForHeapStart()
        : heap.heap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(a_slot.index) * heap.stride;
    return Result<D3D12_CPU_DESCRIPTOR_HANDLE>::success(handle);
}

/// @brief Shader Binding 用 Descriptor を作成済み CPU Heap から複製する
Result<void> DescriptorAllocator::sync_shader_descriptor(DescriptorSlot a_slot)
{
    if (!owns(a_slot) || a_slot.kind != DescriptorHeapKind::ShaderResource)
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument,
                                      "DescriptorAllocator.sync_shader_descriptor"});
    }
    const auto& heap = m_heaps[static_cast<std::size_t>(a_slot.kind)];
    auto source = m_shaderCpuHeap->GetCPUDescriptorHandleForHeapStart();
    auto target = heap.heap->GetCPUDescriptorHandleForHeapStart();
    source.ptr += static_cast<SIZE_T>(a_slot.index) * heap.stride;
    target.ptr += static_cast<SIZE_T>(a_slot.index) * heap.stride;
    m_device->CopyDescriptorsSimple(1, target, source, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    return Result<void>::success();
}

/// @brief Shader-visible Heap だけ GPU Handle を貸し出す
Result<D3D12_GPU_DESCRIPTOR_HANDLE> DescriptorAllocator::gpu_handle(DescriptorSlot a_slot) const
{
    if (!owns(a_slot) || a_slot.kind != DescriptorHeapKind::ShaderResource)
    {
        return Result<D3D12_GPU_DESCRIPTOR_HANDLE>::failure({ErrorCategory::InvalidState,
                                                               "DescriptorAllocator.gpu_handle"});
    }
    const auto& heap = m_heaps[static_cast<std::size_t>(a_slot.kind)];
    auto handle = heap.heap->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<UINT64>(a_slot.index) * heap.stride;
    return Result<D3D12_GPU_DESCRIPTOR_HANDLE>::success(handle);
}

/// @brief 単一の Shader-visible Heap を返す
ID3D12DescriptorHeap* DescriptorAllocator::shader_heap() const noexcept
{
    return m_heaps[static_cast<std::size_t>(DescriptorHeapKind::ShaderResource)].heap.Get();
}

/// @brief ImGui の Descriptor は通常描画の Bind と独立した Heap に置く
ID3D12DescriptorHeap* DescriptorAllocator::imgui_heap() const noexcept
{
    return m_imguiHeap.Get();
}

/// @brief Texture 用に予約した Table は Shader Heap の先頭から始まる
D3D12_GPU_DESCRIPTOR_HANDLE DescriptorAllocator::texture_table_base() const noexcept
{
    return shader_heap()->GetGPUDescriptorHandleForHeapStart();
}

/// @brief 初期化済み Heap の Slot 数を返す
UINT DescriptorAllocator::capacity(DescriptorHeapKind a_kind) const noexcept
{
    return is_kind(a_kind) ? static_cast<UINT>(m_heaps[static_cast<std::size_t>(a_kind)].slots.size()) : 0;
}

/// @brief Root Signature の Table 長を割当可能領域と照合するため返す
UINT DescriptorAllocator::texture_capacity() const noexcept
{
    return m_textureCapacity;
}

/// @brief 公開 Handle から内部 Slot を再構成する Owner 識別子を返す
std::uint64_t DescriptorAllocator::owner_id() const noexcept
{
    return m_ownerId;
}

/// @brief Kind が三種類の Heap の一つか確認する
bool DescriptorAllocator::is_kind(DescriptorHeapKind a_kind) noexcept
{
    return static_cast<std::size_t>(a_kind) < k_heapTypes.size();
}

/// @brief 解放済みまたは古い世代の Slot を拒否する
bool DescriptorAllocator::owns(DescriptorSlot a_slot) const noexcept
{
    if (!is_kind(a_slot.kind))
    {
        return false;
    }
    const auto& slots = m_heaps[static_cast<std::size_t>(a_slot.kind)].slots;
    return a_slot.ownerId == m_ownerId && a_slot.index < slots.size() && slots[a_slot.index].isAllocated &&
           slots[a_slot.index].generation == a_slot.generation;
}
} // namespace cue::detail
