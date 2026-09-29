#include "D3D12CommandPool.h"

#include <string>
#include <utility>

namespace cue::detail
{
/// @brief Back Buffer 数の指定 Queue 用 Context を生成する
Result<std::unique_ptr<D3D12CommandPool>> D3D12CommandPool::create(D3D12DeviceContext& a_device,
                                                                     GpuQueueType a_type)
{
    using PoolResult = Result<std::unique_ptr<D3D12CommandPool>>;
    if (a_type != GpuQueueType::Graphics && a_type != GpuQueueType::Compute &&
        a_type != GpuQueueType::Copy)
    {
        return PoolResult::failure({ErrorCategory::InvalidArgument, "D3D12CommandPool.create.type"});
    }
    const auto listType = a_type == GpuQueueType::Graphics ? D3D12_COMMAND_LIST_TYPE_DIRECT
                          : a_type == GpuQueueType::Compute ? D3D12_COMMAND_LIST_TYPE_COMPUTE
                                                            : D3D12_COMMAND_LIST_TYPE_COPY;
    auto pool = std::make_unique<D3D12CommandPool>();
    pool->m_type = a_type;
    pool->m_device = a_device.device();
    pool->m_contexts.resize(k_backBufferCount);
    for (UINT index = 0; index < k_backBufferCount; ++index)
    {
        auto& context = pool->m_contexts[index];
        context.slot = index;
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
    }
    auto listsResult = pool->recreate_lists(a_device);
    if (!listsResult.has_value())
    {
        return PoolResult::failure(*listsResult.try_error());
    }
    return PoolResult::success(std::move(pool));
}

/// @brief 対応する Fence 完了後だけ Context を貸し出す
Result<CommandLease> D3D12CommandPool::acquire(D3D12QueueContext& a_queue, UINT a_slot)
{
    if (a_queue.type() != m_type || a_slot >= k_backBufferCount || !m_contexts[a_slot].list ||
        m_contexts[a_slot].status != ContextStatus::Idle)
    {
        return Result<CommandLease>::failure({ErrorCategory::InvalidState, "D3D12CommandPool.acquire"});
    }
    return reset_context(a_queue, a_slot);
}

/// @brief 完了済み Context を再利用し、GPU 実行中なら追加の Context を作る
Result<CommandLease> D3D12CommandPool::acquire_batch(D3D12QueueContext& a_queue, UINT a_slot)
{
    if (a_queue.type() != m_type || a_slot >= k_backBufferCount)
    {
        return Result<CommandLease>::failure({ErrorCategory::InvalidArgument, "D3D12CommandPool.acquire_batch"});
    }
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

/// @brief Slot と結び付く全 Context の Fence を待って CPU 書込みを安全にする
Result<void> D3D12CommandPool::wait_for_slot(D3D12QueueContext& a_queue, UINT a_slot) const
{
    if (a_queue.type() != m_type || a_slot >= k_backBufferCount)
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "D3D12CommandPool.wait_for_slot"});
    }
    for (const auto& context : m_contexts)
    {
        if (context.slot != a_slot)
        {
            continue;
        }
        if (context.status != ContextStatus::Idle)
        {
            return Result<void>::failure({ErrorCategory::InvalidState, "D3D12CommandPool.slotBusy"});
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
Result<CommandLease> D3D12CommandPool::reset_context(D3D12QueueContext& a_queue, UINT a_index)
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
Result<void> D3D12CommandPool::append_context(UINT a_slot)
{
    const auto listType = m_type == GpuQueueType::Graphics ? D3D12_COMMAND_LIST_TYPE_DIRECT
                          : m_type == GpuQueueType::Compute ? D3D12_COMMAND_LIST_TYPE_COMPUTE
                                                            : D3D12_COMMAND_LIST_TYPE_COPY;
    FrameContext context;
    context.slot = a_slot;
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
    m_contexts.push_back(std::move(context));
    return Result<void>::success();
}

/// @brief 記録済み Context を閉じて対応 Queue へ投入する
Result<void> D3D12CommandPool::submit(D3D12QueueContext& a_queue, CommandLease a_lease)
{
    if (a_queue.type() != m_type || !is_lease(a_lease, ContextStatus::Recording))
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "D3D12CommandPool.submit"});
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
Result<void> D3D12CommandPool::abort(CommandLease a_lease)
{
    if (!is_lease(a_lease, ContextStatus::Recording))
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "D3D12CommandPool.abort"});
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
Result<void> D3D12CommandPool::retire(D3D12QueueContext& a_queue, CommandLease a_lease)
{
    auto result = retire_fence(a_queue, a_lease);
    if (!result.has_value())
    {
        return Result<void>::failure(*result.try_error());
    }
    return Result<void>::success();
}

/// @brief Queue 位置を記録し、依存先 Queue が待つ Fence 値を返す
Result<std::uint64_t> D3D12CommandPool::retire_fence(D3D12QueueContext& a_queue, CommandLease a_lease)
{
    if (a_queue.type() != m_type || !is_lease(a_lease, ContextStatus::Submitted))
    {
        return Result<std::uint64_t>::failure({ErrorCategory::InvalidState, "D3D12CommandPool.retire"});
    }
    auto valueResult = a_queue.signal();
    if (!valueResult.has_value())
    {
        return Result<std::uint64_t>::failure(*valueResult.try_error());
    }
    auto& context = m_contexts[a_lease.contextIndex];
    context.fenceValue = valueResult.take_value();
    context.status = ContextStatus::Idle;
    return Result<std::uint64_t>::success(context.fenceValue);
}

/// @brief GPU 完了後に旧 Back Buffer を参照し得る List を全て解放する
void D3D12CommandPool::release_for_resize() noexcept
{
    for (auto& context : m_contexts)
    {
        context.list.Reset();
    }
    mark_idle();
}

/// @brief Resize 後に Context の Command List を再生成する
Result<void> D3D12CommandPool::recreate_lists(D3D12DeviceContext& a_device)
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
bool D3D12CommandPool::has_pending_gpu() const noexcept
{
    return m_hasPendingGpu;
}

/// @brief Queue 全体の完了後に Fence 条件を解除する
void D3D12CommandPool::mark_idle() noexcept
{
    for (auto& context : m_contexts)
    {
        context.fenceValue = 0;
        context.status = ContextStatus::Idle;
    }
    m_hasPendingGpu = false;
}

/// @brief Slot と借用 List が現在の貸出状態に一致するか確認する
bool D3D12CommandPool::is_lease(CommandLease a_lease, ContextStatus a_status) const noexcept
{
    return a_lease.slot < k_backBufferCount && a_lease.contextIndex < m_contexts.size() &&
           m_contexts[a_lease.contextIndex].slot == a_lease.slot &&
           a_lease.generation == m_contexts[a_lease.contextIndex].generation &&
           a_lease.list == m_contexts[a_lease.contextIndex].list.Get() &&
           m_contexts[a_lease.contextIndex].status == a_status;
}
} // namespace cue::detail
