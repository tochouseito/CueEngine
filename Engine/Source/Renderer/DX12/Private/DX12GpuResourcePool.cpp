#include <DX12/DX12GpuResourcePool.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <utility>
#include <vector>

#include <d3d12.h>
#include <wrl/client.h>

#include <DX12/DX12GpuResource.h>
#include <DX12/DX12PlacedResourceAllocator.h>
#include <DX12/DX12RenderDevice.h>
#include <Platform/Diagnostics.h>

namespace cue::dx12
{
namespace
{
std::atomic<std::uint64_t> g_nextResourcePoolId = 1;

/// @brief Pool ごとに再利用しない識別子を発行する
Result<std::uint64_t> next_pool_id()
{
    std::uint64_t id = g_nextResourcePoolId.load(std::memory_order_relaxed);
    while (id != (std::numeric_limits<std::uint64_t>::max)() &&
           !g_nextResourcePoolId.compare_exchange_weak(id, id + 1, std::memory_order_relaxed))
    {
    }
    if (id == (std::numeric_limits<std::uint64_t>::max)())
    {
        return Result<std::uint64_t>::failure({ErrorCategory::Fatal, "DX12GpuResourcePool.id_exhausted"});
    }
    return Result<std::uint64_t>::success(id);
}
} // namespace

/// @brief Pool 破棄後も Lease と GPU Completion の寿命を維持する
struct DX12GpuResourcePool::State final
{
    struct PendingUse final
    {
        GpuResourceAccess access;
        std::shared_ptr<ICommandCompletion> completion;
    };

    struct Slot final
    {
        std::unique_ptr<DX12GpuResource> resource;
        std::vector<PendingUse> pending;
        std::uint64_t generation = 1;
        std::size_t activeReaders = 0;
        bool hasActiveWriter = false;
        bool isRetired = false;
    };

    /// @brief 未確認の GPU 作業を待ち、失敗時は未完了 Resource の解放を防ぐ
    ~State()
    {
        for (Slot& slot : slots)
        {
            for (PendingUse& use : slot.pending)
            {
                auto result = use.completion->wait();
                if (!result.has_value())
                {
                    report_error("DX12GpuResourcePool", *result.try_error(), DiagnosticSeverity::Fatal);
                    std::terminate();
                }
            }
        }
    }

    /// @brief Handle の Pool、世代、貸出状態を Mutex 保持中に確認する
    [[nodiscard]] bool is_current_locked(GpuResourceHandle a_handle) const noexcept
    {
        return a_handle.is_valid() && a_handle.poolId == poolId && a_handle.index < slots.size() &&
               slots[a_handle.index].resource && !slots[a_handle.index].isRetired &&
               slots[a_handle.index].generation == a_handle.generation;
    }

    /// @brief 完了済みの使用を除き、破棄予約が安全になった Slot を解放する
    [[nodiscard]] bool collect_slot_locked(std::uint32_t a_index)
    {
        Slot& slot = slots[a_index];
        if (!slot.resource)
        {
            return false;
        }
        std::erase_if(slot.pending, [](const PendingUse& a_use) { return a_use.completion->is_complete(); });
        if (!slot.isRetired || slot.activeReaders != 0 || slot.hasActiveWriter || !slot.pending.empty())
        {
            return false;
        }
        slot.resource.reset();
        if (slot.generation != (std::numeric_limits<std::uint64_t>::max)())
        {
            freeSlots.push_back(a_index);
        }
        return true;
    }

    std::mutex mutex;
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    std::vector<Slot> slots;
    std::vector<std::uint32_t> freeSlots;
    std::uint64_t poolId = 0;
    bool isClosed = false;
};

/// @brief 借用期間に Resource を保持し、提出済み Completion を Slot へ登録する
class DX12GpuResourcePool::Lease final : public IGpuResourceLease
{
public:
    /// @brief Pool の共有状態と借用中 Resource を保持する
    Lease(std::shared_ptr<State> a_state, std::uint32_t a_index, DX12GpuResource* a_resource,
          GpuResourceAccess a_access) noexcept
        : m_state(std::move(a_state)), m_index(a_index), m_resource(a_resource), m_access(a_access)
    {
    }

