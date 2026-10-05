#include <DX12/DX12FrameGraphFrames.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include <DX12/DX12CommandPool.h>
#include <DX12/DX12FrameGraphResources.h>
#include <DX12/DX12GpuResource.h>
#include <DX12/DX12QueuePool.h>
#include <DX12/DX12RenderDevice.h>
#include <DX12/DX12ViewManager.h>

namespace
{
/// @brief Native Texture の全体を単一 State へ遷移させる
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

/// @brief 外部 Texture の View が枠の準備時にだけ有効になることを WARP で確認する
int run_imported_view_tests()
{
    cue::GpuTexture2DDesc desc{4, 4};
    desc.isRenderTarget = true;
    desc.isShaderReadable = true;
    auto builderResult = cue::FrameGraphBuilder::create();
    if (!builderResult.has_value()) return 20;
    auto builder = builderResult.take_value();
    auto textureResult = builder->import_texture2d(
        desc, cue::FrameGraphResourceState::Common, cue::FrameGraphResourceState::Common);
    auto writeResult = builder->add_pass("WriteImported", cue::QueueType::Graphics);
    auto readResult = builder->add_pass("ReadImported", cue::QueueType::Graphics);
    if (!textureResult.has_value() || !writeResult.has_value() || !readResult.has_value()) return 21;
    const auto texture = textureResult.take_value();
    if (!builder->use(writeResult.take_value(), texture, cue::FrameGraphAccess::Write,
                      cue::FrameGraphResourceState::RenderTarget).has_value() ||
        !builder->use(readResult.take_value(), texture, cue::FrameGraphAccess::Read,
                      cue::FrameGraphResourceState::ShaderRead).has_value()) return 22;
    auto planResult = builder->build();
    if (!planResult.has_value()) return 23;
    auto plan = planResult.take_value();
    auto deviceResult = cue::dx12::DX12RenderDevice::create(cue::dx12::AdapterSelection::Warp);
    if (!deviceResult.has_value()) return 24;
    auto device = deviceResult.take_value();
    auto rtvResult = cue::dx12::DX12DescriptorAllocator::create(
        *device->device(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2);
    auto srvResult = cue::dx12::DX12DescriptorAllocator::create(
        *device->device(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2, true);
    if (!rtvResult.has_value() || !srvResult.has_value()) return 25;
    auto rtvAllocator = rtvResult.take_value();
    auto srvAllocator = srvResult.take_value();
    auto viewsResult = cue::dx12::DX12ViewManager::create(*device, *rtvAllocator, *srvAllocator);
    if (!viewsResult.has_value())
        return 30;
    auto views = viewsResult.take_value();
    const cue::dx12::DX12ResourceContext resources{*device, *views};
    // 容量 2 の Heap に 3 枠を依頼した途中失敗でも Slot を回収し、再生成できる
    if (cue::dx12::DX12FrameGraphFrames::create(resources, plan, {3, {}}).has_value())
        return 31;
    auto framesResult = cue::dx12::DX12FrameGraphFrames::create(resources, plan, {2, {}});
    auto nativeResult = cue::dx12::DX12GpuResource::create_texture2d(*device->device(), desc);
    cue::GpuTexture2DDesc wrongDesc{8, 8};
    wrongDesc.isRenderTarget = true;
    wrongDesc.isShaderReadable = true;
    auto wrongResult = cue::dx12::DX12GpuResource::create_texture2d(*device->device(), wrongDesc);
    if (!framesResult.has_value() || !nativeResult.has_value() || !wrongResult.has_value()) return 26;
    auto frames = framesResult.take_value();
    auto native = nativeResult.take_value();
    auto wrong = wrongResult.take_value();
    const std::array<cue::dx12::DX12FrameGraphExternalResource, 1> valid{{texture, native->resource()}};
    const std::array<cue::dx12::DX12FrameGraphExternalResource, 1> invalid{{texture, wrong->resource()}};
    if (frames->rtv(0, texture).has_value() || frames->srv(0, texture).has_value() ||
        !frames->begin_frame(0).has_value() ||
        frames->prepare_imported_views(0, plan, invalid).has_value() ||
        frames->rtv(0, texture).has_value() ||
        !frames->prepare_imported_views(0, plan, valid).has_value() ||
        frames->prepare_imported_views(0, plan, valid).has_value() ||
        !frames->rtv(0, texture).has_value() || !frames->srv(0, texture).has_value() ||
        !frames->begin_frame(1).has_value() ||
        !frames->prepare_imported_views(1, plan, valid).has_value() ||
        !frames->rtv(1, texture).has_value() || !frames->srv(1, texture).has_value()) return 27;
    auto firstSrvResult = frames->srv(0, texture);
    auto secondSrvResult = frames->srv(1, texture);
    if (!firstSrvResult.has_value() || !secondSrvResult.has_value()) return 28;
    const auto firstSrv = firstSrvResult.take_value();
    const auto secondSrv = secondSrvResult.take_value();
    if (firstSrv.ptr == secondSrv.ptr || !frames->begin_frame(0).has_value() ||
        frames->rtv(0, texture).has_value() || frames->srv(0, texture).has_value() ||
        !frames->shutdown().has_value()) return 29;
    return 0;
}

/// @brief WARP で枠ごとの RenderTarget と SRV、Clear、Readback を検証する
int run_tests()
{
    cue::GpuTexture2DDesc colorDesc{4, 4};
    colorDesc.clearColor = {0.25f, 0.5f, 0.75f, 1.0f};
    auto builderResult = cue::FrameGraphBuilder::create_main(colorDesc);
    if (!builderResult.has_value())
    {
        return 1;
    }
    auto builder = builderResult.take_value();
    const auto color = builder->final_color();
    auto clearResult = builder->add_pass("ClearFinalColor");
    auto copyResult = builder->add_pass("ReadbackFinalColor");
    if (!color.is_valid() || !clearResult.has_value() || !copyResult.has_value())
    {
        return 2;
    }
    const auto clearPass = clearResult.take_value();
    const auto copyPass = copyResult.take_value();
    if (!builder->use(clearPass, color, cue::FrameGraphAccess::Write,
                      cue::FrameGraphResourceState::RenderTarget).has_value() ||
        !builder->use(copyPass, color, cue::FrameGraphAccess::Read,
                      cue::FrameGraphResourceState::CopySource).has_value() ||
        !builder->depends_on(copyPass, clearPass).has_value())
    {
        return 3;
    }
    auto planResult = builder->build();
    if (!planResult.has_value() || planResult.try_value()->alias_slots().size() != 1)
    {
        return 4;
    }
    auto plan = planResult.take_value();
    auto deviceResult = cue::dx12::DX12RenderDevice::create(cue::dx12::AdapterSelection::Warp);
    if (!deviceResult.has_value())
    {
        return 5;
    }
    auto device = deviceResult.take_value();
    auto rtvResult = cue::dx12::DX12DescriptorAllocator::create(
        *device->device(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2);
    auto srvResult = cue::dx12::DX12DescriptorAllocator::create(
        *device->device(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2, true);
    if (!rtvResult.has_value() || !srvResult.has_value())
    {
        return 6;
    }
    auto rtvAllocator = rtvResult.take_value();
    auto srvAllocator = srvResult.take_value();
    auto viewsResult = cue::dx12::DX12ViewManager::create(*device, *rtvAllocator, *srvAllocator);
    if (!viewsResult.has_value())
        return 30;
    auto views = viewsResult.take_value();
    const cue::dx12::DX12ResourceContext resources{*device, *views};
    auto framesResult = cue::dx12::DX12FrameGraphFrames::create(resources, plan, {2, {}});
    if (!framesResult.has_value())
    {
        return 7;
    }
    auto frames = framesResult.take_value();
    auto* first = frames->resource(0, color);
    auto* second = frames->resource(1, color);
    if (frames->frame_count() != 2 || !first || !second ||
        first->resource() == second->resource() ||
        (first->resource()->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) == 0 ||
        !frames->rtv(0, color).has_value() || !frames->rtv(1, color).has_value() ||
        !frames->srv(0, color).has_value() || !frames->srv(1, color).has_value() ||
        frames->rtv(2, color).has_value() || frames->srv(2, color).has_value() ||
        !frames->begin_frame(0).has_value())
    {
        return 8;
    }

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
    UINT64 totalBytes = 0;
    const auto textureDesc = first->resource()->GetDesc();
    device->device()->GetCopyableFootprints(&textureDesc, 0, 1, 0, &layout, nullptr, nullptr, &totalBytes);
    auto readbackResult = cue::dx12::DX12GpuResource::create_buffer(
        *device->device(), {totalBytes, cue::GpuMemoryUsage::Readback});
    if (!readbackResult.has_value())
    {
        return 9;
    }
    auto readback = readbackResult.take_value();
    auto queuePoolResult = cue::dx12::DX12QueuePool::create(*device);
    auto commandPoolResult = cue::dx12::DX12CommandPool::create(*device);
    if (!queuePoolResult.has_value() || !commandPoolResult.has_value())
    {
        return 10;
    }
    auto queuePool = queuePoolResult.take_value();
    auto commandPool = commandPoolResult.take_value();
    auto queueResult = queuePool->acquire(cue::QueueType::Graphics);
    auto commandResult = commandPool->acquire(cue::QueueType::Graphics);
    if (!queueResult.has_value() || !commandResult.has_value())
    {
        return 11;
    }
    auto queue = queueResult.take_value();
    auto command = commandResult.take_value();
    auto* context = dynamic_cast<cue::dx12::DX12GpuCommandContext*>(command.get());
    if (!context || !context->command_list())
    {
        return 12;
    }
    auto* list = context->command_list();
    auto aliasBarriers = frames->graph_resources(0)->barriers_before_pass(0);
    if (aliasBarriers.size() != 1)
    {
        return 13;
    }
    list->ResourceBarrier(static_cast<UINT>(aliasBarriers.size()), aliasBarriers.data());
    transition(*list, *first->resource(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const std::array<float, 4> clearColor{0.25f, 0.5f, 0.75f, 1.0f};
    auto targetResult = frames->rtv(0, color);
    if (!targetResult.has_value())
    {
        return 14;
    }
    list->ClearRenderTargetView(*targetResult.try_value(), clearColor.data(), 0, nullptr);
    transition(*list, *first->resource(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = first->resource();
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = readback->resource();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = layout;
    list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    if (!command->close().has_value())
    {
        return 14;
    }
    auto submitResult = commandPool->submit(*queue, *command);
    if (!submitResult.has_value())
    {
        return 15;
    }
    std::shared_ptr<cue::ICommandCompletion> completion(submitResult.take_value());
    if (!frames->mark_submitted(0, completion).has_value() ||
        frames->mark_submitted(0, completion).has_value() ||
        !frames->begin_frame(0).has_value())
    {
        return 16;
    }
    std::vector<std::byte> pixels(static_cast<std::size_t>(totalBytes));
    if (!readback->read(0, pixels).has_value())
    {
        return 17;
    }
    const auto matches = [&pixels, &layout](std::size_t a_x, std::size_t a_y) {
        const auto offset = a_y * layout.Footprint.RowPitch + a_x * 4;
        const auto red = std::to_integer<int>(pixels[offset]);
        const auto green = std::to_integer<int>(pixels[offset + 1]);
        const auto blue = std::to_integer<int>(pixels[offset + 2]);
        return red >= 63 && red <= 65 && green >= 127 && green <= 129 &&
               blue >= 190 && blue <= 192;
    };
    if (!matches(0, 0) || !matches(3, 3))
    {
        return 18;
    }
    command.reset();
    queue.reset();
    if (!frames->shutdown().has_value() || !frames->shutdown().has_value() ||
        frames->resource(0, color) || !commandPool->shutdown().has_value() ||
        !queuePool->shutdown().has_value())
    {
        return 19;
    }
    return 0;
}
} // namespace

/// @brief FinalColorTexture の枠分離と WARP 上の画素値を確認する
int main()
{
    const auto result = run_tests();
    return result == 0 ? run_imported_view_tests() : result;
}
