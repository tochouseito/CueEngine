#include <DX12/DX12MainFrameGraph.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include <DX12/DX12CommandPool.h>
#include <DX12/DX12DescriptorAllocator.h>
#include <DX12/DX12GpuResource.h>
#include <DX12/DX12QueuePool.h>
#include <DX12/DX12RenderDevice.h>
#include <DX12/DX12SwapChain.h>
#include <Platform/Windows/WindowsPlatform.h>

namespace
{
/// @brief 読み戻し用に Back Buffer の State を遷移させる
void transition(ID3D12GraphicsCommandList &a_list, ID3D12Resource &a_resource, D3D12_RESOURCE_STATES a_before,
                D3D12_RESOURCE_STATES a_after)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = &a_resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = a_before;
    barrier.Transition.StateAfter = a_after;
    a_list.ResourceBarrier(1, &barrier);
}

/// @brief 固定 Pass の State 計画と WARP の Back Buffer 画素を確認する
int run_tests()
{
    auto systemResult = cue::create_windows_window_system();
    if (!systemResult.has_value())
    {
        return 1;
    }
    auto system = systemResult.take_value();
    auto windowResult = system->create_window({"CueEngine Main Graph Test", {64, 64}});
    if (!windowResult.has_value())
    {
        return 2;
    }
    auto window = windowResult.take_value();
    if (!window->show().has_value())
    {
        return 3;
    }
    auto handleResult = cue::borrow_windows_window_handle(*window);
    auto deviceResult = cue::dx12::DX12RenderDevice::create(cue::dx12::AdapterSelection::Warp);
    if (!handleResult.has_value() || !deviceResult.has_value())
    {
        return 4;
    }
    auto device = deviceResult.take_value();
    auto rtvResult = cue::dx12::DX12DescriptorAllocator::create(*device->device(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 4);
    auto srvResult =
        cue::dx12::DX12DescriptorAllocator::create(*device->device(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2, true);
    auto queueResult = cue::dx12::DX12GpuCommandQueue::create(*device->device(), cue::QueueType::Graphics, 0);
    if (!rtvResult.has_value() || !srvResult.has_value() || !queueResult.has_value())
    {
        return 5;
    }
    auto rtvAllocator = rtvResult.take_value();
    auto srvAllocator = srvResult.take_value();
    cue::queueLease queue(queueResult.take_value().release(), [](cue::IQueueContext *a_queue) { delete a_queue; });
    const cue::dx12::DX12SwapChainConfig config{64, 64, 2, DXGI_FORMAT_R8G8B8A8_UNORM, false, false};
    auto swapResult =
        cue::dx12::DX12SwapChain::create(*device, std::move(queue), *rtvAllocator, *handleResult.try_value(), config);
    if (!swapResult.has_value())
    {
        return 6;
    }
    auto swapChain = swapResult.take_value();
    const std::array<float, 4> clearColor{0.2f, 0.4f, 0.6f, 1.0f};
    auto graphResult =
        cue::dx12::DX12MainFrameGraph::create(*device, *swapChain, 2, *rtvAllocator, *srvAllocator, clearColor);
    if (!graphResult.has_value())
    {
        return 7;
    }
    auto graph = graphResult.take_value();
    const auto &plan = graph->plan();
    if (plan.passes().size() != 2 || plan.passes()[0].name != "ClearFinalColor" ||
        plan.passes()[1].name != "DisplayFinalColor" || plan.passes()[0].barriersBefore.size() != 1 ||
        plan.passes()[0].barriersBefore[0].after != cue::FrameGraphResourceState::RenderTarget ||
        plan.passes()[1].barriersBefore.size() != 2 ||
        plan.passes()[1].barriersBefore[0].after != cue::FrameGraphResourceState::ShaderRead ||
        plan.passes()[1].barriersBefore[1].before != cue::FrameGraphResourceState::Present ||
        plan.passes()[1].barriersBefore[1].after != cue::FrameGraphResourceState::RenderTarget ||
        plan.final_barriers().size() != 2 || plan.final_barriers()[0].after != cue::FrameGraphResourceState::Common ||
        plan.final_barriers()[1].after != cue::FrameGraphResourceState::Present)
    {
        return 8;
    }
    const auto backIndex = swapChain->current_index();
    auto *backBuffer = swapChain->back_buffer(backIndex);
    if (!backBuffer || !swapChain->graphics_queue())
    {
        return 9;
    }
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
    UINT64 totalBytes = 0;
    const auto backDesc = backBuffer->GetDesc();
    device->device()->GetCopyableFootprints(&backDesc, 0, 1, 0, &layout, nullptr, nullptr, &totalBytes);
    auto readbackResult =
        cue::dx12::DX12GpuResource::create_buffer(*device->device(), {totalBytes, cue::GpuMemoryUsage::Readback});
    auto commandPoolResult = cue::dx12::DX12CommandPool::create(*device);
    if (!readbackResult.has_value() || !commandPoolResult.has_value())
    {
        return 10;
    }
    auto readback = readbackResult.take_value();
    auto commandPool = commandPoolResult.take_value();
    auto commandResult = commandPool->acquire(cue::QueueType::Graphics);
    if (!commandResult.has_value())
    {
        return 11;
    }
    auto command = commandResult.take_value();
    auto *context = dynamic_cast<cue::dx12::DX12GpuCommandContext *>(command.get());
    if (!context || !context->command_list() || graph->record(2, *context).has_value() ||
        !graph->record(0, *context).has_value())
    {
        return 12;
    }
    auto *list = context->command_list();
    transition(*list, *backBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = backBuffer;
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = readback->resource();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = layout;
    list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    transition(*list, *backBuffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
    if (!command->close().has_value())
    {
        return 13;
    }
    auto submitResult = commandPool->submit(*swapChain->graphics_queue(), *command);
    if (!submitResult.has_value())
    {
        return 14;
    }
    std::shared_ptr<cue::ICommandCompletion> completion(submitResult.take_value());
    if (!graph->mark_submitted(0, completion).has_value() || !completion->wait().has_value())
    {
        return 15;
    }
    std::vector<std::byte> pixels(static_cast<std::size_t>(totalBytes));
    if (!readback->read(0, pixels).has_value())
    {
        return 16;
    }
    const auto matches = [&pixels, &layout](std::size_t a_x, std::size_t a_y)
    {
        const auto offset = a_y * layout.Footprint.RowPitch + a_x * 4;
        return std::to_integer<int>(pixels[offset]) == 51 && std::to_integer<int>(pixels[offset + 1]) == 102 &&
               std::to_integer<int>(pixels[offset + 2]) == 153 && std::to_integer<int>(pixels[offset + 3]) == 255;
    };
    if (!matches(0, 0) || !matches(63, 0) || !matches(0, 63) || !matches(63, 63) || !swapChain->present().has_value())
    {
        return 17;
    }
    command.reset();
    if (!graph->shutdown().has_value() || !graph->shutdown().has_value())
    {
        return 18;
    }
    int customCallCount = 0;
    auto extendedResult = cue::dx12::DX12MainFrameGraph::create(
        *device, *swapChain, 2, *rtvAllocator, *srvAllocator, clearColor,
        [&customCallCount](cue::FrameGraphBuilder &a_builder, cue::FrameGraphResourceHandle a_finalColor,
                           std::vector<cue::dx12::DX12MainGraphPass> &a_passes) -> cue::Result<void>
        {
            auto passResult = a_builder.add_pass("AfterClear");
            if (!passResult.has_value())
            {
                return cue::Result<void>::failure(*passResult.try_error());
            }
            const auto pass = passResult.take_value();
            auto useResult = a_builder.use(pass, a_finalColor, cue::FrameGraphAccess::Read,
                                           cue::FrameGraphResourceState::ShaderRead);
            if (!useResult.has_value())
            {
                return useResult;
            }
            a_passes.push_back({pass,
                                [&customCallCount, a_finalColor](ID3D12GraphicsCommandList &,
                                                                 const cue::dx12::DX12FrameGraphPassContext &a_context)
                                {
                                    if (!a_context.resource(a_finalColor))
                                    {
                                        return cue::Result<void>::failure({cue::ErrorCategory::InvalidState,
                                                                           "DX12MainFrameGraphTests.custom_binding"});
                                    }
                                    ++customCallCount;
                                    return cue::Result<void>::success();
                                }});
            return cue::Result<void>::success();
        });
    if (!extendedResult.has_value())
    {
        return 19;
    }
    auto extended = extendedResult.take_value();
    if (extended->plan().passes().size() != 3 || extended->plan().passes()[0].name != "ClearFinalColor" ||
        extended->plan().passes()[1].name != "AfterClear" || extended->plan().passes()[2].name != "DisplayFinalColor")
    {
        return 20;
    }
    auto extraCommandResult = commandPool->acquire(cue::QueueType::Graphics);
    if (!extraCommandResult.has_value())
    {
        return 21;
    }
    auto extraCommand = extraCommandResult.take_value();
    auto *extraContext = dynamic_cast<cue::dx12::DX12GpuCommandContext *>(extraCommand.get());
    if (!extraContext || !extended->record(0, *extraContext).has_value() || customCallCount != 1 ||
        !extraCommand->close().has_value())
    {
        return 22;
    }
    extraCommand.reset();
    if (!extended->shutdown().has_value() || !commandPool->shutdown().has_value() ||
        !swapChain->shutdown().has_value() || !window->destroy().has_value())
    {
        return 23;
    }
    return 0;
}
} // namespace

/// @brief 固定 Main Graph だけで Clear 色を表示できることを確認する
int main()
{
    return run_tests();
}
