#include "DX12CommandPool.h"

#include <cstddef>
#include <cstring>
#include <string>
#include <utility>

namespace cue::detail
{
/// @brief 最後の Submit に対応する Queue 完了値を確認する
bool DX12GpuCommandContext::is_pending_fence_complete() const noexcept
{
    return fenceValue == 0 || (m_queue && m_queue->is_complete(fenceValue));
}

/// @brief Context に残した Queue の Fence 完了まで待つ
Result<void> DX12GpuCommandContext::wait_for_pending_fence() const
{
    if (fenceValue == 0)
    {
        return Result<void>::success();
    }
    if (!m_queue)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "DX12GpuCommandContext.queue"});
    }
    return m_queue->wait_for(fenceValue);
}

/// @brief Copy Queue は Timestamp Query を持たない
bool DX12GpuCommandContext::supports_timestamps() const noexcept
{
    return queryHeap && queryReadback && m_type != GpuQueueType::Copy;
}

/// @brief 記録中 List に GPU Timestamp を書き込む
Result<void> DX12GpuCommandContext::write_timestamp(std::uint32_t a_queryIndex)
{
    if (!supports_timestamps() || status != ContextStatus::Recording ||
        a_queryIndex >= k_maxTimestampQueries)
    {
        return Result<void>::failure({ErrorCategory::InvalidState,
                                      "DX12GpuCommandContext.write_timestamp"});
    }
    list->EndQuery(queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, a_queryIndex);
    return Result<void>::success();
}

/// @brief Query Heap の指定範囲を Context 固有の Readback Buffer へ解決する
Result<void> DX12GpuCommandContext::resolve_timestamps(std::uint32_t a_firstQuery,
                                                         std::uint32_t a_queryCount)
{
    if (!supports_timestamps() || status != ContextStatus::Recording || a_queryCount == 0 ||
        a_firstQuery >= k_maxTimestampQueries ||
        a_queryCount > k_maxTimestampQueries - a_firstQuery)
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument,
                                      "DX12GpuCommandContext.resolve_timestamps"});
    }
    list->ResolveQueryData(queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, a_firstQuery, a_queryCount,
                           queryReadback.Get(), static_cast<UINT64>(a_firstQuery) * sizeof(UINT64));
    return Result<void>::success();
}

/// @brief Fence 完了を確かめて Readback Buffer の Query 値を読む
Result<std::uint64_t> DX12GpuCommandContext::read_timestamp(std::uint32_t a_queryIndex) const
{
    if (!supports_timestamps() || a_queryIndex >= k_maxTimestampQueries ||
        status != ContextStatus::Idle || fenceValue == 0 || !is_pending_fence_complete())
    {
        return Result<std::uint64_t>::failure({ErrorCategory::InvalidState,
                                                "DX12GpuCommandContext.read_timestamp"});
    }
    D3D12_RANGE range{static_cast<SIZE_T>(a_queryIndex) * sizeof(UINT64),
                       static_cast<SIZE_T>(a_queryIndex + 1) * sizeof(UINT64)};
    void* mapped = nullptr;
    const HRESULT result = queryReadback->Map(0, &range, &mapped);
    if (FAILED(result))
    {
        return Result<std::uint64_t>::failure(gpu_error("ID3D12Resource.Map.Timestamp", result));
    }
    std::uint64_t value = 0;
    std::memcpy(&value, static_cast<std::byte*>(mapped) + range.Begin, sizeof(value));
    D3D12_RANGE written{0, 0};
    queryReadback->Unmap(0, &written);
    return Result<std::uint64_t>::success(value);
}

/// @brief PIX 等で参照できる ANSI Event の開始を記録する
void DX12GpuCommandContext::begin_event(const char* a_name)
{
    if (status != ContextStatus::Recording || !list)
    {
        return;
    }
    const char* name = a_name && a_name[0] ? a_name : "UnnamedEvent";
    list->BeginEvent(1, name, static_cast<UINT>(std::strlen(name) + 1));
}

/// @brief 記録中の GPU Event Scope を閉じる
void DX12GpuCommandContext::end_event()
{
    if (status == ContextStatus::Recording && list)
    {
        list->EndEvent();
    }
}