    /// @brief 借用を返し、破棄予約済み Slot の回収を試みる
    ~Lease() override
    {
        std::lock_guard lock(m_state->mutex);
        State::Slot& slot = m_state->slots[m_index];
        if (m_access == GpuResourceAccess::Read)
        {
            --slot.activeReaders;
        }
        else
        {
            slot.hasActiveWriter = false;
        }
        static_cast<void>(m_state->collect_slot_locked(m_index));
    }

    /// @brief 借用中の Resource を非所有で返す
    [[nodiscard]] IGpuResource* resource() const noexcept override
    {
        return m_resource;
    }

    /// @brief GPU 提出の完了点を一度だけ記録し、未完了なら後続の競合 Access を拒否する
    [[nodiscard]] Result<void> mark_submitted(std::shared_ptr<ICommandCompletion> a_completion) override
    {
        if (!a_completion || m_isSubmitted)
        {
            return Result<void>::failure({ErrorCategory::InvalidArgument, "DX12GpuResourceLease.mark_submitted"});
        }
        std::lock_guard lock(m_state->mutex);
        State::Slot& slot = m_state->slots[m_index];
        // acquire 時に active な Lease 数まで容量を確保済みなので提出後に再配置しない
        slot.pending.push_back({m_access, std::move(a_completion)});
        m_isSubmitted = true;
        return Result<void>::success();
    }

private:
    std::shared_ptr<State> m_state;
    std::uint32_t m_index;
    DX12GpuResource* m_resource;
    GpuResourceAccess m_access;
    bool m_isSubmitted = false;
};

/// @brief create の内部でのみ空の Pool を構築する
DX12GpuResourcePool::DX12GpuResourcePool(CreateToken) noexcept
{
}

/// @brief Device の参照と Pool 固有 ID を持つ共有状態を作る
Result<std::unique_ptr<DX12GpuResourcePool>> DX12GpuResourcePool::create(DX12RenderDevice& a_device)
{
    using PoolResult = Result<std::unique_ptr<DX12GpuResourcePool>>;
    if (!a_device.device())
    {
        return PoolResult::failure({ErrorCategory::InvalidArgument, "DX12GpuResourcePool.create.device"});
    }
    auto idResult = next_pool_id();
    if (!idResult.has_value())
    {
        return PoolResult::failure(*idResult.try_error());
    }
    try
    {
        auto pool = std::make_unique<DX12GpuResourcePool>(CreateToken{});
        pool->m_state = std::make_shared<State>();
        pool->m_state->device = a_device.device();
        pool->m_state->poolId = idResult.take_value();
        auto allocatorResult = DX12PlacedResourceAllocator::create(a_device);
        if (!allocatorResult.has_value())
        {
            return PoolResult::failure(*allocatorResult.try_error());
        }
        pool->m_placedAllocator = allocatorResult.take_value();
        return PoolResult::success(std::move(pool));
    }
    catch (const std::bad_alloc&)
    {
        return PoolResult::failure({ErrorCategory::PlatformFailure, "DX12GpuResourcePool.create.allocation"});
    }
}

/// @brief Pool 停止を試み、返却待ち Lease があれば共有状態を維持する
DX12GpuResourcePool::~DX12GpuResourcePool()
{
    auto result = shutdown();
    if (!result.has_value())
    {
        report_error("DX12GpuResourcePool.shutdown", *result.try_error(), DiagnosticSeverity::Error);
    }
}

/// @brief Buffer を作成して Pool の Slot に収める
Result<GpuResourceHandle> DX12GpuResourcePool::create_buffer(GpuBufferDesc a_desc)
{
    {
        std::lock_guard lock(m_state->mutex);
        if (m_state->isClosed)
        {
            return Result<GpuResourceHandle>::failure({ErrorCategory::InvalidState,
                                                       "DX12GpuResourcePool.create_buffer.closed"});
        }
    }
    try
    {
        auto resourceResult = DX12GpuResource::create_buffer(*m_state->device.Get(), a_desc);
        if (!resourceResult.has_value())
        {
            return Result<GpuResourceHandle>::failure(*resourceResult.try_error());
        }
        return insert_resource(resourceResult.take_value());
    }
    catch (const std::bad_alloc&)
    {
        return Result<GpuResourceHandle>::failure({ErrorCategory::PlatformFailure,
                                                   "DX12GpuResourcePool.create_buffer.allocation"});
    }
}

/// @brief Texture を作成して Pool の Slot に収める
Result<GpuResourceHandle> DX12GpuResourcePool::create_texture2d(GpuTexture2DDesc a_desc)
{
    {
        std::lock_guard lock(m_state->mutex);
        if (m_state->isClosed)
        {
            return Result<GpuResourceHandle>::failure({ErrorCategory::InvalidState,
                                                       "DX12GpuResourcePool.create_texture2d.closed"});
        }
    }
    try
    {
        auto resourceResult = DX12GpuResource::create_texture2d(*m_state->device.Get(), a_desc);
        if (!resourceResult.has_value())
        {
            return Result<GpuResourceHandle>::failure(*resourceResult.try_error());
        }
        return insert_resource(resourceResult.take_value());
    }
    catch (const std::bad_alloc&)
    {
        return Result<GpuResourceHandle>::failure({ErrorCategory::PlatformFailure,
                                                   "DX12GpuResourcePool.create_texture2d.allocation"});
    }
}

/// @brief FrameGraph 用 Default Buffer を Placed Heap に作る
Result<GpuResourceHandle> DX12GpuResourcePool::create_transient_buffer(GpuBufferDesc a_desc)
{
    {
        std::lock_guard lock(m_state->mutex);
        if (m_state->isClosed)
        {
            return Result<GpuResourceHandle>::failure({ErrorCategory::InvalidState,
                                                       "DX12GpuResourcePool.create_transient_buffer.closed"});
        }
    }
    auto resourceResult = m_placedAllocator->create_buffer(a_desc);
    if (!resourceResult.has_value())
    {
        return Result<GpuResourceHandle>::failure(*resourceResult.try_error());
    }
    return insert_resource(resourceResult.take_value());
}

/// @brief FrameGraph 用 Texture を Placed Heap に作る
Result<GpuResourceHandle> DX12GpuResourcePool::create_transient_texture2d(GpuTexture2DDesc a_desc)
{
    {
        std::lock_guard lock(m_state->mutex);
        if (m_state->isClosed)
        {
            return Result<GpuResourceHandle>::failure({ErrorCategory::InvalidState,
                                                       "DX12GpuResourcePool.create_transient_texture2d.closed"});
        }
    }
    auto resourceResult = m_placedAllocator->create_texture2d(a_desc);
    if (!resourceResult.has_value())
    {
        return Result<GpuResourceHandle>::failure(*resourceResult.try_error());
    }
    return insert_resource(resourceResult.take_value());
}

/// @brief 同じ Placed 領域の Buffer 群を失敗時にまとめて Rollback する
Result<std::vector<GpuResourceHandle>> DX12GpuResourcePool::create_alias_buffers(
    std::span<const GpuBufferDesc> a_descs)
{
    {
        std::lock_guard lock(m_state->mutex);
        if (m_state->isClosed)
        {
            return Result<std::vector<GpuResourceHandle>>::failure(
                {ErrorCategory::InvalidState, "DX12GpuResourcePool.create_alias_buffers.closed"});
        }
    }
    auto result = m_placedAllocator->create_alias_buffers(a_descs);
    if (!result.has_value())
    {
        return Result<std::vector<GpuResourceHandle>>::failure(*result.try_error());
    }
    return insert_resources(result.take_value());
}

/// @brief 同じ Placed 領域の Texture 群を失敗時にまとめて Rollback する
Result<std::vector<GpuResourceHandle>> DX12GpuResourcePool::create_alias_texture2ds(
    std::span<const GpuTexture2DDesc> a_descs)
{
    {
        std::lock_guard lock(m_state->mutex);
        if (m_state->isClosed)
        {
            return Result<std::vector<GpuResourceHandle>>::failure(
                {ErrorCategory::InvalidState, "DX12GpuResourcePool.create_alias_texture2ds.closed"});
        }
    }
    auto result = m_placedAllocator->create_alias_texture2ds(a_descs);
    if (!result.has_value())
    {
        return Result<std::vector<GpuResourceHandle>>::failure(*result.try_error());
    }
    return insert_resources(result.take_value());
}

/// @brief 世代を検証し、GPU 利用と CPU 記録の競合を防ぐ Lease を貸す
Result<gpuResourceLease> DX12GpuResourcePool::acquire(GpuResourceHandle a_handle, GpuResourceAccess a_access)
{
    using LeaseResult = Result<gpuResourceLease>;
    if (a_access != GpuResourceAccess::Read && a_access != GpuResourceAccess::Write)
    {
        return LeaseResult::failure({ErrorCategory::InvalidArgument, "DX12GpuResourcePool.acquire.access"});
    }
    std::lock_guard lock(m_state->mutex);
    if (m_state->isClosed)
    {
        return LeaseResult::failure({ErrorCategory::InvalidState, "DX12GpuResourcePool.acquire.closed"});
    }
    if (!m_state->is_current_locked(a_handle))
    {
        return LeaseResult::failure({ErrorCategory::InvalidArgument, "DX12GpuResourcePool.acquire.handle"});
    }
    State::Slot& slot = m_state->slots[a_handle.index];
    static_cast<void>(m_state->collect_slot_locked(a_handle.index));
    if (slot.hasActiveWriter || (a_access == GpuResourceAccess::Write && slot.activeReaders != 0))
    {
        return LeaseResult::failure({ErrorCategory::InvalidState, "DX12GpuResourcePool.acquire.active_conflict"});
    }
    for (const State::PendingUse& use : slot.pending)
    {
        if (a_access == GpuResourceAccess::Write || use.access == GpuResourceAccess::Write)
        {
            return LeaseResult::failure({ErrorCategory::InvalidState, "DX12GpuResourcePool.acquire.gpu_conflict"});
        }
    }

    try
    {
        // 同時借用の全員が提出後に一件ずつ追加できる容量を、提出前に確保する
        slot.pending.reserve(slot.pending.size() + slot.activeReaders + (slot.hasActiveWriter ? 1 : 0) + 1);
        auto lease = std::make_unique<Lease>(m_state, a_handle.index, slot.resource.get(), a_access);
        if (a_access == GpuResourceAccess::Read)
        {
            ++slot.activeReaders;
        }
        else
        {
            slot.hasActiveWriter = true;
        }
        return LeaseResult::success(std::move(lease));
    }
    catch (const std::bad_alloc&)
    {
        return LeaseResult::failure({ErrorCategory::PlatformFailure, "DX12GpuResourcePool.acquire.allocation"});
    }
}

/// @brief Handle を無効化し、借用と GPU 参照の両方が終わるまで実体を保つ
Result<void> DX12GpuResourcePool::retire(GpuResourceHandle a_handle)
{
    std::lock_guard lock(m_state->mutex);
    if (m_state->isClosed || !m_state->is_current_locked(a_handle))
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "DX12GpuResourcePool.retire.handle"});
    }
    State::Slot& slot = m_state->slots[a_handle.index];
    slot.isRetired = true;
    if (slot.generation < (std::numeric_limits<std::uint64_t>::max)())
    {
        ++slot.generation;
    }
    static_cast<void>(m_state->collect_slot_locked(a_handle.index));
    return Result<void>::success();
}

