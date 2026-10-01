#include <DX12/DX12DescriptorAllocator.h>

#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>

#include <Platform/Diagnostics.h>

namespace cue::dx12
{
namespace
{
std::atomic<std::uint64_t> g_nextAllocatorId = 1;

/// @brief Heap 種類ごとの Debug Object 名を返す
const wchar_t* heap_name(D3D12_DESCRIPTOR_HEAP_TYPE a_type, bool a_isShaderVisible) noexcept
{
    switch (a_type)
    {
    case D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV:
        return a_isShaderVisible ? L"CueEngine DX12 Shader-visible CBV/SRV/UAV Heap"
                                 : L"CueEngine DX12 CPU CBV/SRV/UAV Heap";
    case D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER:
        return a_isShaderVisible ? L"CueEngine DX12 Shader-visible Sampler Heap" : L"CueEngine DX12 CPU Sampler Heap";
    case D3D12_DESCRIPTOR_HEAP_TYPE_RTV:
        return L"CueEngine DX12 RTV Heap";
    case D3D12_DESCRIPTOR_HEAP_TYPE_DSV:
        return L"CueEngine DX12 DSV Heap";
    default:
        return L"CueEngine DX12 Descriptor Heap";
    }
}

/// @brief HRESULT を操作名と Native Code を持つ Error に変換する
Error descriptor_error(const char* a_operation, HRESULT a_result)
{
    return {ErrorCategory::PlatformFailure, a_operation, static_cast<std::int64_t>(a_result)};
}
} // namespace

/// @brief 初期値の Handle を Allocator へ渡す前に除外する
bool DX12DescriptorHandle::is_valid() const noexcept
{
    return generation != 0 && allocatorId != 0;
}

/// @brief create の内部でのみ Heap を持たない中間状態を作る
DX12DescriptorAllocator::DX12DescriptorAllocator(CreateToken) noexcept
{
}

/// @brief 容量と Heap 種類を検証して Native Heap と Slot 管理領域を作る
Result<std::unique_ptr<DX12DescriptorAllocator>> DX12DescriptorAllocator::create(
    ID3D12Device& a_device, D3D12_DESCRIPTOR_HEAP_TYPE a_type, std::uint32_t a_capacity, bool a_isShaderVisible)
{
    using AllocatorResult = Result<std::unique_ptr<DX12DescriptorAllocator>>;
    const bool isSupportedType = a_type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV ||
                                 a_type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER ||
                                 a_type == D3D12_DESCRIPTOR_HEAP_TYPE_RTV || a_type == D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    if (!isSupportedType || a_capacity == 0 ||
        (a_isShaderVisible && a_type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV &&
         a_type != D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER))
    {
        return AllocatorResult::failure({ErrorCategory::InvalidArgument, "DX12DescriptorAllocator.create"});
    }

    auto allocator = std::make_unique<DX12DescriptorAllocator>(CreateToken{});
    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.Type = a_type;
    desc.NumDescriptors = a_capacity;
    desc.Flags = a_isShaderVisible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    const HRESULT createResult = a_device.CreateDescriptorHeap(&desc, IID_PPV_ARGS(&allocator->m_heap));
    if (FAILED(createResult))
    {
        return AllocatorResult::failure(descriptor_error("ID3D12Device.CreateDescriptorHeap", createResult));
    }

    allocator->m_incrementSize = a_device.GetDescriptorHandleIncrementSize(a_type);
    if (allocator->m_incrementSize == 0)
    {
        return AllocatorResult::failure({ErrorCategory::PlatformFailure, "GetDescriptorHandleIncrementSize"});
    }

    std::uint64_t allocatorId = g_nextAllocatorId.load(std::memory_order_relaxed);
    while (allocatorId != (std::numeric_limits<std::uint64_t>::max)() &&
           !g_nextAllocatorId.compare_exchange_weak(allocatorId, allocatorId + 1, std::memory_order_relaxed))
    {
    }
    if (allocatorId == (std::numeric_limits<std::uint64_t>::max)())
    {
        return AllocatorResult::failure({ErrorCategory::Fatal, "DX12DescriptorAllocator.id_exhausted"});
    }
    allocator->m_allocatorId = allocatorId;
    allocator->m_type = a_type;
    allocator->m_isShaderVisible = a_isShaderVisible;
    allocator->m_slots.resize(a_capacity);
    allocator->m_freeSlots.reserve(a_capacity);

    const HRESULT nameResult = allocator->m_heap->SetName(heap_name(a_type, a_isShaderVisible));
    if (FAILED(nameResult))
    {
        report_error("DX12DescriptorAllocator", descriptor_error("ID3D12DescriptorHeap.SetName", nameResult),
                     DiagnosticSeverity::Warning);
    }
    return AllocatorResult::success(std::move(allocator));
}

/// @brief 未使用 Slot または返却済み Slot から新しい世代の Handle を作る
Result<DX12DescriptorHandle> DX12DescriptorAllocator::allocate()
{
    std::lock_guard lock(m_mutex);
    std::uint32_t index = 0;
    if (!m_freeSlots.empty())
    {
        index = m_freeSlots.back();
        m_freeSlots.pop_back();
    }
    else if (m_nextIndex < m_slots.size())
    {
        index = m_nextIndex++;
    }
    else
    {
        return Result<DX12DescriptorHandle>::failure({ErrorCategory::InvalidState,
                                                        "DX12DescriptorAllocator.allocate.capacity_exhausted"});
    }
    Slot& slot = m_slots[index];
    slot.isAllocated = true;
    return Result<DX12DescriptorHandle>::success({index, slot.generation, m_allocatorId});
}

/// @brief GPU 完了を呼出側が保証した Slot のみ再利用可能にする
Result<void> DX12DescriptorAllocator::release(DX12DescriptorHandle a_handle)
{
    std::lock_guard lock(m_mutex);
    if (!is_allocated_locked(a_handle))
    {
        return Result<void>::failure(
            {ErrorCategory::InvalidArgument, "DX12DescriptorAllocator.release.invalid_handle"});
    }

    Slot& slot = m_slots[a_handle.index];
    slot.isAllocated = false;
    // 世代が上限に達した Slot は永久に退役させ、古い Handle の再有効化を防ぐ
    if (slot.generation < (std::numeric_limits<std::uint64_t>::max)())
    {
        ++slot.generation;
        m_freeSlots.push_back(a_handle.index);
    }
    return Result<void>::success();
}

/// @brief 所属と世代が一致する Slot の CPU Handle を計算する
Result<D3D12_CPU_DESCRIPTOR_HANDLE> DX12DescriptorAllocator::cpu_handle(DX12DescriptorHandle a_handle) const
{
    std::lock_guard lock(m_mutex);
    if (!is_allocated_locked(a_handle))
    {
        return Result<D3D12_CPU_DESCRIPTOR_HANDLE>::failure(
            {ErrorCategory::InvalidArgument, "DX12DescriptorAllocator.cpu_handle.invalid_handle"});
    }
    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_heap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(a_handle.index) * m_incrementSize;
    return Result<D3D12_CPU_DESCRIPTOR_HANDLE>::success(handle);
}

/// @brief Shader 可視 Heap に限り GPU Handle を計算する
Result<D3D12_GPU_DESCRIPTOR_HANDLE> DX12DescriptorAllocator::gpu_handle(DX12DescriptorHandle a_handle) const
{
    std::lock_guard lock(m_mutex);
    if (!m_isShaderVisible || !is_allocated_locked(a_handle))
    {
        return Result<D3D12_GPU_DESCRIPTOR_HANDLE>::failure(
            {ErrorCategory::InvalidArgument, "DX12DescriptorAllocator.gpu_handle.invalid_handle"});
    }
    D3D12_GPU_DESCRIPTOR_HANDLE handle = m_heap->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<UINT64>(a_handle.index) * m_incrementSize;
    return Result<D3D12_GPU_DESCRIPTOR_HANDLE>::success(handle);
}

/// @brief Heap Pointer を所有権を移さず返す
ID3D12DescriptorHeap* DX12DescriptorAllocator::heap() const noexcept
{
    return m_heap.Get();
}

/// @brief Heap の作成種類を返す
D3D12_DESCRIPTOR_HEAP_TYPE DX12DescriptorAllocator::type() const noexcept
{
    return m_type;
}

/// @brief GPU から参照できる Heap か返す
bool DX12DescriptorAllocator::is_shader_visible() const noexcept
{
    return m_isShaderVisible;
}

/// @brief Allocator 固有 ID と現在世代を照合して Stale Handle を拒否する
bool DX12DescriptorAllocator::is_allocated_locked(DX12DescriptorHandle a_handle) const noexcept
{
    return a_handle.is_valid() && a_handle.allocatorId == m_allocatorId && a_handle.index < m_slots.size() &&
           m_slots[a_handle.index].isAllocated && m_slots[a_handle.index].generation == a_handle.generation;
}
} // namespace cue::dx12