/// @brief Timestamp Query Heap と結果用 Readback Buffer を Context ごとに所有する
Result<void> DX12GpuCommandContext::initialize_queries(ID3D12Device& a_device)
{
    if (m_type == GpuQueueType::Copy)
    {
        return Result<void>::success();
    }
    D3D12_QUERY_HEAP_DESC queryDesc{};
    queryDesc.Count = k_maxTimestampQueries;
    queryDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    HRESULT result = a_device.CreateQueryHeap(&queryDesc, IID_PPV_ARGS(&queryHeap));
    if (FAILED(result))
    {
        return Result<void>::failure(gpu_error("ID3D12Device.CreateQueryHeap", result));
    }
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = k_maxTimestampQueries * sizeof(UINT64);
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    result = a_device.CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&queryReadback));
    if (FAILED(result))
    {
        return Result<void>::failure(gpu_error("ID3D12Device.CreateCommittedResource.Timestamp", result));
    }
    return Result<void>::success();
}

/// @brief 抽象 Queue が同じ DX12 Backend に属することを確認して Context を貸す
Result<CommandContextLease> DX12CommandPool::acquire_context(IQueueContext& a_queue, std::uint32_t a_slot)
{
    auto* queue = dynamic_cast<DX12GpuCommandQueue*>(&a_queue);
    if (!queue)
    {
        return Result<CommandContextLease>::failure({ErrorCategory::InvalidArgument,
                                                      "DX12CommandPool.acquire_context.queue"});
    }
    auto result = acquire(*queue, a_slot);
    if (!result.has_value())
    {
        return Result<CommandContextLease>::failure(*result.try_error());
    }
    const auto lease = result.take_value();
    return Result<CommandContextLease>::success({lease.slot, lease.contextIndex, lease.generation, this});
}

/// @brief Active Lease に対応する Context だけを返す
Result<ICommandContext*> DX12CommandPool::context(CommandContextLease a_lease)
{
    auto nativeResult = native_lease(a_lease);
    if (!nativeResult.has_value() || !is_lease(*nativeResult.try_value(), ContextStatus::Recording))
    {
        return Result<ICommandContext*>::failure({ErrorCategory::InvalidState, "DX12CommandPool.context"});
    }
    return Result<ICommandContext*>::success(&m_contexts[a_lease.contextIndex]);
}

/// @brief Queue に投入後も Lease は retire まで有効なまま保つ
Result<void> DX12CommandPool::submit_context(IQueueContext& a_queue, CommandContextLease a_lease)
{
    auto* queue = dynamic_cast<DX12GpuCommandQueue*>(&a_queue);
    auto nativeResult = native_lease(a_lease);
    if (!queue || !nativeResult.has_value())
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "DX12CommandPool.submit_context"});
    }
    return submit(*queue, nativeResult.take_value());
}

/// @brief Submit 前の失敗時に List を閉じて Lease を失効させる
Result<void> DX12CommandPool::abort_context(CommandContextLease a_lease)
{
    auto nativeResult = native_lease(a_lease);
    if (!nativeResult.has_value())
    {
        return Result<void>::failure(*nativeResult.try_error());
    }
    return abort(nativeResult.take_value());
}

/// @brief Submit 済み List の再利用条件を Fence に記録する
Result<std::uint64_t> DX12CommandPool::retire_context(IQueueContext& a_queue, CommandContextLease a_lease)
{
    auto* queue = dynamic_cast<DX12GpuCommandQueue*>(&a_queue);
    auto nativeResult = native_lease(a_lease);
    if (!queue || !nativeResult.has_value())
    {
        return Result<std::uint64_t>::failure({ErrorCategory::InvalidState,
                                                "DX12CommandPool.retire_context"});
    }
    return retire_fence(*queue, nativeResult.take_value());
}

/// @brief Back Buffer 数の指定 Queue 用 Context を生成する
Result<std::unique_ptr<DX12CommandPool>> DX12CommandPool::create(DX12RenderDevice& a_device,
                                                                     GpuQueueType a_type)
{
    using PoolResult = Result<std::unique_ptr<DX12CommandPool>>;
    if (a_type != GpuQueueType::Graphics && a_type != GpuQueueType::Compute &&
        a_type != GpuQueueType::Copy)
    {
        return PoolResult::failure({ErrorCategory::InvalidArgument, "DX12CommandPool.create.type"});
    }
    const auto listType = a_type == GpuQueueType::Graphics ? D3D12_COMMAND_LIST_TYPE_DIRECT
                          : a_type == GpuQueueType::Compute ? D3D12_COMMAND_LIST_TYPE_COMPUTE
                                                            : D3D12_COMMAND_LIST_TYPE_COPY;
    auto pool = std::make_unique<DX12CommandPool>();
    pool->m_type = a_type;
    pool->m_device = a_device.device();
    pool->m_contexts.resize(k_backBufferCount);
    for (UINT index = 0; index < k_backBufferCount; ++index)
    {
        auto& context = pool->m_contexts[index];
        context.slot = index;
        context.m_type = a_type;
        const HRESULT result = a_device.device()->CreateCommandAllocator(listType,
                                                                           IID_PPV_ARGS(&context.allocator));
        if (FAILED(result))
        {
            return PoolResult::failure(gpu_error("ID3D12Device.CreateCommandAllocator", result));
        }
        const std::wstring name = L"CueEngine Command Allocator " +
                                  std::to_wstring(static_cast<int>(a_type)) + L" " + std::to_wstring(index);
        const HRESULT nameResult = context.allocator->SetName(name.c_str());
        if (FAILED(nameResult))
        {
            return PoolResult::failure(gpu_error("ID3D12CommandAllocator.SetName", nameResult));
        }
        auto queryResult = context.initialize_queries(*a_device.device());
        if (!queryResult.has_value())
        {
            return PoolResult::failure(*queryResult.try_error());
        }
    }
    auto listsResult = pool->recreate_lists(a_device);
    if (!listsResult.has_value())
    {
        return PoolResult::failure(*listsResult.try_error());
    }
    return PoolResult::success(std::move(pool));
}

