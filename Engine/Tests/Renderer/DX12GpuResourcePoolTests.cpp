#include <DX12/DX12GpuResourcePool.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <span>

#include <DX12/DX12CommandPool.h>
#include <DX12/DX12GpuResource.h>
#include <DX12/DX12QueuePool.h>
#include <DX12/DX12RenderDevice.h>

namespace
{
/// @brief 完了前後の破棄予約を再現する制御可能な Completion
class ManualCompletion final : public cue::ICommandCompletion
{
public:
    /// @brief GPU 完了を模した状態を切り替える
    void complete() noexcept
    {
        m_isComplete.store(true);
    }

    /// @brief Test 用 Completion の Queue 種類を返す
    [[nodiscard]] cue::QueueType type() const noexcept override
    {
        return cue::QueueType::Graphics;
    }

    /// @brief 外部から設定した完了状態を返す
    [[nodiscard]] bool is_complete() const noexcept override
    {
        return m_isComplete.load();
    }

    /// @brief 未完了なら Test の誤った停止順を失敗として返す
    [[nodiscard]] cue::Result<void> wait() override
    {
        if (!is_complete())
        {
            return cue::Result<void>::failure({cue::ErrorCategory::InvalidState, "ManualCompletion.wait"});
        }
        return cue::Result<void>::success();
    }

private:
    std::atomic<bool> m_isComplete = false;
};
} // namespace

