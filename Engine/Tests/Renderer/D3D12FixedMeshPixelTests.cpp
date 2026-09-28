#include "D3D12DeviceContext.h"
#include "D3D12PipelineCache.h"
#include "D3D12QueueContext.h"
#include "D3D12StaticMeshPool.h"
#include "D3D12SurfacePool.h"
#include "D3D12TrianglePass.h"
#include "D3D12ViewManager.h"

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
    auto deviceResult = D3D12DeviceContext::create();
    if (!deviceResult.has_value())
    {
        return 1;
    }
    auto device = deviceResult.take_value();
    auto queueResult = D3D12QueueContext::create(*device);
    if (!queueResult.has_value())
    {
        return 21;
    }
    auto queue = queueResult.take_value();
    auto viewsResult = D3D12ViewManager::create(*device, 2);
    if (!viewsResult.has_value())
    {
        return 2;
    }
    auto views = viewsResult.take_value();
    auto surfacesResult = D3D12SurfacePool::create(*device, *views, {64, 64});
    if (!surfacesResult.has_value())
    {
        return 3;
    }
    auto surfaces = surfacesResult.take_value();
    auto pipelineResult = D3D12PipelineCache::create(*device);
    if (!pipelineResult.has_value())
    {
        return 4;
    }
    auto pipeline = pipelineResult.take_value();
    auto meshesResult = D3D12StaticMeshPool::create(*device, *queue);
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
    auto rtvResult = surfaces->color_rtv(0);
    if (!rtvResult.has_value())
    {
        return 8;
    }
    const auto rtv = rtvResult.take_value();
    const auto dsv = surfaces->depth_dsv();
    cue::FrameGraphBuilder graph;
    auto colorHandle = graph.create_resource("TriangleColor", cue::GraphResourceLifetime::Transient,
                                             cue::GraphResourceState::Common, cue::GraphResourceState::Common);
    auto depthHandle = graph.create_resource("TriangleDepth", cue::GraphResourceLifetime::Persistent,
                                             cue::GraphResourceState::Common, cue::GraphResourceState::Common);
    if (!colorHandle.has_value() || !depthHandle.has_value())
    {
        return 22;
    }
    const TrianglePassContext passContext{*pipeline, *meshes, colorHandle.take_value(), depthHandle.take_value(),
                                          meshes->triangle(), {64, 64}, 0, rtv, dsv,
                                          {1.0f, 1.0f, 1.0f, 1.0f}};
    const D3D12TrianglePass trianglePass(passContext);
    if (!trianglePass.setup(graph).has_value())
    {
        return 23;
    }
    auto graphResult = graph.compile();
    if (!graphResult.has_value() || graphResult.try_value()->passes.size() != 1 ||
        graphResult.try_value()->passes[0].name != trianglePass.name() ||
        graphResult.try_value()->passes[0].barriers.size() != 2)
    {
        return 24;
    }
    transition(list.Get(), surfaces->color(0), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
    transition(list.Get(), surfaces->depth(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
    constexpr float clearColor[4] = {0.07f, 0.13f, 0.25f, 1.0f};
    list->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
    list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    if (!trianglePass.execute(list.Get()).has_value())
    {
        return 10;
    }
    transition(list.Get(), surfaces->color(0), D3D12_RESOURCE_STATE_RENDER_TARGET,
               D3D12_RESOURCE_STATE_COPY_SOURCE);

    const auto desc = surfaces->color(0)->GetDesc();
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
    source.pResource = surfaces->color(0);
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
    if (!surfaces->resize(*device, {80, 48}).has_value() || surfaces->color(0)->GetDesc().Width != 80 ||
        surfaces->depth()->GetDesc().Height != 48 || !surfaces->color_rtv(0).has_value())
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
    if (!meshes->destroy(*queue, handle).has_value() ||
        meshes->draw(list.Get(), handle).has_value())
    {
        return 16;
    }
    return 0;
}
