#include "DX12RenderDevice.h"
#include "DX12CommandRecorder.h"
#include "DX12FramePasses.h"
#include "DX12GraphExecutor.h"
#include "DX12GraphResourceBindings.h"
#include "DX12PipelineCache.h"
#include "DX12PipelineManager.h"
#include "DX12GpuCommandQueue.h"
#include "DX12QueuePool.h"
#include "DX12ResourcePool.h"
#include "DX12StaticMeshPool.h"
#include "DX12SurfacePool.h"
#include "DX12TrianglePass.h"
#include "DX12ViewManager.h"

#include <Cue/Renderer/FrameGraph/FrameGraphRuntime.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace
{
/// @brief 一つの Texture の状態遷移を記録する
void transition(ID3D12GraphicsCommandList* a_list, ID3D12Resource* a_resource,
                D3D12_RESOURCE_STATES a_before, D3D12_RESOURCE_STATES a_after)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = a_resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = a_before;
    barrier.Transition.StateAfter = a_after;
    a_list->ResourceBarrier(1, &barrier);
}
} // namespace

/// @brief 利用可能な Adapter で固定 Mesh を描き、中心 Pixel と背景の違いを Readback で確認する
int main()
{
    using namespace cue::detail;
    auto deviceResult = DX12RenderDevice::create();
    if (!deviceResult.has_value())
    {
        return 1;
    }
    auto device = deviceResult.take_value();
    auto viewsResult = DX12ViewManager::create(*device, 2);
    if (!viewsResult.has_value())
    {
        return 2;
    }
    auto views = viewsResult.take_value();
    auto queuesResult = DX12QueuePool::create(*device);
    if (!queuesResult.has_value())
    {
        return 22;
    }
    auto queues = queuesResult.take_value();
    auto* queue = &queues->context(cue::GpuQueueType::Graphics);
    auto resourcesResult = DX12ResourcePool::create(*device, *queues);
    if (!resourcesResult.has_value())
    {
        return 23;
    }
    auto resources = resourcesResult.take_value();
    auto surfacesResult = DX12SurfacePool::create(*device, *views, *resources, {64, 64});
    if (!surfacesResult.has_value())
    {
        return 3;
    }
    auto surfaces = surfacesResult.take_value();
    auto libraryResult = DX12PipelineManager::create(*device, *queues);
    if (!libraryResult.has_value())
    {
        return 4;
    }
    auto library = libraryResult.take_value();
    auto pipelineResult = DX12PipelineCache::create(*device, *resources, *library);
    if (!pipelineResult.has_value())
    {
        return 4;
    }
    auto pipeline = pipelineResult.take_value();
    auto meshesResult = DX12StaticMeshPool::create(*device, *resources);
    if (!meshesResult.has_value())
    {
        return 5;
    }
    auto meshes = meshesResult.take_value();
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    if (FAILED(device->device()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                        IID_PPV_ARGS(&allocator))))
    {
        return 6;
    }
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device->device()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                                   IID_PPV_ARGS(&list))))
    {
        return 7;
    }
    auto backResult = resources->create_texture({64, 64, cue::GpuTextureFormat::Rgba8Unorm});
    if (!backResult.has_value())
    {
        return 8;
    }
    const auto backResource = backResult.take_value();
    auto backPointerResult = resources->resource(backResource);
    if (!backPointerResult.has_value())
    {
        return 8;
    }
    ID3D12Resource* back = backPointerResult.take_value();
    cue::FrameGraph frameGraph;
    auto& graph = frameGraph.builder();
    auto backHandle = graph.import_resource("TestBackBuffer", cue::GraphResourceState::Present,
                                            cue::GraphResourceState::Present);
    auto colorHandle = graph.create_resource("TriangleColor", cue::GraphResourceLifetime::Transient,
                                             cue::GraphResourceState::Common, cue::GraphResourceState::Common);
    auto depthHandle = graph.create_resource("TriangleDepth", cue::GraphResourceLifetime::Persistent,
                                             cue::GraphResourceState::Common, cue::GraphResourceState::Common);
    if (!backHandle.has_value() || !colorHandle.has_value() || !depthHandle.has_value())
    {
        return 22;
    }
    const auto backGraphResource = backHandle.take_value();
    const auto colorGraphResource = colorHandle.take_value();
    const auto depthGraphResource = depthHandle.take_value();
    const TrianglePassContext passContext{*pipeline, *meshes, colorGraphResource, depthGraphResource,
                                          meshes->triangle(),
                                          surfaces->color_view(0), surfaces->depth_view(),
                                          {1.0f, 1.0f, 1.0f, 1.0f}};
    if (!frameGraph.add_pass(std::make_unique<DX12ClearPass>(colorGraphResource, depthGraphResource,
                                                               surfaces->color_view(0), surfaces->depth_view())).has_value() ||
        !frameGraph.add_pass(std::make_unique<DX12TrianglePass>(passContext)).has_value() ||
        !frameGraph.add_pass(std::make_unique<DX12CopyToBackBufferPass>(colorGraphResource,
                                                                          backGraphResource,
                                                                          surfaces->color_resource(0), back)).has_value())
    {
        return 23;
    }
    auto graphResult = frameGraph.build();
    if (!graphResult.has_value() || graphResult.try_value()->passes.size() != 3 ||
        graphResult.try_value()->passes[0].name != "Clear" ||
        graphResult.try_value()->passes[1].name != "FixedMesh" ||
        graphResult.try_value()->passes[2].name != "CopyToBackBuffer")
    {
        return 24;
    }
    std::vector<GraphPassCallback> callbacks;
    for (const auto& pass : frameGraph.passes())
    {
        callbacks.emplace_back([&resources, &library, graphPass = pass.get()](ID3D12GraphicsCommandList* a_list) {
            DX12CommandRecorder passRecorder(a_list, cue::GpuQueueType::Graphics, *resources, *library);
            cue::FrameGraphContext passContext(passRecorder, {64, 64, 0});
            return graphPass->execute(passContext);
        });
    }
    DX12GraphResourceBindings bindings(graph);
    cue::FrameGraphBuilder otherGraph;
    auto foreignResource = otherGraph.import_resource("Foreign", cue::GraphResourceState::Common,
                                                     cue::GraphResourceState::Common);
    if (!foreignResource.has_value() || bindings.bind(foreignResource.take_value(), back).has_value())
    {
        return 26;
    }
    if (bindings.resolve().has_value() ||
        !bindings.bind(backGraphResource, back).has_value() ||
        !bindings.bind(colorGraphResource, surfaces->color(0)).has_value() ||
        !bindings.bind(depthGraphResource, surfaces->depth()).has_value() ||
        bindings.bind(backGraphResource, back).has_value())
    {
        return 25;
    }
    auto physicalResources = bindings.resolve();
    if (!physicalResources.has_value() ||
        !DX12GraphExecutor::record(*graphResult.try_value(), list.Get(),
                                   physicalResources.take_value(), callbacks).has_value())
    {
        return 10;
    }
    DX12CommandRecorder recorder(list.Get(), cue::GpuQueueType::Graphics, *resources, *library);
    transition(list.Get(), back, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);

    const auto desc = back->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
    UINT64 bytes = 0;
    device->device()->GetCopyableFootprints(&desc, 0, 1, 0, &layout, nullptr, nullptr, &bytes);
    D3D12_HEAP_PROPERTIES readbackHeap{};
    readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = bytes;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    Microsoft::WRL::ComPtr<ID3D12Resource> readback;
    if (FAILED(device->device()->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &buffer,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback))))
    {
        return 11;
    }
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = back;
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = readback.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = layout;
    list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    if (FAILED(list->Close()))
    {
        return 12;
    }
    ID3D12CommandList* lists[] = {list.Get()};
    queue->queue()->ExecuteCommandLists(1, lists);
    if (!queue->wait_idle().has_value())
    {
        return 13;
    }
    void* mapped = nullptr;
    if (FAILED(readback->Map(0, nullptr, &mapped)))
    {
        return 14;
    }
    const auto* pixels = static_cast<const std::uint8_t*>(mapped);
    const auto* corner = pixels;
    const auto* center = pixels + 32 * layout.Footprint.RowPitch + 32 * 4;
    const bool isVisible = std::memcmp(corner, center, 3) != 0;
    readback->Unmap(0, nullptr);
    if (!isVisible)
    {
        return 15;
    }
    // 同期後の Resize が旧 View を失効させず、新しい Surface を指すことを確認する
    const auto oldColorView = surfaces->color_view(0);
    const auto oldDepthView = surfaces->depth_view();
    if (!surfaces->resize(*device, {80, 48}).has_value() || surfaces->color(0)->GetDesc().Width != 80 ||
        surfaces->depth()->GetDesc().Height != 48 || !surfaces->color_rtv(0).has_value() ||
        resources->cpu_handle(oldColorView).has_value() || resources->cpu_handle(oldDepthView).has_value() ||
        !resources->cpu_handle(surfaces->color_view(0)).has_value() ||
        !resources->cpu_handle(surfaces->depth_view()).has_value())
    {
        return 17;
    }
    Microsoft::WRL::ComPtr<ID3D12InfoQueue> infoQueue;
    if (SUCCEEDED(device->device()->QueryInterface(IID_PPV_ARGS(&infoQueue))))
    {
        const UINT64 messageCount = infoQueue->GetNumStoredMessagesAllowedByRetrievalFilter();
        for (UINT64 index = 0; index < messageCount; ++index)
        {
            SIZE_T length = 0;
            if (FAILED(infoQueue->GetMessage(index, nullptr, &length)))
            {
                return 18;
            }
            std::vector<std::byte> storage(length);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            if (FAILED(infoQueue->GetMessage(index, message, &length)))
            {
                return 19;
            }
            if (message->Severity == D3D12_MESSAGE_SEVERITY_ERROR ||
                message->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION)
            {
                return 20;
            }
        }
    }
    const auto handle = meshes->triangle();
    if (!meshes->destroy(handle).has_value() ||
        meshes->draw(recorder, handle).has_value())
    {
        return 16;
    }
    return 0;
}