/// @brief 全 Slot の完了済み使用を除き、安全な破棄予約を回収する
Result<std::size_t> DX12GpuResourcePool::collect()
{
    std::lock_guard lock(m_state->mutex);
    std::size_t released = 0;
    for (std::size_t index = 0; index < m_state->slots.size(); ++index)
    {
        released += m_state->collect_slot_locked(static_cast<std::uint32_t>(index)) ? 1 : 0;
    }
    return Result<std::size_t>::success(released);
}

/// @brief 新規利用を止め、GPU 完了を確認した後にすべての Resource を解放する
Result<void> DX12GpuResourcePool::shutdown()
{
    if (!m_state)
    {
        return Result<void>::success();
    }
    std::lock_guard lock(m_state->mutex);
    m_state->isClosed = true;
    for (const State::Slot& slot : m_state->slots)
    {
        if (slot.activeReaders != 0 || slot.hasActiveWriter)
        {
            return Result<void>::failure({ErrorCategory::InvalidState, "DX12GpuResourcePool.shutdown.active_lease"});
        }
    }
    for (State::Slot& slot : m_state->slots)
    {
        for (State::PendingUse& use : slot.pending)
        {
            auto result = use.completion->wait();
            if (!result.has_value())
            {
                return result;
            }
        }
    }
    m_state->slots.clear();
    m_state->freeSlots.clear();
    return Result<void>::success();
}

