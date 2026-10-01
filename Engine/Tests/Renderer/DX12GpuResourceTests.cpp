#include <DX12/DX12GpuResource.h>

#include <array>
#include <cstdint>
#include <memory>
#include <span>

#include <DX12/DX12CommandPool.h>
#include <DX12/DX12QueuePool.h>
#include <DX12/DX12RenderDevice.h>

/// @brief WARP で Upload、Default、Readback 間の GPU コピーと Texture 生成を検証する
int main()
{
    auto deviceResult = cue::dx12::DX12RenderDevice::create(cue::dx12::AdapterSelection::Warp);
    if (!deviceResult.has_value())
    {
        return 1;
    }
    auto device = deviceResult.take_value();
    if (cue::dx12::DX12GpuResource::create_buffer(*device->device(), {0, cue::GpuMemoryUsage::Upload})
            .has_value() ||
        cue::dx12::DX12GpuResource::create_texture2d(*device->device(), {0, 1}).has_value() ||
        cue::dx12::DX12GpuResource::create_texture2d(*device->device(), {4, 4, 4}).has_value() ||
        cue::dx12::DX12GpuResource::create_texture2d(
            *device->device(), {1, 1, 1, static_cast<cue::GpuTextureFormat>(255)})
            .has_value())
    {
        return 2;
    }

    constexpr std::array<std::uint32_t, 4> k_source = {0x12345678, 0x9abcdef0, 0x13572468, 0xdeadbeef};
    constexpr std::uint64_t k_bufferSize = sizeof(k_source);
    auto uploadResult = cue::dx12::DX12GpuResource::create_buffer(
        *device->device(), {k_bufferSize, cue::GpuMemoryUsage::Upload}, L"CueEngine Resource Test Upload");
    auto gpuResult = cue::dx12::DX12GpuResource::create_buffer(
        *device->device(), {k_bufferSize, cue::GpuMemoryUsage::Default}, L"CueEngine Resource Test Default");
    auto readbackResult = cue::dx12::DX12GpuResource::create_buffer(
        *device->device(), {k_bufferSize, cue::GpuMemoryUsage::Readback}, L"CueEngine Resource Test Readback");
    auto textureResult = cue::dx12::DX12GpuResource::create_texture2d(
        *device->device(), {4, 4, 1, cue::GpuTextureFormat::Rgba8Unorm}, L"CueEngine Resource Test Texture");
    if (!uploadResult.has_value() || !gpuResult.has_value() || !readbackResult.has_value() ||
        !textureResult.has_value())
    {
        return 3;
    }
    auto upload = uploadResult.take_value();
    auto gpu = gpuResult.take_value();
    auto readback = readbackResult.take_value();
    auto texture = textureResult.take_value();
    if (texture->kind() != cue::GpuResourceKind::Texture2D ||
        texture->memory_usage() != cue::GpuMemoryUsage::Default ||
        texture->resource()->GetDesc().Format != DXGI_FORMAT_R8G8B8A8_UNORM ||
        upload->buffer_size() != k_bufferSize || texture->buffer_size() != 0)
    {
        return 4;
    }

    const auto sourceBytes = std::as_bytes(std::span{k_source});
    if (!upload->write(0, sourceBytes).has_value() ||
        upload->write(1, sourceBytes).has_value() ||
        gpu->write(0, sourceBytes).has_value() ||
        texture->write(0, sourceBytes).has_value())
    {
        return 5;
    }

    auto queuePoolResult = cue::dx12::DX12QueuePool::create(*device);
    auto commandPoolResult = cue::dx12::DX12CommandPool::create(*device);
    if (!queuePoolResult.has_value() || !commandPoolResult.has_value())
    {
        return 6;
    }
    auto queuePool = queuePoolResult.take_value();
    auto commandPool = commandPoolResult.take_value();
    auto queueResult = queuePool->acquire(cue::QueueType::Graphics);
    auto commandResult = commandPool->acquire(cue::QueueType::Graphics);
    if (!queueResult.has_value() || !commandResult.has_value())
    {
        return 7;
    }
    auto queue = queueResult.take_value();
    auto command = commandResult.take_value();
    auto* native = dynamic_cast<cue::dx12::DX12GpuCommandContext*>(command.get());
    if (!native || !native->command_list())
    {
        return 8;
    }

    // Default Buffer の初期 COMMON State を明示的にコピー先へ遷移させる
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = gpu->resource();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    native->command_list()->ResourceBarrier(1, &barrier);
    native->command_list()->CopyBufferRegion(gpu->resource(), 0, upload->resource(), 0, k_bufferSize);
    // 同じ List で Default Buffer を読み戻すため、コピー元へ遷移させる
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    native->command_list()->ResourceBarrier(1, &barrier);
    native->command_list()->CopyBufferRegion(readback->resource(), 0, gpu->resource(), 0, k_bufferSize);

    if (!command->close().has_value())
    {
        return 9;
    }
    auto completionResult = commandPool->submit(*queue, *command);
    if (!completionResult.has_value())
    {
        return 10;
    }
    auto completion = completionResult.take_value();
    if (!completion->wait().has_value())
    {
        return 11;
    }
    std::array<std::uint32_t, k_source.size()> output{};
    auto outputBytes = std::as_writable_bytes(std::span{output});
    if (!readback->read(0, outputBytes).has_value() || output != k_source ||
        readback->read(1, outputBytes).has_value() || upload->read(0, outputBytes).has_value())
    {
        return 12;
    }

    command.reset();
    queue.reset();
    if (!commandPool->shutdown().has_value() || !queuePool->shutdown().has_value())
    {
        return 13;
    }
    return 0;
}