/// @brief 対応する Fence 完了後だけ Context を貸し出す
Result<CommandLease> DX12CommandPool::acquire(DX12GpuCommandQueue& a_queue, UINT a_slot)
{
    if (a_queue.type() != m_type || a_queue.device() != m_device ||
        (m_boundQueue && m_boundQueue != &a_queue) || a_slot >= k_backBufferCount ||
        !m_contexts[a_slot].list ||
        m_contexts[a_slot].status != ContextStatus::Idle)
    {
        return Result<CommandLease>::failure({ErrorCategory::InvalidState, "DX12CommandPool.acquire"});
    }
    m_boundQueue = &a_queue;
    return reset_context(a_queue, a_slot);
}

/// @brief 完了済み Context を再利用し、GPU 実行中なら追加の Context を作る
Result<CommandLease> DX12CommandPool::acquire_batch(DX12GpuCommandQueue& a_queue, UINT a_slot)
{
    if (a_queue.type() != m_type || a_queue.device() != m_device ||
        (m_boundQueue && m_boundQueue != &a_queue) || a_slot >= k_backBufferCount)
    {
        return Result<CommandLease>::failure({ErrorCategory::InvalidArgument, "DX12CommandPool.acquire_batch"});
    }
    m_boundQueue = &a_queue;
    for (UINT index = 0; index < m_contexts.size(); ++index)
    {
        const auto& context = m_contexts[index];
        if (context.slot == a_slot && context.list && context.status == ContextStatus::Idle &&
            a_queue.is_complete(context.fenceValue))
        {
            return reset_context(a_queue, index);
        }
    }
    auto appendResult = append_context(a_slot);
    if (!appendResult.has_value())
    {
        return Result<CommandLease>::failure(*appendResult.try_error());
    }
    return reset_context(a_queue, static_cast<UINT>(m_contexts.size() - 1));
}

/// @brief Graph Executor が貸した List に一致する記録中 Context を返す
ICommandContext* DX12CommandPool::find_recording_context(ID3D12GraphicsCommandList* a_list) noexcept
{
    for (auto& context : m_contexts)
    {
        if (context.status == ContextStatus::Recording && context.list.Get() == a_list)
        {
            return &context;
        }
    }
    return nullptr;
}

/// @brief Slot と結び付く全 Context の Fence を待って CPU 書込みを安全にする
Result<void> DX12CommandPool::wait_for_slot(DX12GpuCommandQueue& a_queue, UINT a_slot) const
{
    if (a_queue.type() != m_type || a_queue.device() != m_device ||
        (m_boundQueue && m_boundQueue != &a_queue) || a_slot >= k_backBufferCount)
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "DX12CommandPool.wait_for_slot"});
    }
    for (const auto& context : m_contexts)
    {
        if (context.slot != a_slot)
        {
            continue;
        }
        if (context.status != ContextStatus::Idle)
        {
            return Result<void>::failure({ErrorCategory::InvalidState, "DX12CommandPool.slotBusy"});
        }
        if (context.fenceValue != 0)
        {
            auto result = a_queue.wait_for(context.fenceValue);
            if (!result.has_value())
            {
                return result;
            }
        }
    }
    return Result<void>::success();
}

