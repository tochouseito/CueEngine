#include <DX12/DX12FullscreenPipeline.h>

#include <cstddef>
#include <cstdint>
#include <vector>

#include <DX12/DX12CommandPool.h>
#include <DX12/DX12DescriptorAllocator.h>
#include <DX12/DX12GpuResource.h>
#include <DX12/DX12QueuePool.h>
#include <DX12/DX12RenderDevice.h>

namespace
{
/// @brief Texture 全体を指定 State へ遷移させる
void transition(ID3D12GraphicsCommandList& a_list, ID3D12Resource& a_resource,
                D3D12_RESOURCE_STATES a_before, D3D12_RESOURCE_STATES a_after)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = &a_resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = a_before;
    barrier.Transition.StateAfter = a_after;
    a_list.ResourceBarrier(1, &barrier);
}

/// @brief WARP で SRV から RTV への全画面描画を画素で確認する
int run_tests()
{
    auto deviceResult = cue::dx12::DX12RenderDevice::create(cue::dx12::AdapterSelection::Warp);
    if (!deviceResult.has_value())
    {
        return 1;
    }
    auto device = deviceResult.take_value();
    if (cue::dx12::DX12FullscreenPipeline::create(*device, DXGI_FORMAT_UNKNOWN).has_value())
    {
        return 2;
    }
    auto pipelineResult = cue::dx12::DX12FullscreenPipeline::create(*device, DXGI_FORMAT_R8G8B8A8_UNORM);
    if (!pipelineResult.has_value())
    {
        return 3;
    }
    auto pipeline = pipelineResult.take_value();

    cue::GpuTexture2DDesc sourceDesc{4, 4};
    sourceDesc.isRenderTarget = true;
    sourceDesc.clearColor = {1.0f, 0.0f, 0.0f, 1.0f};
    cue::GpuTexture2DDesc targetDesc{4, 4};
    targetDesc.isRenderTarget = true;
    auto sourceResult = cue::dx12::DX12GpuResource::create_texture2d(*device->device(), sourceDesc);
    auto targetResult = cue::dx12::DX12GpuResource::create_texture2d(*device->device(), targetDesc);
    auto rtvResult = cue::dx12::DX12DescriptorAllocator::create(
        *device->device(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2);
    auto srvResult = cue::dx12::DX12DescriptorAllocator::create(
        *device->device(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, true);
    if (!sourceResult.has_value() || !targetResult.has_value() ||
        !rtvResult.has_value() || !srvResult.has_value())
    {
        return 4;
    }
    auto source = sourceResult.take_value();
    auto target = targetResult.take_value();
    auto rtvAllocator = rtvResult.take_value();
    auto srvAllocator = srvResult.take_value();
    auto sourceRtvResult = rtvAllocator->allocate();
    auto targetRtvResult = rtvAllocator->allocate();
    auto sourceSrvResult = srvAllocator->allocate();
    if (!sourceRtvResult.has_value() || !targetRtvResult.has_value() || !sourceSrvResult.has_value())
    {
        return 5;
    }
    auto sourceRtv = rtvAllocator->cpu_handle(sourceRtvResult.take_value());
    auto targetRtv = rtvAllocator->cpu_handle(targetRtvResult.take_value());
    const auto sourceSrvSlot = sourceSrvResult.take_value();
    auto sourceSrvCpu = srvAllocator->cpu_handle(sourceSrvSlot);
    auto sourceSrvGpu = srvAllocator->gpu_handle(sourceSrvSlot);
    if (!sourceRtv.has_value() || !targetRtv.has_value() ||
        !sourceSrvCpu.has_value() || !sourceSrvGpu.has_value())
    {
        return 6;
    }
    device->device()->CreateRenderTargetView(source->resource(), nullptr, *sourceRtv.try_value());
    device->device()->CreateRenderTargetView(target->resource(), nullptr, *targetRtv.try_value());
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    device->device()->CreateShaderResourceView(source->resource(), &srvDesc, *sourceSrvCpu.try_value());

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
    UINT64 totalBytes = 0;
    const auto textureDesc = target->resource()->GetDesc();
    device->device()->GetCopyableFootprints(&textureDesc, 0, 1, 0, &layout, nullptr, nullptr, &totalBytes);
    auto readbackResult = cue::dx12::DX12GpuResource::create_buffer(
        *device->device(), {totalBytes, cue::GpuMemoryUsage::Readback});
    auto queuePoolResult = cue::dx12::DX12QueuePool::create(*device);
    auto commandPoolResult = cue::dx12::DX12CommandPool::create(*device);
    if (!readbackResult.has_value() || !queuePoolResult.has_value() || !commandPoolResult.has_value())
    {
        return 7;
    }
    auto readback = readbackResult.take_value();
    auto queuePool = queuePoolResult.take_value();
    auto commandPool = commandPoolResult.take_value();
    auto queueResult = queuePool->acquire(cue::QueueType::Graphics);
    auto commandResult = commandPool->acquire(cue::QueueType::Graphics);
    if (!queueResult.has_value() || !commandResult.has_value())
    {
        return 8;
    }
    auto queue = queueResult.take_value();
    auto command = commandResult.take_value();
    auto* context = dynamic_cast<cue::dx12::DX12GpuCommandContext*>(command.get());
    if (!context || !context->command_list())
    {
        return 9;
    }
    auto* list = context->command_list();
    transition(*list, *source->resource(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
    list->ClearRenderTargetView(*sourceRtv.try_value(), sourceDesc.clearColor.data(), 0, nullptr);
    transition(*list, *source->resource(), D3D12_RESOURCE_STATE_RENDER_TARGET,
               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    transition(*list, *target->resource(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
    if (pipeline->draw(*list, *srvAllocator->heap(), *sourceSrvGpu.try_value(),
                       *targetRtv.try_value(), 0, 4).has_value() ||
        !pipeline->draw(*list, *srvAllocator->heap(), *sourceSrvGpu.try_value(),
                        *targetRtv.try_value(), 4, 4).has_value())
    {
        return 10;
    }
    transition(*list, *target->resource(), D3D12_RESOURCE_STATE_RENDER_TARGET,
               D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION copySource{};
    copySource.pResource = target->resource();
    copySource.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION copyDestination{};
    copyDestination.pResource = readback->resource();
    copyDestination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    copyDestination.PlacedFootprint = layout;
    list->CopyTextureRegion(&copyDestination, 0, 0, 0, &copySource, nullptr);
    if (!command->close().has_value())
    {
        return 11;
    }
    auto submitResult = commandPool->submit(*queue, *command);
    if (!submitResult.has_value())
    {
        return 12;
    }
    auto completion = submitResult.take_value();
    if (!completion->wait().has_value())
    {
        return 13;
    }
    std::vector<std::byte> pixels(static_cast<std::size_t>(totalBytes));
    if (!readback->read(0, pixels).has_value())
    {
        return 14;
    }
    const auto is_red = [&pixels, &layout](std::size_t a_x, std::size_t a_y) {
        const auto offset = a_y * layout.Footprint.RowPitch + a_x * 4;
        return std::to_integer<int>(pixels[offset]) == 255 &&
               std::to_integer<int>(pixels[offset + 1]) == 0 &&
               std::to_integer<int>(pixels[offset + 2]) == 0 &&
               std::to_integer<int>(pixels[offset + 3]) == 255;
    };
    if (!is_red(0, 0) || !is_red(3, 0) || !is_red(0, 3) || !is_red(3, 3))
    {
        return 15;
    }
    command.reset();
    queue.reset();
    if (!commandPool->shutdown().has_value() || !queuePool->shutdown().has_value())
    {
        return 16;
    }
    return 0;
}
} // namespace

/// @brief DXC Shader と全画面 Triangle の WARP 描画を検証する
int main()
{
    return run_tests();
}