/// @brief 世代付き借用、並列読み取り、遅延破棄と WARP での実コピーを検証する
int main()
{
    auto deviceResult = cue::dx12::DX12RenderDevice::create(cue::dx12::AdapterSelection::Warp);
    if (!deviceResult.has_value())
    {
        return 1;
    }
    auto device = deviceResult.take_value();
    auto poolResult = cue::dx12::DX12GpuResourcePool::create(*device);
    auto foreignPoolResult = cue::dx12::DX12GpuResourcePool::create(*device);
    if (!poolResult.has_value() || !foreignPoolResult.has_value())
    {
        return 2;
    }
    auto pool = poolResult.take_value();
    auto foreignPool = foreignPoolResult.take_value();

    auto sharedResult = pool->create_buffer({64, cue::GpuMemoryUsage::Default});
    if (!sharedResult.has_value())
    {
        return 3;
    }
    const cue::GpuResourceHandle shared = sharedResult.take_value();
    if (!shared.is_valid() || foreignPool->acquire(shared, cue::GpuResourceAccess::Read).has_value())
    {
        return 4;
    }
    auto firstResult = pool->acquire(shared, cue::GpuResourceAccess::Read);
    auto secondResult = pool->acquire(shared, cue::GpuResourceAccess::Read);
    if (!firstResult.has_value() || !secondResult.has_value() ||
        pool->acquire(shared, cue::GpuResourceAccess::Write).has_value())
    {
        return 5;
    }
    auto first = firstResult.take_value();
    auto second = secondResult.take_value();
    auto firstCompletion = std::make_shared<ManualCompletion>();
    auto secondCompletion = std::make_shared<ManualCompletion>();
    if (!first->mark_submitted(firstCompletion).has_value() ||
        !second->mark_submitted(secondCompletion).has_value() ||
        first->mark_submitted(firstCompletion).has_value())
    {
        return 6;
    }
    first.reset();
    second.reset();

    // GPU 上の読み取り同士は並行でき、書き込みは全読み取りの完了を待つ
    auto parallelReadResult = pool->acquire(shared, cue::GpuResourceAccess::Read);
    if (!parallelReadResult.has_value() || pool->acquire(shared, cue::GpuResourceAccess::Write).has_value())
    {
        return 7;
    }
    auto parallelRead = parallelReadResult.take_value();
    parallelRead.reset();
    if (!pool->retire(shared).has_value() || pool->acquire(shared, cue::GpuResourceAccess::Read).has_value() ||
        pool->retire(shared).has_value())
    {
        return 8;
    }
    auto firstCollect = pool->collect();
    if (!firstCollect.has_value() || *firstCollect.try_value() != 0)
    {
        return 8;
    }
    firstCompletion->complete();
    auto secondCollect = pool->collect();
    if (!secondCollect.has_value() || *secondCollect.try_value() != 0)
    {
        return 9;
    }
    secondCompletion->complete();
    auto finalCollect = pool->collect();
    if (!finalCollect.has_value() || *finalCollect.try_value() != 1)
    {
        return 10;
    }
    auto reusedResult = pool->create_buffer({64, cue::GpuMemoryUsage::Default});
    if (!reusedResult.has_value())
    {
        return 11;
    }
    const cue::GpuResourceHandle reused = reusedResult.take_value();
    if (reused.index != shared.index || reused.generation == shared.generation ||
        pool->acquire(shared, cue::GpuResourceAccess::Read).has_value())
    {
        return 12;
    }
    auto writerResult = pool->acquire(reused, cue::GpuResourceAccess::Write);
    if (!writerResult.has_value() || pool->acquire(reused, cue::GpuResourceAccess::Read).has_value())
    {
        return 13;
    }
    auto writer = writerResult.take_value();
    auto* persistentNative = dynamic_cast<cue::dx12::DX12GpuResource*>(writer->resource());
    if (!persistentNative || persistentNative->is_placed())
    {
        return 49;
    }
    auto writerCompletion = std::make_shared<ManualCompletion>();
    if (!writer->mark_submitted(writerCompletion).has_value())
    {
        return 14;
    }
    writer.reset();
    if (pool->acquire(reused, cue::GpuResourceAccess::Read).has_value())
    {
        return 15;
    }
    writerCompletion->complete();
    auto afterWriteResult = pool->acquire(reused, cue::GpuResourceAccess::Read);
    if (!afterWriteResult.has_value())
    {
        return 16;
    }
    auto afterWrite = afterWriteResult.take_value();
    afterWrite.reset();
    if (!pool->retire(reused).has_value())
    {
        return 17;
    }

    // 未完了の Placed Resource の領域を先に再利用させない
    auto pendingHandleResult = pool->create_transient_buffer({64, cue::GpuMemoryUsage::Default});
    if (!pendingHandleResult.has_value() ||
        pool->create_transient_buffer({64, cue::GpuMemoryUsage::Upload}).has_value())
    {
        return 36;
    }
    const auto pendingHandle = pendingHandleResult.take_value();
    auto pendingLeaseResult = pool->acquire(pendingHandle, cue::GpuResourceAccess::Read);
    if (!pendingLeaseResult.has_value())
    {
        return 37;
    }
    auto pendingLease = pendingLeaseResult.take_value();
    auto* pendingNative = dynamic_cast<cue::dx12::DX12GpuResource*>(pendingLease->resource());
    if (!pendingNative || !pendingNative->is_placed())
    {
        return 38;
    }
    ID3D12Heap* pendingHeap = pendingNative->placement_heap();
    const std::uint64_t pendingOffset = pendingNative->placement_offset();
    auto pendingCompletion = std::make_shared<ManualCompletion>();
    if (!pendingLease->mark_submitted(pendingCompletion).has_value())
    {
        return 39;
    }
    pendingLease.reset();
    if (!pool->retire(pendingHandle).has_value())
    {
        return 40;
    }
    auto occupiedHandleResult = pool->create_transient_buffer({64, cue::GpuMemoryUsage::Default});
    if (!occupiedHandleResult.has_value())
    {
        return 41;
    }
    const auto occupiedHandle = occupiedHandleResult.take_value();
    auto occupiedLeaseResult = pool->acquire(occupiedHandle, cue::GpuResourceAccess::Read);
    if (!occupiedLeaseResult.has_value())
    {
        return 42;
    }
    auto occupiedLease = occupiedLeaseResult.take_value();
    auto* occupiedNative = dynamic_cast<cue::dx12::DX12GpuResource*>(occupiedLease->resource());
    if (!occupiedNative || (occupiedNative->placement_heap() == pendingHeap &&
                            occupiedNative->placement_offset() == pendingOffset))
    {
        return 43;
    }
    occupiedLease.reset();
    pendingCompletion->complete();
    auto pendingCollect = pool->collect();
    if (!pendingCollect.has_value() || *pendingCollect.try_value() != 1)
    {
        return 44;
    }
    auto reusedPlacedResult = pool->create_transient_buffer({64, cue::GpuMemoryUsage::Default});
    if (!reusedPlacedResult.has_value())
    {
        return 45;
    }
    const auto reusedPlacedHandle = reusedPlacedResult.take_value();
    auto reusedPlacedLeaseResult = pool->acquire(reusedPlacedHandle, cue::GpuResourceAccess::Read);
    if (!reusedPlacedLeaseResult.has_value())
    {
        return 46;
    }
    auto reusedPlacedLease = reusedPlacedLeaseResult.take_value();
    auto* reusedPlacedNative = dynamic_cast<cue::dx12::DX12GpuResource*>(reusedPlacedLease->resource());
    if (!reusedPlacedNative || reusedPlacedNative->placement_heap() != pendingHeap ||
        reusedPlacedNative->placement_offset() != pendingOffset)
    {
        return 47;
    }
    reusedPlacedLease.reset();
    if (!pool->retire(occupiedHandle).has_value() || !pool->retire(reusedPlacedHandle).has_value())
    {
        return 48;
    }

    constexpr std::array<std::uint32_t, 4> k_source = {0x12345678, 0x9abcdef0, 0x13572468, 0xdeadbeef};
    constexpr std::uint64_t k_size = sizeof(k_source);
    auto uploadHandleResult = pool->create_buffer({k_size, cue::GpuMemoryUsage::Upload});
    auto gpuHandleResult = pool->create_transient_buffer({k_size, cue::GpuMemoryUsage::Default});
    auto readbackHandleResult = pool->create_buffer({k_size, cue::GpuMemoryUsage::Readback});
    auto textureHandleResult = pool->create_transient_texture2d({4, 4});
    if (!uploadHandleResult.has_value() || !gpuHandleResult.has_value() ||
        !readbackHandleResult.has_value() || !textureHandleResult.has_value())
    {
        return 18;
    }
    const auto uploadHandle = uploadHandleResult.take_value();
    const auto gpuHandle = gpuHandleResult.take_value();
    const auto readbackHandle = readbackHandleResult.take_value();
    const auto textureHandle = textureHandleResult.take_value();
    auto uploadResult = pool->acquire(uploadHandle, cue::GpuResourceAccess::Write);
    auto gpuResult = pool->acquire(gpuHandle, cue::GpuResourceAccess::Write);
    auto readbackResult = pool->acquire(readbackHandle, cue::GpuResourceAccess::Write);
    if (!uploadResult.has_value() || !gpuResult.has_value() || !readbackResult.has_value())
    {
        return 19;
    }
    auto upload = uploadResult.take_value();
    auto gpu = gpuResult.take_value();
    auto readback = readbackResult.take_value();
    auto* uploadNative = dynamic_cast<cue::dx12::DX12GpuResource*>(upload->resource());
    auto* gpuNative = dynamic_cast<cue::dx12::DX12GpuResource*>(gpu->resource());
    auto* readbackNative = dynamic_cast<cue::dx12::DX12GpuResource*>(readback->resource());
    if (!uploadNative || !gpuNative || !readbackNative || !gpuNative->is_placed() ||
        uploadNative->is_placed() || readbackNative->is_placed() ||
        !uploadNative->write(0, std::as_bytes(std::span{k_source})).has_value())
    {
        return 20;
    }
    auto queuePoolResult = cue::dx12::DX12QueuePool::create(*device);
    auto commandPoolResult = cue::dx12::DX12CommandPool::create(*device);
    if (!queuePoolResult.has_value() || !commandPoolResult.has_value())
    {
        return 21;
    }
    auto queuePool = queuePoolResult.take_value();
    auto commandPool = commandPoolResult.take_value();
    auto queueResult = queuePool->acquire(cue::QueueType::Graphics);
    auto commandResult = commandPool->acquire(cue::QueueType::Graphics);
    if (!queueResult.has_value() || !commandResult.has_value())
    {
        return 22;
    }
    auto queue = queueResult.take_value();
    auto command = commandResult.take_value();
    auto* nativeCommand = dynamic_cast<cue::dx12::DX12GpuCommandContext*>(command.get());
    if (!nativeCommand || !nativeCommand->command_list())
    {
        return 23;
    }
    // Placed Buffer の初回使用前に simple model の Activation Barrier を記録する
    D3D12_RESOURCE_BARRIER activation{};
    activation.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
    activation.Aliasing.pResourceAfter = gpuNative->resource();
    nativeCommand->command_list()->ResourceBarrier(1, &activation);
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = gpuNative->resource();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    nativeCommand->command_list()->ResourceBarrier(1, &barrier);
    nativeCommand->command_list()->CopyBufferRegion(gpuNative->resource(), 0, uploadNative->resource(), 0, k_size);
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    nativeCommand->command_list()->ResourceBarrier(1, &barrier);
    nativeCommand->command_list()->CopyBufferRegion(readbackNative->resource(), 0, gpuNative->resource(), 0, k_size);
    if (!command->close().has_value())
    {
        return 24;
    }
    auto submittedResult = commandPool->submit(*queue, *command);
    if (!submittedResult.has_value())
    {
        return 25;
    }
    std::shared_ptr<cue::ICommandCompletion> completion(submittedResult.take_value());
    if (!upload->mark_submitted(completion).has_value() || !gpu->mark_submitted(completion).has_value() ||
        !readback->mark_submitted(completion).has_value())
    {
        return 26;
    }
    upload.reset();
    gpu.reset();
    readback.reset();
    if (!pool->retire(uploadHandle).has_value() || !pool->retire(gpuHandle).has_value())
    {
        return 27;
    }
    if (!completion->wait().has_value())
    {
        return 28;
    }
    auto outputResult = pool->acquire(readbackHandle, cue::GpuResourceAccess::Read);
    if (!outputResult.has_value())
    {
        return 29;
    }
    auto output = outputResult.take_value();
    auto* outputNative = dynamic_cast<cue::dx12::DX12GpuResource*>(output->resource());
    std::array<std::uint32_t, k_source.size()> copied{};
    if (!outputNative || !outputNative->read(0, std::as_writable_bytes(std::span{copied})).has_value() ||
        copied != k_source)
    {
        return 30;
    }
    output.reset();
    if (!pool->retire(readbackHandle).has_value() || !pool->retire(textureHandle).has_value())
    {
        return 31;
    }
    command.reset();
    queue.reset();
    // 破棄予約後も Lease を保持すれば Pool 停止は失敗し、Resource は借用側で生存する
    auto activeHandleResult = pool->create_buffer({32, cue::GpuMemoryUsage::Default});
    if (!activeHandleResult.has_value())
    {
        return 32;
    }
    const auto activeHandle = activeHandleResult.take_value();
    auto activeResult = pool->acquire(activeHandle, cue::GpuResourceAccess::Read);
    if (!activeResult.has_value())
    {
        return 33;
    }
    auto active = activeResult.take_value();
    auto activeCompletion = std::make_shared<ManualCompletion>();
    if (!pool->retire(activeHandle).has_value() ||
        !active->mark_submitted(activeCompletion).has_value() ||
        pool->shutdown().has_value() || active->resource() == nullptr)
    {
        return 34;
    }
    activeCompletion->complete();
    active.reset();
    if (!pool->shutdown().has_value() || !commandPool->shutdown().has_value() ||
        !queuePool->shutdown().has_value() || pool->create_buffer({64}).has_value() ||
        pool->acquire(readbackHandle, cue::GpuResourceAccess::Read).has_value() ||
        !pool->shutdown().has_value())
    {
        return 35;
    }
    return 0;
}