/// @brief 生成した Resource を再利用可能な Slot または新規 Slot に収める
Result<GpuResourceHandle> DX12GpuResourcePool::insert_resource(std::unique_ptr<DX12GpuResource> a_resource)
{
    std::lock_guard lock(m_state->mutex);
    if (m_state->isClosed)
    {
        return Result<GpuResourceHandle>::failure({ErrorCategory::InvalidState,
                                                   "DX12GpuResourcePool.insert_resource.closed"});
    }
    std::uint32_t index = 0;
    if (!m_state->freeSlots.empty())
    {
        index = m_state->freeSlots.back();
        m_state->freeSlots.pop_back();
    }
    else
    {
        if (m_state->slots.size() >= (std::numeric_limits<std::uint32_t>::max)())
        {
            return Result<GpuResourceHandle>::failure({ErrorCategory::Fatal,
                                                       "DX12GpuResourcePool.insert_resource.slot_exhausted"});
        }
        index = static_cast<std::uint32_t>(m_state->slots.size());
        try
        {
            // Lease の破棄中の push_back を無例外に保ち、容量は段階的に増やす
            const std::size_t required = m_state->slots.size() + 1;
            if (m_state->freeSlots.capacity() < required)
            {
                const std::size_t doubled = m_state->freeSlots.capacity() == 0
                                                ? 1
                                                : m_state->freeSlots.capacity() * 2;
                m_state->freeSlots.reserve(doubled > required ? doubled : required);
            }
            m_state->slots.emplace_back();
        }
        catch (const std::bad_alloc&)
        {
            return Result<GpuResourceHandle>::failure({ErrorCategory::PlatformFailure,
                                                       "DX12GpuResourcePool.insert_resource.allocation"});
        }
    }
    State::Slot& slot = m_state->slots[index];
    slot.resource = std::move(a_resource);
    slot.isRetired = false;
    return Result<GpuResourceHandle>::success({index, slot.generation, m_state->poolId});
}

