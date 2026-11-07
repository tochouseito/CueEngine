#include <DX12/DX12FrameGraphResources.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include <wrl/client.h>

#include <DX12/DX12CommandPool.h>
#include <DX12/DX12GpuResource.h>
#include <DX12/DX12QueuePool.h>
#include <DX12/DX12RenderDevice.h>

/// @brief Resource の現在 State だけを一つの Command List 内で遷移させる
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

/// @brief WARP で Alias の同一 Offset、Activation 順、GPU Copy と Fence 回収を検証する
int main()
{
    auto builderResult = cue::FrameGraphBuilder::create();
    if (!builderResult.has_value())
    {
        return 1;
    }
    auto builder = builderResult.take_value();
    auto firstResult = builder->create_transient_buffer({64});
    auto secondResult = builder->create_transient_buffer({128});
    auto concurrentResult = builder->create_transient_buffer({64});
    auto externalResult = builder->import_buffer({64}, cue::FrameGraphResourceState::Common,
                                                cue::FrameGraphResourceState::Common);
    auto writeFirstResult = builder->add_pass("WriteFirst");
    auto readFirstResult = builder->add_pass("ReadFirst");
    auto writeSecondResult = builder->add_pass("WriteSecond");
    auto readSecondResult = builder->add_pass("ReadSecond");
    if (!firstResult.has_value() || !secondResult.has_value() || !concurrentResult.has_value() ||
        !externalResult.has_value() || !writeFirstResult.has_value() || !readFirstResult.has_value() ||
        !writeSecondResult.has_value() || !readSecondResult.has_value())
    {
        return 2;
    }
    const auto first = firstResult.take_value();
    const auto second = secondResult.take_value();
    const auto concurrent = concurrentResult.take_value();
    const auto external = externalResult.take_value();
    const auto writeFirst = writeFirstResult.take_value();
    const auto readFirst = readFirstResult.take_value();
    const auto writeSecond = writeSecondResult.take_value();
    const auto readSecond = readSecondResult.take_value();
    if (!builder->use(writeFirst, first, cue::FrameGraphAccess::Write,
                      cue::FrameGraphResourceState::CopyDestination).has_value() ||
        !builder->use(readFirst, first, cue::FrameGraphAccess::Read,
                      cue::FrameGraphResourceState::CopySource).has_value() ||
        !builder->use(writeSecond, second, cue::FrameGraphAccess::Write,
                      cue::FrameGraphResourceState::CopyDestination).has_value() ||
        !builder->use(writeSecond, concurrent, cue::FrameGraphAccess::Write,
                      cue::FrameGraphResourceState::CopyDestination).has_value() ||
        !builder->use(readSecond, second, cue::FrameGraphAccess::Read,
                      cue::FrameGraphResourceState::CopySource).has_value())
    {
        return 3;
    }
    auto planResult = builder->build();
    if (!planResult.has_value())
    {
        return 4;
    }
    auto plan = planResult.take_value();
    if (plan.alias_slots().size() != 2 || plan.alias_slots()[0].resources.size() != 2 ||
        plan.alias_slots()[0].resources[0].index != first.index ||
        plan.alias_slots()[0].resources[1].index != second.index ||
        plan.alias_slots()[1].resources.size() != 1 ||
        plan.alias_slots()[1].resources[0].index != concurrent.index)
    {
        return 5;
    }

    auto deviceResult = cue::dx12::DX12RenderDevice::create(cue::dx12::AdapterSelection::Warp);
    if (!deviceResult.has_value())
    {
        return 6;
    }
    auto device = deviceResult.take_value();
    Microsoft::WRL::ComPtr<ID3D12InfoQueue> infoQueue;
    if (SUCCEEDED(device->device()->QueryInterface(IID_PPV_ARGS(&infoQueue))))
    {
        infoQueue->ClearStoredMessages();
    }
    auto unsupportedBuilderResult = cue::FrameGraphBuilder::create();
    if (!unsupportedBuilderResult.has_value())
    {
        return 22;
    }
    auto unsupportedBuilder = unsupportedBuilderResult.take_value();
    auto unsupportedTextureResult = unsupportedBuilder->create_transient_texture2d({4, 4});
    auto unsupportedPassResult = unsupportedBuilder->add_pass("RenderTarget");
    if (!unsupportedTextureResult.has_value() || !unsupportedPassResult.has_value() ||
        !unsupportedBuilder->use(unsupportedPassResult.take_value(), unsupportedTextureResult.take_value(),
                                 cue::FrameGraphAccess::Write,
                                 cue::FrameGraphResourceState::RenderTarget).has_value())
    {
        return 23;
    }
    auto unsupportedPlanResult = unsupportedBuilder->build();
    if (!unsupportedPlanResult.has_value() ||
        cue::dx12::DX12FrameGraphResources::create(*device, *unsupportedPlanResult.try_value()).has_value())
    {
        return 24;
    }
    auto graphResult = cue::dx12::DX12FrameGraphResources::create(*device, plan);
    if (!graphResult.has_value())
    {
        return 7;
    }
    auto graph = graphResult.take_value();
    auto* firstResource = graph->resource(first);
    auto* secondResource = graph->resource(second);
    auto* concurrentResource = graph->resource(concurrent);
    if (!firstResource || !secondResource || !concurrentResource || graph->resource(external) ||
        graph->resource({first.graphId + 1, first.index}) ||
        firstResource->placement_heap() != secondResource->placement_heap() ||
        firstResource->placement_offset() != secondResource->placement_offset() ||
        (concurrentResource->placement_heap() == secondResource->placement_heap() &&
         concurrentResource->placement_offset() == secondResource->placement_offset()))
    {
        return 8;
    }
    const auto firstBarriers = graph->barriers_before_pass(0);
    const auto secondBarriers = graph->barriers_before_pass(2);
    if (firstBarriers.size() != 1 || firstBarriers[0].Aliasing.pResourceBefore ||
        firstBarriers[0].Aliasing.pResourceAfter != firstResource->resource() ||
        secondBarriers.size() != 2 ||
        secondBarriers[0].Aliasing.pResourceBefore != firstResource->resource() ||
        secondBarriers[0].Aliasing.pResourceAfter != secondResource->resource() ||
        secondBarriers[1].Aliasing.pResourceAfter != concurrentResource->resource())
    {
        return 9;
    }

    constexpr std::array<std::uint32_t, 8> k_source = {
        0x12345678, 0x9abcdef0, 0x13572468, 0xdeadbeef,
        0x24681357, 0xfedcba98, 0xabcdef01, 0x76543210};
    constexpr std::uint64_t k_segmentSize = sizeof(k_source) / 2;
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
    if (!upload->write(0, std::as_bytes(std::span{k_source})).has_value())
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
    auto& list = *native->command_list();
    list.ResourceBarrier(static_cast<UINT>(firstBarriers.size()), firstBarriers.data());
    transition(list, *firstResource->resource(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    list.CopyBufferRegion(firstResource->resource(), 0, upload->resource(), 0, k_segmentSize);
    transition(list, *firstResource->resource(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list.CopyBufferRegion(readback->resource(), 0, firstResource->resource(), 0, k_segmentSize);

    list.ResourceBarrier(static_cast<UINT>(secondBarriers.size()), secondBarriers.data());
    transition(list, *secondResource->resource(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    transition(list, *concurrentResource->resource(), D3D12_RESOURCE_STATE_COMMON,
               D3D12_RESOURCE_STATE_COPY_DEST);
    list.CopyBufferRegion(secondResource->resource(), 0, upload->resource(), k_segmentSize, k_segmentSize);
    list.CopyBufferRegion(concurrentResource->resource(), 0, upload->resource(), 0, k_segmentSize);
    transition(list, *secondResource->resource(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list.CopyBufferRegion(readback->resource(), k_segmentSize, secondResource->resource(), 0, k_segmentSize);
    if (!command->close().has_value())
    {
        return 15;
    }
    auto submittedResult = commandPool->submit(*queue, *command);
    if (!submittedResult.has_value())
    {
        return 16;
    }
    std::shared_ptr<cue::ICommandCompletion> completion(submittedResult.take_value());
    if (!graph->mark_submitted(completion).has_value() || !graph->mark_submitted(completion).has_value() ||
        !graph->shutdown().has_value() || graph->resource(first))
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
    if (infoQueue)
    {
        const std::uint64_t messageCount = infoQueue->GetNumStoredMessages();
        for (std::uint64_t index = 0; index < messageCount; ++index)
        {
            SIZE_T bytes = 0;
            if (FAILED(infoQueue->GetMessage(index, nullptr, &bytes)))
            {
                return 20;
            }
            std::vector<std::max_align_t> storage((bytes + sizeof(std::max_align_t) - 1) /
                                                   sizeof(std::max_align_t));
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            if (FAILED(infoQueue->GetMessage(index, message, &bytes)) ||
                message->Severity == D3D12_MESSAGE_SEVERITY_ERROR ||
                message->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION)
            {
                return 21;
            }
        }
    }
    return 0;
}
