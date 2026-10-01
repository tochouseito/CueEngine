#include <DX12/DX12PlacedResourceAllocator.h>

#include <array>
#include <cstdint>
#include <memory>
#include <span>

#include <DX12/DX12CommandPool.h>
#include <DX12/DX12GpuResource.h>
#include <DX12/DX12QueuePool.h>
#include <DX12/DX12RenderDevice.h>

/// @brief WARP で Heap の非重複配置、再利用、初回 Activation と GPU コピーを検証する
int main()
{
    auto deviceResult = cue::dx12::DX12RenderDevice::create(cue::dx12::AdapterSelection::Warp);
    if (!deviceResult.has_value())
    {
        return 1;
    }
    auto device = deviceResult.take_value();
    if (cue::dx12::DX12PlacedResourceAllocator::create(*device, 0).has_value())
    {
        return 2;
    }
    auto allocatorResult = cue::dx12::DX12PlacedResourceAllocator::create(*device, 256 * 1024);
    if (!allocatorResult.has_value())
    {
        return 3;
    }
    auto allocator = allocatorResult.take_value();
    if (allocator->create_buffer({64, cue::GpuMemoryUsage::Upload}).has_value() ||
        allocator->create_texture2d({0, 4}).has_value())
    {
        return 4;
    }
    auto firstResult = allocator->create_buffer({64, cue::GpuMemoryUsage::Default});
    auto secondResult = allocator->create_buffer({64, cue::GpuMemoryUsage::Default});
    auto textureResult = allocator->create_texture2d({4, 4});
    if (!firstResult.has_value() || !secondResult.has_value() || !textureResult.has_value())
    {
        return 5;
    }
    auto first = firstResult.take_value();
    auto second = secondResult.take_value();
    auto texture = textureResult.take_value();
    const D3D12_RESOURCE_DESC bufferDesc = first->resource()->GetDesc();
    const auto bufferInfo = device->device()->GetResourceAllocationInfo(0, 1, &bufferDesc);
    const D3D12_RESOURCE_DESC textureDesc = texture->resource()->GetDesc();
    const auto textureInfo = device->device()->GetResourceAllocationInfo(0, 1, &textureDesc);
    if (!first->is_placed() || !second->is_placed() || !texture->is_placed() ||
        first->placement_heap() != second->placement_heap() ||
        first->placement_heap() == texture->placement_heap() ||
        first->placement_offset() % bufferInfo.Alignment != 0 ||
        texture->placement_offset() % textureInfo.Alignment != 0 ||
        first->placement_offset() + bufferInfo.SizeInBytes > second->placement_offset())
    {
        return 6;
    }
    const auto beforeRelease = allocator->stats();
    if (beforeRelease.heapCount != 2 ||
        beforeRelease.occupiedBytes != bufferInfo.SizeInBytes * 2 + textureInfo.SizeInBytes)
    {
        return 7;
    }
    ID3D12Heap* firstHeap = first->placement_heap();
    const std::uint64_t firstOffset = first->placement_offset();
    first.reset();
    auto reusedResult = allocator->create_buffer({64, cue::GpuMemoryUsage::Default});
    if (!reusedResult.has_value())
    {
        return 8;
    }
    auto reused = reusedResult.take_value();
    if (reused->placement_heap() != firstHeap || reused->placement_offset() != firstOffset)
    {
        return 9;
    }

    constexpr std::array<std::uint32_t, 4> k_source = {0x12345678, 0x9abcdef0, 0x13572468, 0xdeadbeef};
    auto uploadResult = cue::dx12::DX12GpuResource::create_buffer(
        *device->device(), {sizeof(k_source), cue::GpuMemoryUsage::Upload});
    auto readbackResult = cue::dx12::DX12GpuResource::create_buffer(
        *device->device(), {sizeof(k_source), cue::GpuMemoryUsage::Readback});
    if (!uploadResult.has_value() || !readbackResult.has_value())
    {
        return 10;
    }
    auto upload = uploadResult.take_value();
    auto readback = readbackResult.take_value();
    if (upload->is_placed() || readback->is_placed() ||
        !upload->write(0, std::as_bytes(std::span{k_source})).has_value())
    {
        return 11;
    }
    auto queuePoolResult = cue::dx12::DX12QueuePool::create(*device);
    auto commandPoolResult = cue::dx12::DX12CommandPool::create(*device);
    if (!queuePoolResult.has_value() || !commandPoolResult.has_value())
    {
        return 12;
    }
    auto queuePool = queuePoolResult.take_value();
    auto commandPool = commandPoolResult.take_value();
    auto queueResult = queuePool->acquire(cue::QueueType::Graphics);
    auto commandResult = commandPool->acquire(cue::QueueType::Graphics);
    if (!queueResult.has_value() || !commandResult.has_value())
    {
        return 13;
    }
    auto queue = queueResult.take_value();
    auto command = commandResult.take_value();
    auto* native = dynamic_cast<cue::dx12::DX12GpuCommandContext*>(command.get());
    if (!native || !native->command_list())
    {
        return 14;
    }
    // Placed Resource は初期状態が inactive なので、初回使用前に明示的に有効化する
    D3D12_RESOURCE_BARRIER activation{};
    activation.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
    activation.Aliasing.pResourceAfter = reused->resource();
    native->command_list()->ResourceBarrier(1, &activation);
    D3D12_RESOURCE_BARRIER transition{};
    transition.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    transition.Transition.pResource = reused->resource();
    transition.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    transition.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    transition.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    native->command_list()->ResourceBarrier(1, &transition);
    native->command_list()->CopyBufferRegion(reused->resource(), 0, upload->resource(), 0, sizeof(k_source));
    transition.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    transition.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    native->command_list()->ResourceBarrier(1, &transition);
    native->command_list()->CopyBufferRegion(readback->resource(), 0, reused->resource(), 0, sizeof(k_source));
    if (!command->close().has_value())
    {
        return 15;
    }
    auto submittedResult = commandPool->submit(*queue, *command);
    if (!submittedResult.has_value())
    {
        return 16;
    }
    auto completion = submittedResult.take_value();
    if (!completion->wait().has_value())
    {
        return 17;
    }
    std::array<std::uint32_t, k_source.size()> output{};
    if (!readback->read(0, std::as_writable_bytes(std::span{output})).has_value() || output != k_source)
    {
        return 18;
    }
    command.reset();
    queue.reset();
    if (!commandPool->shutdown().has_value() || !queuePool->shutdown().has_value())
    {
        return 19;
    }

    // 満杯なら同じ種類の Heap を増やし、空になった余分な Page を解放する
    auto thirdResult = allocator->create_buffer({64, cue::GpuMemoryUsage::Default});
    auto fourthResult = allocator->create_buffer({64, cue::GpuMemoryUsage::Default});
    auto fifthResult = allocator->create_buffer({64, cue::GpuMemoryUsage::Default});
    if (!thirdResult.has_value() || !fourthResult.has_value() || !fifthResult.has_value())
    {
        return 21;
    }
    auto third = thirdResult.take_value();
    auto fourth = fourthResult.take_value();
    auto fifth = fifthResult.take_value();
    if (bufferInfo.SizeInBytes * 4 == 256 * 1024 &&
        (allocator->stats().heapCount != 3 || fifth->placement_heap() == firstHeap))
    {
        return 22;
    }
    fifth.reset();
    if (bufferInfo.SizeInBytes * 4 == 256 * 1024 && allocator->stats().heapCount != 2)
    {
        return 23;
    }

    // Alias Group は最後の Native Resource が消えるまで共有領域を返さない
    auto aliasAllocatorResult = cue::dx12::DX12PlacedResourceAllocator::create(*device, 256 * 1024);
    if (!aliasAllocatorResult.has_value())
    {
        return 24;
    }
    auto aliasAllocator = aliasAllocatorResult.take_value();
    constexpr std::array<cue::GpuBufferDesc, 2> k_aliasBuffers = {{{64}, {128}}};
    constexpr std::array<cue::GpuTexture2DDesc, 2> k_aliasTextures = {{{4, 4}, {16, 16}}};
    if (aliasAllocator->create_alias_buffers({}).has_value() ||
        aliasAllocator->create_alias_texture2ds({}).has_value())
    {
        return 25;
    }
    auto aliasBuffersResult = aliasAllocator->create_alias_buffers(k_aliasBuffers);
    auto aliasTexturesResult = aliasAllocator->create_alias_texture2ds(k_aliasTextures);
    if (!aliasBuffersResult.has_value() || !aliasTexturesResult.has_value())
    {
        return 26;
    }
    auto aliasBuffers = aliasBuffersResult.take_value();
    auto aliasTextures = aliasTexturesResult.take_value();
    if (aliasBuffers[0]->placement_heap() != aliasBuffers[1]->placement_heap() ||
        aliasBuffers[0]->placement_offset() != aliasBuffers[1]->placement_offset() ||
        aliasTextures[0]->placement_heap() != aliasTextures[1]->placement_heap() ||
        aliasTextures[0]->placement_offset() != aliasTextures[1]->placement_offset() ||
        aliasBuffers[0]->placement_heap() == aliasTextures[0]->placement_heap())
    {
        return 27;
    }
    ID3D12Heap* aliasHeap = aliasBuffers[0]->placement_heap();
    const std::uint64_t aliasOffset = aliasBuffers[0]->placement_offset();
    aliasBuffers[0].reset();
    auto blockedReuseResult = aliasAllocator->create_buffer({64});
    if (!blockedReuseResult.has_value())
    {
        return 28;
    }
    auto blockedReuse = blockedReuseResult.take_value();
    if (blockedReuse->placement_heap() == aliasHeap && blockedReuse->placement_offset() == aliasOffset)
    {
        return 29;
    }
    aliasBuffers[1].reset();
    auto afterAliasResult = aliasAllocator->create_buffer({64});
    if (!afterAliasResult.has_value())
    {
        return 30;
    }
    auto afterAlias = afterAliasResult.take_value();
    if (afterAlias->placement_heap() != aliasHeap || afterAlias->placement_offset() != aliasOffset)
    {
        return 31;
    }

    // Allocator 本体を先に破棄しても、Resource が Heap を最後まで保持する
    allocator.reset();
    if (!second->resource() || !texture->resource() || !reused->placement_heap())
    {
        return 20;
    }
    reused.reset();
    second.reset();
    texture.reset();
    return 0;
}