/// @brief 対応する Fence の完了後に Allocator と List を再利用する
Result<CommandLease> DX12CommandPool::reset_context(DX12GpuCommandQueue& a_queue, UINT a_index)
{
    using LeaseResult = Result<CommandLease>;
    auto& context = m_contexts[a_index];
    if (context.fenceValue != 0)
    {
        auto waitResult = a_queue.wait_for(context.fenceValue);
        if (!waitResult.has_value())
        {
            return LeaseResult::failure(*waitResult.try_error());
        }
    }
    context.fenceValue = 0;
    context.m_queue = nullptr;
    HRESULT result = context.allocator->Reset();
    if (FAILED(result))
    {
        return LeaseResult::failure(gpu_error("ID3D12CommandAllocator.Reset", result));
    }
    result = context.list->Reset(context.allocator.Get(), nullptr);
    if (FAILED(result))
    {
        return LeaseResult::failure(gpu_error("ID3D12GraphicsCommandList.Reset", result));
    }
    context.status = ContextStatus::Recording;
    ++context.generation;
    if (context.generation == 0)
    {
        ++context.generation;
    }
    return LeaseResult::success({context.slot, a_index, context.generation, context.list.Get()});
}

/// @brief 一つの Pass に専有する Allocator と List を追加する
Result<void> DX12CommandPool::append_context(UINT a_slot)
{
    const auto listType = m_type == GpuQueueType::Graphics ? D3D12_COMMAND_LIST_TYPE_DIRECT
                          : m_type == GpuQueueType::Compute ? D3D12_COMMAND_LIST_TYPE_COMPUTE
                                                            : D3D12_COMMAND_LIST_TYPE_COPY;
    DX12GpuCommandContext context;
    context.slot = a_slot;
    context.m_type = m_type;
    HRESULT result = m_device->CreateCommandAllocator(listType, IID_PPV_ARGS(&context.allocator));
    if (FAILED(result))
    {
        return Result<void>::failure(gpu_error("ID3D12Device.CreateCommandAllocator.batch", result));
    }
    result = m_device->CreateCommandList(0, listType, context.allocator.Get(), nullptr,
                                         IID_PPV_ARGS(&context.list));
    if (FAILED(result))
    {
        return Result<void>::failure(gpu_error("ID3D12Device.CreateCommandList.batch", result));
    }
    result = context.list->Close();
    if (FAILED(result))
    {
        return Result<void>::failure(gpu_error("ID3D12GraphicsCommandList.Close.batch", result));
    }
    auto queryResult = context.initialize_queries(*m_device);
    if (!queryResult.has_value())
    {
        return queryResult;
    }
    m_contexts.push_back(std::move(context));
    return Result<void>::success();
}

/// @brief 記録済み Context を閉じて対応 Queue へ投入する
Result<void> DX12CommandPool::submit(DX12GpuCommandQueue& a_queue, CommandLease a_lease)
{
    if (&a_queue != m_boundQueue || !is_lease(a_lease, ContextStatus::Recording))
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "DX12CommandPool.submit"});
    }
    auto& context = m_contexts[a_lease.contextIndex];
    const HRESULT result = context.list->Close();
    if (FAILED(result))
    {
        return Result<void>::failure(gpu_error("ID3D12GraphicsCommandList.Close", result));
    }
    ID3D12CommandList* lists[] = {context.list.Get()};
    a_queue.queue()->ExecuteCommandLists(1, lists);
    context.status = ContextStatus::Submitted;
    m_hasPendingGpu = true;
    return Result<void>::success();
}

/// @brief Submit 前の記録失敗時に貸出 List を閉じる
Result<void> DX12CommandPool::abort(CommandLease a_lease)
{
    if (!is_lease(a_lease, ContextStatus::Recording))
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "DX12CommandPool.abort"});
    }
    auto& context = m_contexts[a_lease.contextIndex];
    const HRESULT result = context.list->Close();
    context.status = ContextStatus::Idle;
    if (FAILED(result))
    {
        // Submit していない List だけを破棄し、失敗状態を次の貸出へ持ち越さない
        context.list.Reset();
        return Result<void>::failure(gpu_error("ID3D12GraphicsCommandList.Close.abort", result));
    }
    return Result<void>::success();
}

/// @brief Submit 後の Queue 位置を Context の再利用条件として記録する
Result<void> DX12CommandPool::retire(DX12GpuCommandQueue& a_queue, CommandLease a_lease)
{
    auto result = retire_fence(a_queue, a_lease);
    if (!result.has_value())
    {
        return Result<void>::failure(*result.try_error());
    }
    return Result<void>::success();
}