/// @brief Group 内の一部だけが Pool に残らないよう Handle をまとめて登録する
Result<std::vector<GpuResourceHandle>> DX12GpuResourcePool::insert_resources(
    std::vector<std::unique_ptr<DX12GpuResource>> a_resources)
{
    using HandlesResult = Result<std::vector<GpuResourceHandle>>;
    std::vector<GpuResourceHandle> handles;
    try
    {
        handles.reserve(a_resources.size());
    }
    catch (const std::bad_alloc&)
    {
        return HandlesResult::failure({ErrorCategory::PlatformFailure,
                                       "DX12GpuResourcePool.insert_resources.allocation"});
    }
    for (auto& resource : a_resources)
    {
        auto result = insert_resource(std::move(resource));
        if (!result.has_value())
        {
            const auto error = *result.try_error();
            for (const auto handle : handles)
            {
                auto retireResult = retire(handle);
                if (!retireResult.has_value())
                {
                    return HandlesResult::failure(*retireResult.try_error());
                }
            }
            auto collectResult = collect();
            if (!collectResult.has_value())
            {
                return HandlesResult::failure(*collectResult.try_error());
            }
            return HandlesResult::failure(error);
        }
        handles.push_back(result.take_value());
    }
    return HandlesResult::success(std::move(handles));
}
} // namespace cue::dx12
