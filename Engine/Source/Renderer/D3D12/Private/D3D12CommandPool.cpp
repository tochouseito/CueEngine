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
    for (UINT index = 0; index < k_backBufferCount; ++index)
    {
        auto& context = pool->m_contexts[index];
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
    using LeaseResult = Result<CommandLease>;
    if (a_queue.type() != m_type || a_slot >= k_backBufferCount || !m_contexts[a_slot].list ||
        m_contexts[a_slot].status != ContextStatus::Idle)
    {
        return LeaseResult::failure({ErrorCategory::InvalidState, "D3D12CommandPool.acquire"});
    }
    auto& context = m_contexts[a_slot];
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
    return LeaseResult::success({a_slot, context.generation, context.list.Get()});
}

/// @brief 記録済み Context を閉じて対応 Queue へ投入する
Result<void> D3D12CommandPool::submit(D3D12QueueContext& a_queue, CommandLease a_lease)
{
    if (a_queue.type() != m_type || !is_lease(a_lease, ContextStatus::Recording))
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "D3D12CommandPool.submit"});
    }
    auto& context = m_contexts[a_lease.slot];
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
    auto& context = m_contexts[a_lease.slot];
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

/// @brief Present 後の Queue 位置を Context の再利用条件として記録する
Result<void> D3D12CommandPool::retire(D3D12QueueContext& a_queue, CommandLease a_lease)
{
    if (a_queue.type() != m_type || !is_lease(a_lease, ContextStatus::Submitted))
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "D3D12CommandPool.retire"});
    }
    auto valueResult = a_queue.signal();
    if (!valueResult.has_value())
    {
        return Result<void>::failure(*valueResult.try_error());
    }
    auto& context = m_contexts[a_lease.slot];
    context.fenceValue = valueResult.take_value();
    context.status = ContextStatus::Idle;
    return Result<void>::success();
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
    for (UINT index = 0; index < k_backBufferCount; ++index)
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
    return a_lease.slot < k_backBufferCount && a_lease.generation == m_contexts[a_lease.slot].generation &&
           a_lease.list == m_contexts[a_lease.slot].list.Get() &&
           m_contexts[a_lease.slot].status == a_status;
}
} // namespace cue::detail