/// @brief Queue 位置を記録し、依存先 Queue が待つ Fence 値を返す
Result<std::uint64_t> DX12CommandPool::retire_fence(DX12GpuCommandQueue& a_queue, CommandLease a_lease)
{
    if (&a_queue != m_boundQueue || !is_lease(a_lease, ContextStatus::Submitted))
    {
        return Result<std::uint64_t>::failure({ErrorCategory::InvalidState, "DX12CommandPool.retire"});
    }
    auto valueResult = a_queue.signal();
    if (!valueResult.has_value())
    {
        return Result<std::uint64_t>::failure(*valueResult.try_error());
    }
    auto& context = m_contexts[a_lease.contextIndex];
    context.fenceValue = valueResult.take_value();
    context.m_queue = &a_queue;
    context.status = ContextStatus::Idle;
    return Result<std::uint64_t>::success(context.fenceValue);
}

/// @brief GPU 完了後に旧 Back Buffer を参照し得る List を全て解放する
void DX12CommandPool::release_for_resize() noexcept
{
    for (auto& context : m_contexts)
    {
        context.list.Reset();
    }
    mark_idle();
}

/// @brief Resize 後に Context の Command List を再生成する
Result<void> DX12CommandPool::recreate_lists(DX12RenderDevice& a_device)
{
    const auto listType = m_type == GpuQueueType::Graphics ? D3D12_COMMAND_LIST_TYPE_DIRECT
                          : m_type == GpuQueueType::Compute ? D3D12_COMMAND_LIST_TYPE_COMPUTE
                                                            : D3D12_COMMAND_LIST_TYPE_COPY;
    for (UINT index = 0; index < m_contexts.size(); ++index)
    {
        auto& context = m_contexts[index];
        const HRESULT resetResult = context.allocator->Reset();
        if (FAILED(resetResult))
        {
            return Result<void>::failure(gpu_error("ID3D12CommandAllocator.Reset.resize", resetResult));
        }
        HRESULT result = a_device.device()->CreateCommandList(0, listType,
                                                                context.allocator.Get(), nullptr,
                                                                IID_PPV_ARGS(&context.list));
        if (FAILED(result))
        {
            return Result<void>::failure(gpu_error("ID3D12Device.CreateCommandList", result));
        }
        const std::wstring name = L"CueEngine Command List " +
                                  std::to_wstring(static_cast<int>(m_type)) + L" " + std::to_wstring(index);
        result = context.list->SetName(name.c_str());
        if (FAILED(result))
        {
            return Result<void>::failure(gpu_error("ID3D12GraphicsCommandList.SetName", result));
        }
        result = context.list->Close();
        if (FAILED(result))
        {
            return Result<void>::failure(gpu_error("ID3D12GraphicsCommandList.Close", result));
        }
    }
    return Result<void>::success();
}

/// @brief Submit 後の Queue 完了がまだ確認されていないか返す
bool DX12CommandPool::has_pending_gpu() const noexcept
{
    return m_hasPendingGpu;
}

/// @brief Queue 全体の完了後に Fence 条件を解除する
void DX12CommandPool::mark_idle() noexcept
{
    for (auto& context : m_contexts)
    {
        context.fenceValue = 0;
        context.m_queue = nullptr;
        context.status = ContextStatus::Idle;
    }
    m_hasPendingGpu = false;
}

/// @brief Slot と借用 List が現在の貸出状態に一致するか確認する
bool DX12CommandPool::is_lease(CommandLease a_lease, ContextStatus a_status) const noexcept
{
    return a_lease.slot < k_backBufferCount && a_lease.contextIndex < m_contexts.size() &&
           m_contexts[a_lease.contextIndex].slot == a_lease.slot &&
           a_lease.generation == m_contexts[a_lease.contextIndex].generation &&
           a_lease.list == m_contexts[a_lease.contextIndex].list.Get() &&
           m_contexts[a_lease.contextIndex].status == a_status;
}

/// @brief Pool、Index、世代から今の Native List を再構成する
Result<CommandLease> DX12CommandPool::native_lease(CommandContextLease a_lease) const
{
    if (a_lease.owner != this || a_lease.slot >= k_backBufferCount ||
        a_lease.contextIndex >= m_contexts.size())
    {
        return Result<CommandLease>::failure({ErrorCategory::InvalidState, "DX12CommandPool.native_lease"});
    }
    const auto& context = m_contexts[a_lease.contextIndex];
    if (context.slot != a_lease.slot || context.generation != a_lease.generation || !context.list)
    {
        return Result<CommandLease>::failure({ErrorCategory::InvalidState, "DX12CommandPool.native_lease"});
    }
    return Result<CommandLease>::success({a_lease.slot, a_lease.contextIndex, a_lease.generation,
                                           context.list.Get()});
}
} // namespace cue::detail
