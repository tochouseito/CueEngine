#include "DX12CommandPool.h"
#include "DX12CommandRecorder.h"
#include "DX12BufferManager.h"
#include "DX12GpuCommandQueue.h"
#include "DX12QueuePool.h"
#include "DX12ResourcePool.h"
#include "DX12PipelineManager.h"
#include "DX12TextureManager.h"
#include "DX12ViewManager.h"
#include "DescriptorAllocator.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <array>
#include <vector>

/// @brief RTV Slot の失効と Command Context の貸出・GPU 完了条件を利用可能な Adapter で確認する
int main()
{
    auto deviceResult = cue::detail::DX12RenderDevice::create();
    if (!deviceResult.has_value())
    {
        return 1;
    }
    auto device = deviceResult.take_value();
    auto queueResult = cue::detail::DX12GpuCommandQueue::create(*device);
    if (!queueResult.has_value())
    {
        return 15;
    }
    auto queue = queueResult.take_value();

    auto viewsResult = cue::detail::DX12ViewManager::create(*device, 2);
    if (!viewsResult.has_value())
    {
        return 2;
    }
    auto views = viewsResult.take_value();
    auto firstView = views->allocate_rtv();
    auto secondView = views->allocate_rtv();
    auto exhausted = views->allocate_rtv();
    if (!firstView.has_value() || !secondView.has_value() || exhausted.has_value())
    {
        return 3;
    }
    const cue::detail::RtvSlot oldSlot = firstView.take_value();
    if (!views->cpu_handle(oldSlot).has_value() || !views->release_rtv(oldSlot).has_value())
    {
        return 4;
    }
    if (views->cpu_handle(oldSlot).has_value() || views->release_rtv(oldSlot).has_value())
    {
        return 5;
    }
    auto replacement = views->allocate_rtv();
    if (!replacement.has_value() || replacement.try_value()->index != oldSlot.index ||
        replacement.try_value()->generation == oldSlot.generation)
    {
        return 6;
    }

    auto poolResult = cue::detail::DX12CommandPool::create(*device);
    if (!poolResult.has_value())
    {
        return 7;
    }
    auto pool = poolResult.take_value();
    auto firstLease = pool->acquire(*queue, 0);
    if (!firstLease.has_value() || pool->acquire(*queue, 0).has_value())
    {
        return 8;
    }
    const cue::detail::CommandLease lease = firstLease.take_value();
    if (!pool->submit(*queue, lease).has_value() || pool->submit(*queue, lease).has_value() ||
        !pool->retire(*queue, lease).has_value())
    {
        return 9;
    }

    // 同じ Slot の再貸出は記録した Fence の完了後だけ許す
    auto reusedLease = pool->acquire(*queue, 0);
    if (!reusedLease.has_value())
    {
        return 10;
    }
    const cue::detail::CommandLease reused = reusedLease.take_value();
    if (pool->submit(*queue, lease).has_value() || !pool->submit(*queue, reused).has_value() ||
        !pool->retire(*queue, reused).has_value())
    {
        return 10;
    }
    if (!queue->wait_idle().has_value())
    {
        return 11;
    }
    pool->release_for_resize();
    if (!pool->recreate_lists(*device).has_value())
    {
        return 12;
    }
    auto otherLease = pool->acquire(*queue, 1);
    if (!otherLease.has_value())
    {
        return 13;
    }
    const cue::detail::CommandLease other = otherLease.take_value();
    if (!pool->submit(*queue, other).has_value() || !pool->retire(*queue, other).has_value() ||
        !queue->wait_idle().has_value())
    {
        return 14;
    }
    pool->mark_idle();

    // 抽象 Command Pool でも貸出、Submit、Fence 返却を同じ経路で完結させる
    cue::ICommandPool& commandApi = *pool;
    auto abstractLeaseResult = commandApi.acquire_context(*queue, 0);
    if (!abstractLeaseResult.has_value())
    {
        return 57;
    }
    const auto abstractLease = abstractLeaseResult.take_value();
    auto contextResult = commandApi.context(abstractLease);
    if (!contextResult.has_value() || !*contextResult.try_value() ||
        !(*contextResult.try_value())->native_command_list() ||
        (*contextResult.try_value())->type() != cue::GpuQueueType::Graphics)
    {
        return 58;
    }
    cue::ICommandContext* commandContext = contextResult.take_value();
    commandContext->begin_event("Ownership.Timestamp");
    if (!commandContext->supports_timestamps() ||
        !commandContext->write_timestamp(0).has_value() ||
        !commandContext->resolve_timestamps(0, 1).has_value())
    {
        return 99;
    }
    commandContext->end_event();
    auto abstractSubmit = commandApi.submit_context(*queue, abstractLease);
    auto abstractRetire = commandApi.retire_context(*queue, abstractLease);
    if (!abstractSubmit.has_value() || !abstractRetire.has_value() ||
        commandApi.context(abstractLease).has_value() ||
        !commandContext->wait_for_pending_fence().has_value() ||
        !commandContext->is_pending_fence_complete())
    {
        return 59;
    }
    auto timestamp = commandContext->read_timestamp(0);
    if (!timestamp.has_value() || timestamp.take_value() == 0)
    {
        return 100;
    }
    auto otherQueueResult = cue::detail::DX12GpuCommandQueue::create(*device);
    if (!otherQueueResult.has_value() ||
        commandApi.acquire_context(*otherQueueResult.try_value()->get(), 1).has_value())
    {
        return 60;
    }

    // Copy の実転送と Copy → Compute → Graphics の GPU 待機を確認する
    auto queuesResult = cue::detail::DX12QueuePool::create(*device);
    if (!queuesResult.has_value())
    {
        return 16;
    }
    auto queues = queuesResult.take_value();
    cue::IQueuePool& queueApi = *queues;
    auto& graphicsContext = queueApi.context(cue::GpuQueueType::Graphics);
    auto& computeContext = queueApi.context(cue::GpuQueueType::Compute);
    auto frequencyResult = graphicsContext.timestamp_frequency();
    if (&queueApi.present_context() != &graphicsContext || !frequencyResult.has_value() ||
        *frequencyResult.try_value() == 0 || graphicsContext.wait_for_queue(graphicsContext).has_value() ||
        !graphicsContext.wait_for_queue(computeContext).has_value() ||
        !queueApi.wait_for_graphics_queue().has_value())
    {
        return 61;
    }
    // Present 用 Graphics Queue を維持しながら、別 Graphics と複数 Compute を貸出す
    auto graphicsLeaseResult = queueApi.acquire_context(cue::GpuQueueType::Graphics);
    if (!graphicsLeaseResult.has_value())
    {
        return 92;
    }
    const auto graphicsQueueLease = graphicsLeaseResult.take_value();
    auto leasedGraphics = queueApi.context(graphicsQueueLease);
    if (!leasedGraphics.has_value() || *leasedGraphics.try_value() == &graphicsContext ||
        queueApi.acquire_context(cue::GpuQueueType::Graphics).has_value())
    {
        return 93;
    }
    std::vector<cue::QueueContextLease> computeLeases;
    for (std::uint32_t index = 0; index < 4; ++index)
    {
        auto leaseResult = queueApi.acquire_context(cue::GpuQueueType::Compute);
        if (!leaseResult.has_value())
        {
            return 94;
        }
        computeLeases.push_back(leaseResult.take_value());
    }
    if (queueApi.acquire_context(cue::GpuQueueType::Compute).has_value())
    {
        return 95;
    }
    auto secondaryFence = queueApi.signal_context(computeLeases[1]);
    if (!secondaryFence.has_value() || secondaryFence.try_value()->queueIndex != computeLeases[1].index ||
        !queueApi.wait_cpu(secondaryFence.take_value()).has_value())
    {
        return 96;
    }
    for (auto lease : computeLeases)
    {
        if (!queueApi.return_context(lease).has_value() || queueApi.context(lease).has_value())
        {
            return 97;
        }
    }
    if (!queueApi.return_context(graphicsQueueLease).has_value())
    {
        return 98;
    }
    cue::IGpuExecution& execution = *queues;
    if (execution.wait_cpu({cue::GpuQueueType::Copy, 1}).has_value())
    {
        return 17;
    }
    D3D12_RESOURCE_DESC bufferDesc{};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width = sizeof(std::uint32_t);
    bufferDesc.Height = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels = 1;
    bufferDesc.SampleDesc.Count = 1;
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES uploadHeap{};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_HEAP_PROPERTIES readbackHeap{};
    readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;
    Microsoft::WRL::ComPtr<ID3D12Resource> uploadBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> readbackBuffer;
    if (FAILED(device->device()->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&uploadBuffer))) ||
        FAILED(device->device()->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readbackBuffer))))
    {
        return 18;
    }
    constexpr std::uint32_t k_expected = 0x12345678;
    void* mapped = nullptr;
    if (FAILED(uploadBuffer->Map(0, nullptr, &mapped)))
    {
        return 19;
    }
    std::memcpy(mapped, &k_expected, sizeof(k_expected));
    uploadBuffer->Unmap(0, nullptr);
    auto copyPoolResult = cue::detail::DX12CommandPool::create(*device, cue::GpuQueueType::Copy);
    auto computePoolResult = cue::detail::DX12CommandPool::create(*device, cue::GpuQueueType::Compute);
    if (!copyPoolResult.has_value() || !computePoolResult.has_value())
    {
        return 20;
    }
    auto copyPool = copyPoolResult.take_value();
    auto computePool = computePoolResult.take_value();
    auto& copyQueue = queues->context(cue::GpuQueueType::Copy);
    auto& computeQueue = queues->context(cue::GpuQueueType::Compute);
    auto copyLeaseResult = copyPool->acquire(copyQueue, 0);
    if (!copyLeaseResult.has_value() || computePool->acquire(copyQueue, 0).has_value())
    {
        return 21;
    }
    const auto copyLease = copyLeaseResult.take_value();
    copyLease.list->CopyBufferRegion(readbackBuffer.Get(), 0, uploadBuffer.Get(), 0, sizeof(k_expected));
    if (!copyPool->submit(copyQueue, copyLease).has_value() ||
        !copyPool->retire(copyQueue, copyLease).has_value())
    {
        return 22;
    }
    auto copied = execution.signal(cue::GpuQueueType::Copy);
    if (!copied.has_value() ||
        !execution.wait_gpu(cue::GpuQueueType::Compute, copied.take_value()).has_value())
    {
        return 23;
    }
    auto computeLeaseResult = computePool->acquire(computeQueue, 0);
    if (!computeLeaseResult.has_value())
    {
        return 24;
    }
    const auto computeLease = computeLeaseResult.take_value();
    if (!computePool->submit(computeQueue, computeLease).has_value() ||
        !computePool->retire(computeQueue, computeLease).has_value())
    {
        return 25;
    }
    auto computed = execution.signal(cue::GpuQueueType::Compute);
    if (!computed.has_value() ||
        !execution.wait_gpu(cue::GpuQueueType::Graphics, computed.take_value()).has_value())
    {
        return 26;
    }
    auto graphics = execution.signal(cue::GpuQueueType::Graphics);
    if (!graphics.has_value() || !execution.wait_cpu(graphics.take_value()).has_value())
    {
        return 27;
    }
    D3D12_RANGE readRange{0, sizeof(k_expected)};
    if (FAILED(readbackBuffer->Map(0, &readRange, &mapped)))
    {
        return 28;
    }
    std::uint32_t actual = 0;
    std::memcpy(&actual, mapped, sizeof(actual));
    readbackBuffer->Unmap(0, nullptr);
    if (actual != k_expected || !execution.wait_idle().has_value())
    {
        return 29;
    }

    // Buffer、Color/Depth Texture、SRV と古い Handle の失効を RHI 経由で確認する
    auto resourcesResult = cue::detail::DX12ResourcePool::create(*device, *queues, 2);
    if (!resourcesResult.has_value())
    {
        return 30;
    }
    auto resources = resourcesResult.take_value();
    cue::IGpuResources& resourceApi = *resources;
    auto bufferResult = resourceApi.create_buffer({sizeof(k_expected), cue::GpuMemory::Upload});
    auto colorResult = resourceApi.create_texture({16, 16, cue::GpuTextureFormat::Rgba8Unorm});
    auto depthResult = resourceApi.create_texture({16, 16, cue::GpuTextureFormat::Depth32Float});
    if (!bufferResult.has_value() || !colorResult.has_value() || !depthResult.has_value())
    {
        return 31;
    }
    const auto bufferHandle = bufferResult.take_value();
    const auto colorHandle = colorResult.take_value();
    const auto depthHandle = depthResult.take_value();
    if (!resourceApi.write_buffer(bufferHandle, 0, &k_expected, sizeof(k_expected)).has_value() ||
        resourceApi.read_buffer(bufferHandle, 0, &actual, sizeof(actual)).has_value())
    {
        return 32;
    }
    auto managedReadback = resourceApi.create_buffer({sizeof(k_expected), cue::GpuMemory::Readback});
    if (!managedReadback.has_value())
    {
        return 39;
    }
    const auto readbackHandle = managedReadback.take_value();
    auto uploadPhysical = resources->resource(bufferHandle);
    auto readbackPhysical = resources->resource(readbackHandle);
    auto managedCopyLease = copyPool->acquire(copyQueue, 1);
    if (!uploadPhysical.has_value() || !readbackPhysical.has_value() || !managedCopyLease.has_value())
    {
        return 40;
    }
    const auto managedLease = managedCopyLease.take_value();
    managedLease.list->CopyBufferRegion(*readbackPhysical.try_value(), 0,
                                        *uploadPhysical.try_value(), 0, sizeof(k_expected));
    if (!copyPool->submit(copyQueue, managedLease).has_value() ||
        !copyPool->retire(copyQueue, managedLease).has_value() ||
        !resourceApi.read_buffer(readbackHandle, 0, &actual, sizeof(actual)).has_value() ||
        actual != k_expected)
    {
        return 41;
    }
    auto rtvResult = resourceApi.create_view(colorHandle, cue::GpuViewKind::RenderTarget);
    auto srvResult = resourceApi.create_view(colorHandle, cue::GpuViewKind::ShaderResource);
    auto dsvResult = resourceApi.create_view(depthHandle, cue::GpuViewKind::DepthStencil);
    auto depthSrvResult = resourceApi.create_view(depthHandle, cue::GpuViewKind::ShaderResource);
    if (!rtvResult.has_value() || !srvResult.has_value() || !dsvResult.has_value() ||
        !depthSrvResult.has_value())
    {
        return 33;
    }
    const auto rtv = rtvResult.take_value();
    const auto srv = srvResult.take_value();
    const auto dsv = dsvResult.take_value();
    const auto depthSrv = depthSrvResult.take_value();
    if (!resources->cpu_handle(rtv).has_value() || !resources->cpu_handle(dsv).has_value() ||
        !resources->gpu_handle(srv).has_value() || resourceApi.destroy(colorHandle).has_value())
    {
        return 34;
    }
    if (!resourceApi.destroy_view(rtv).has_value() || resourceApi.destroy_view(rtv).has_value() ||
        !resourceApi.destroy_view(srv).has_value() || !resourceApi.destroy_view(dsv).has_value() ||
        !resourceApi.destroy_view(depthSrv).has_value())
    {
        return 35;
    }
    // 共通 Shader-visible Heap の容量、CBV 整列、UAV 許可と世代を確認する
    auto constantsResult = resourceApi.create_buffer({256, cue::GpuMemory::Upload});
    auto storageResult = resourceApi.create_buffer({16, cue::GpuMemory::Device, true});
    auto uavColorResult = resourceApi.create_texture({16, 16, cue::GpuTextureFormat::Rgba8Unorm, true});
    if (!constantsResult.has_value() || !storageResult.has_value() || !uavColorResult.has_value())
    {
        return 42;
    }
    const auto constants = constantsResult.take_value();
    const auto storage = storageResult.take_value();
    const auto uavColor = uavColorResult.take_value();
    if (resourceApi.create_view(constants, {cue::GpuViewKind::ConstantBuffer, 4, 252}).has_value() ||
        resourceApi.create_view(bufferHandle, {cue::GpuViewKind::UnorderedAccess, 0, 4}).has_value())
    {
        return 43;
    }
    auto cbvResult = resourceApi.create_view(constants, {cue::GpuViewKind::ConstantBuffer, 0, 256});
    auto bufferUavResult = resourceApi.create_view(storage, {cue::GpuViewKind::UnorderedAccess, 0, 16});
    if (!cbvResult.has_value() || !bufferUavResult.has_value() ||
        resourceApi.create_view(uavColor, cue::GpuViewKind::UnorderedAccess).has_value())
    {
        return 44;
    }
    const auto cbv = cbvResult.take_value();
    const auto bufferUav = bufferUavResult.take_value();
    if (!resources->gpu_handle(cbv).has_value() || !resources->gpu_handle(bufferUav).has_value() ||
        !resourceApi.destroy_view(cbv).has_value() || resources->gpu_handle(cbv).has_value())
    {
        return 45;
    }
    auto textureUavResult = resourceApi.create_view(uavColor, cue::GpuViewKind::UnorderedAccess);
    if (!textureUavResult.has_value())
    {
        return 46;
    }
    const auto textureUav = textureUavResult.take_value();
    // Raw Buffer の UAV Clear が GPU に届くことを Readback で確認する
    auto clearReadbackResult = resources->create_buffer({16, cue::GpuMemory::Readback});
    auto clearPipelinesResult = cue::detail::DX12PipelineManager::create(*device, *queues);
    auto clearPoolResult = cue::detail::DX12CommandPool::create(*device);
    if (!clearReadbackResult.has_value() || !clearPipelinesResult.has_value() ||
        !clearPoolResult.has_value())
    {
        return 79;
    }
    const auto clearReadback = clearReadbackResult.take_value();
    auto clearPipelines = clearPipelinesResult.take_value();
    auto clearPool = clearPoolResult.take_value();
    auto clearLeaseResult = clearPool->acquire(queues->context(cue::GpuQueueType::Graphics), 0);
    if (!clearLeaseResult.has_value())
    {
        return 80;
    }
    const auto clearLease = clearLeaseResult.take_value();
    cue::detail::DX12CommandRecorder clearRecorder(clearLease.list, cue::GpuQueueType::Graphics,
                                                   *resources, *clearPipelines);
    if (!clearRecorder.transition(storage, cue::GpuResourceState::Common,
                                  cue::GpuResourceState::UnorderedAccess).has_value() ||
        !clearRecorder.clear_unordered_access_uint(bufferUav, {13, 13, 13, 13}).has_value() ||
        !clearRecorder.transition(storage, cue::GpuResourceState::UnorderedAccess,
                                  cue::GpuResourceState::CopySource).has_value() ||
        !clearRecorder.copy_buffer(clearReadback, 0, storage, 0, 16).has_value() ||
        !clearPool->submit(queues->context(cue::GpuQueueType::Graphics), clearLease).has_value() ||
        !clearPool->retire(queues->context(cue::GpuQueueType::Graphics), clearLease).has_value() ||
        !queues->wait_idle().has_value())
    {
        return 81;
    }
    std::array<std::uint32_t, 4> clearValues{};
    if (!resources->read_buffer(clearReadback, 0, clearValues.data(), 16).has_value() ||
        clearValues != std::array<std::uint32_t, 4>{13, 13, 13, 13} ||
        !resources->destroy(clearReadback).has_value())
    {
        return 82;
    }
    if (textureUav.index != cbv.index || textureUav.generation == cbv.generation ||
        !resourceApi.destroy_view(bufferUav).has_value() ||
        !resourceApi.destroy_view(textureUav).has_value() ||
        !resourceApi.destroy(constants).has_value() || !resourceApi.destroy(storage).has_value() ||
        !resourceApi.destroy(uavColor).has_value())
    {
        return 47;
    }
    // 一つの論理 Buffer から複数 Heap Slice を取得し、二番目の Upload から Readback まで確認する
    auto managerResult = cue::detail::DX12BufferManager::create(*resources);
    if (!managerResult.has_value())
    {
        return 70;
    }
    auto manager = managerResult.take_value();
    cue::GpuBufferDesc multiDesc;
    multiDesc.size = 1024;
    multiDesc.name = "multi-slice";
    multiDesc.defaultHeapCount = 2;
    multiDesc.uploadHeapCount = 2;
    multiDesc.readbackHeapCount = 1;
    multiDesc.stride = sizeof(std::uint32_t);
    multiDesc.elementCount = 4;
    multiDesc.alignment = 256;
    auto multiResult = manager->create_buffer(multiDesc);
    if (!multiResult.has_value())
    {
        return 71;
    }
    const auto multi = multiResult.take_value();
    auto uploadView = manager->get_upload_buffer_view(multi);
    auto uploaders = manager->create_slot_uploaders<std::uint32_t>(multi, 2);
    auto firstDefault = resources->resource(multi, cue::GpuMemory::Device, 0);
    auto secondDefault = resources->resource(multi, cue::GpuMemory::Device, 1);
    auto secondUpload = resources->resource(multi, cue::GpuMemory::Upload, 1);
    auto readbackSlice = resources->resource(multi, cue::GpuMemory::Readback, 0);
    if (!uploadView.has_value() || uploadView.try_value()->mappedData.size() != 2 ||
        !uploaders.has_value() || uploaders.try_value()->size() != 2 ||
        !firstDefault.has_value() || !secondDefault.has_value() ||
        *firstDefault.try_value() == *secondDefault.try_value() ||
        !secondUpload.has_value() || !readbackSlice.has_value() ||
        resources->resource(multi, cue::GpuMemory::Upload, 2).has_value())
    {
        return 72;
    }
    constexpr std::uint32_t k_sliceValue = 0x5A17B24Cu;
    if (!(*uploaders.try_value())[1].push(0, k_sliceValue).has_value() ||
        !(*uploaders.try_value())[1].commit().has_value())
    {
        return 73;
    }
    auto indexedCbv = resourceApi.create_view(multi, {cue::GpuViewKind::ConstantBuffer, 0, 256, 0,
                                                       "upload-1", cue::GpuMemory::Upload, 1});
    if (!indexedCbv.has_value() ||
        !resourceApi.destroy_view(indexedCbv.take_value()).has_value())
    {
        return 74;
    }
    auto sliceCopy = copyPool->acquire(copyQueue, 0);
    if (!sliceCopy.has_value())
    {
        return 75;
    }
    const auto sliceLease = sliceCopy.take_value();
    sliceLease.list->CopyBufferRegion(*readbackSlice.try_value(), 0, *secondUpload.try_value(), 0,
                                      sizeof(k_sliceValue));
    if (!copyPool->submit(copyQueue, sliceLease).has_value() ||
        !copyPool->retire(copyQueue, sliceLease).has_value())
    {
        return 76;
    }
    auto readbackView = manager->get_readback_buffer_view(multi);
    std::uint32_t sliceActual = 0;
    if (!readbackView.has_value() || readbackView.try_value()->mappedData.size() != 1)
    {
        return 77;
    }
    std::memcpy(&sliceActual, readbackView.try_value()->mappedData[0], sizeof(sliceActual));
    if (sliceActual != k_sliceValue || !manager->destroy_buffer(multi).has_value() ||
        manager->get_upload_buffer_view(multi).has_value())
    {
        return 78;
    }
    // Cube の Mip と複数 Buffer、および 3D と D24 の View 次元を生成時に検証する
    cue::GpuTextureDesc cubeDesc;
    cubeDesc.width = 64;
    cubeDesc.height = 64;
    cubeDesc.format = cue::GpuTextureFormat::Bc7Unorm;
    cubeDesc.type = cue::GpuTextureType::CubeMap;
    cubeDesc.kind = cue::GpuTextureKind::Default;
    cubeDesc.arraySize = 6;
    cubeDesc.mipLevels = 4;
    cubeDesc.bufferCount = 2;
    auto cubeResult = resourceApi.create_texture(cubeDesc);
    if (!cubeResult.has_value())
    {
        return 79;
    }
    const auto cube = cubeResult.take_value();
    auto secondCube = resources->texture_resource(cube, 1);
    cue::GpuViewDesc cubeViewDesc{cue::GpuViewKind::ShaderResource};
    cubeViewDesc.resourceIndex = 1;
    cubeViewDesc.mipSlice = 1;
    cubeViewDesc.mipLevels = 2;
    auto cubeView = resourceApi.create_view(cube, cubeViewDesc);
    if (!secondCube.has_value() || !cubeView.has_value() ||
        !resourceApi.destroy_view(cubeView.take_value()).has_value() ||
        !resourceApi.destroy(cube).has_value())
    {
        return 80;
    }
    cue::GpuTextureDesc volumeDesc;
    volumeDesc.width = 16;
    volumeDesc.height = 16;
    volumeDesc.arraySize = 4;
    volumeDesc.mipLevels = 2;
    volumeDesc.type = cue::GpuTextureType::Texture3D;
    volumeDesc.allowUnorderedAccess = true;
    auto volumeResult = resourceApi.create_texture(volumeDesc);
    if (!volumeResult.has_value())
    {
        return 81;
    }
    const auto volume = volumeResult.take_value();
    cue::GpuViewDesc volumeUavDesc{cue::GpuViewKind::UnorderedAccess};
    volumeUavDesc.mipSlice = 1;
    auto volumeUav = resourceApi.create_view(volume, volumeUavDesc);
    auto volumeRtv = resourceApi.create_view(volume, cue::GpuViewKind::RenderTarget);
    if (!volumeUav.has_value() || !volumeRtv.has_value() ||
        !resourceApi.destroy_view(volumeUav.take_value()).has_value() ||
        !resourceApi.destroy_view(volumeRtv.take_value()).has_value() ||
        !resourceApi.destroy(volume).has_value())
    {
        return 82;
    }
    cue::GpuTextureDesc depth24Desc;
    depth24Desc.width = 16;
    depth24Desc.height = 16;
    depth24Desc.format = cue::GpuTextureFormat::Depth24Stencil8;
    auto depth24Result = resourceApi.create_texture(depth24Desc);
    if (!depth24Result.has_value())
    {
        return 83;
    }
    const auto depth24 = depth24Result.take_value();
    auto depth24Dsv = resourceApi.create_view(depth24, cue::GpuViewKind::DepthStencil);
    auto depth24Srv = resourceApi.create_view(depth24, cue::GpuViewKind::ShaderResource);
    if (!depth24Dsv.has_value() || !depth24Srv.has_value() ||
        !resourceApi.destroy_view(depth24Dsv.take_value()).has_value() ||
        !resourceApi.destroy_view(depth24Srv.take_value()).has_value() ||
        !resourceApi.destroy(depth24).has_value())
    {
        return 84;
    }
    // 初期 Data の転送後、Copy Queue の Readback で各 Pixel を確認する
    auto textureManagerResult = cue::detail::DX12TextureManager::create(*resources);
    if (!textureManagerResult.has_value())
    {
        return 85;
    }
    auto textureManager = textureManagerResult.take_value();
    cue::GpuTextureDesc uploadedDesc;
    uploadedDesc.width = 2;
    uploadedDesc.height = 2;
    uploadedDesc.kind = cue::GpuTextureKind::Default;
    uploadedDesc.name = "initial-data";
    const std::array<std::byte, 16> pixels = {
        std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4},
        std::byte{5}, std::byte{6}, std::byte{7}, std::byte{8},
        std::byte{9}, std::byte{10}, std::byte{11}, std::byte{12},
        std::byte{13}, std::byte{14}, std::byte{15}, std::byte{16}};
    const cue::GpuTextureSubresourceData initial{pixels.data(), pixels.size(), 8, 16};
    auto uploadedResult = textureManager->create_texture(uploadedDesc,
                                                          std::span<const cue::GpuTextureSubresourceData>(&initial, 1));
    if (!uploadedResult.has_value())
    {
        return 86;
    }
    const auto uploaded = uploadedResult.take_value();
    auto actualDesc = textureManager->get_texture_desc(uploaded);
    auto descriptorIndex = textureManager->get_texture_descriptor_index(uploaded);
    auto descriptorAgain = textureManager->get_texture_descriptor_index(uploaded);
    auto physicalTexture = resources->texture_resource(uploaded, 0);
    if (!actualDesc.has_value() || actualDesc.try_value()->width != 2 ||
        !descriptorIndex.has_value() || !descriptorAgain.has_value() ||
        descriptorIndex.take_value() != descriptorAgain.take_value() || !physicalTexture.has_value())
    {
        return 87;
    }
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT numRows = 0;
    UINT64 rowSize = 0;
    UINT64 readbackBytes = 0;
    const auto physicalDesc = (*physicalTexture.try_value())->GetDesc();
    device->device()->GetCopyableFootprints(&physicalDesc, 0, 1, 0, &footprint,
                                             &numRows, &rowSize, &readbackBytes);
    auto pixelReadback = resourceApi.create_buffer({readbackBytes, cue::GpuMemory::Readback});
    auto pixelCopy = copyPool->acquire(copyQueue, 0);
    if (!pixelReadback.has_value() || !pixelCopy.has_value())
    {
        return 88;
    }
    const auto pixelBuffer = pixelReadback.take_value();
    auto physicalReadback = resources->resource(pixelBuffer);
    if (!physicalReadback.has_value())
    {
        return 89;
    }
    const auto pixelLease = pixelCopy.take_value();
    auto recorderPipelines = cue::detail::DX12PipelineManager::create(*device, *queues);
    if (!recorderPipelines.has_value())
    {
        return 101;
    }
    cue::detail::DX12CommandRecorder pixelRecorder(pixelLease.list, cue::GpuQueueType::Copy,
                                                    *resources, *recorderPipelines.try_value()->get());
    auto textureCopyResult = pixelRecorder.copy_texture_region_to_buffer(
        {uploaded, 0, 0, 0, 2, 2, pixelBuffer, 0, 0});
    if (!textureCopyResult.has_value())
    {
        std::fprintf(stderr, "%s\n", textureCopyResult.try_error()->operation.c_str());
        return 102;
    }
    if (!copyPool->submit(copyQueue, pixelLease).has_value() ||
        !copyPool->retire(copyQueue, pixelLease).has_value())
    {
        return 90;
    }
    std::array<std::byte, 8> actualPixels{};
    if (!resourceApi.read_buffer(pixelBuffer, footprint.Footprint.RowPitch, actualPixels.data(),
                                 actualPixels.size()).has_value() ||
        std::memcmp(actualPixels.data(), pixels.data() + 8, actualPixels.size()) != 0 ||
        !textureManager->destroy_texture(uploaded).has_value() ||
        !resourceApi.destroy(pixelBuffer).has_value())
    {
        return 91;
    }
    if (!resourceApi.destroy(colorHandle).has_value() || !resourceApi.destroy(depthHandle).has_value() ||
        !resourceApi.destroy(bufferHandle).has_value() || !resourceApi.destroy(readbackHandle).has_value() ||
        resources->resource(colorHandle).has_value())
    {
        return 36;
    }
    auto replacementResult = resourceApi.create_texture({16, 16, cue::GpuTextureFormat::Rgba8Unorm});
    if (!replacementResult.has_value())
    {
        return 37;
    }
    const auto replacementHandle = replacementResult.take_value();
    if (replacementHandle.index != bufferHandle.index ||
        replacementHandle.generation == bufferHandle.generation ||
        !resourceApi.destroy(replacementHandle).has_value())
    {
        return 38;
    }

    // 表示用 View と GPU Resource View が同じ Heap の別 Slot を使う
    auto sharedAllocatorResult = cue::detail::DescriptorAllocator::create(*device, 3, 3, 16);
    if (!sharedAllocatorResult.has_value())
    {
        return 48;
    }
    auto sharedAllocator = sharedAllocatorResult.take_value();
    if (!sharedAllocator->imgui_heap() ||
        sharedAllocator->imgui_heap() == sharedAllocator->shader_heap())
    {
        return 75;
    }
    auto sharedViewsResult = cue::detail::DX12ViewManager::create(*device, *sharedAllocator);
    auto sharedResourcesResult = cue::detail::DX12ResourcePool::create(*device, *queues, *sharedAllocator);
    if (!sharedViewsResult.has_value() || !sharedResourcesResult.has_value())
    {
        return 49;
    }
    auto sharedViews = sharedViewsResult.take_value();
    auto sharedResources = sharedResourcesResult.take_value();
    auto sharedTextureResult = sharedResources->create_texture({16, 16, cue::GpuTextureFormat::Rgba8Unorm});
    if (!sharedTextureResult.has_value())
    {
        return 50;
    }
    const auto sharedTexture = sharedTextureResult.take_value();
    auto sharedSrvResult = sharedResources->create_view(sharedTexture, cue::GpuViewKind::ShaderResource);
    auto sharedBufferResult = sharedResources->create_buffer({256, cue::GpuMemory::Upload});
    if (!sharedSrvResult.has_value() || !sharedBufferResult.has_value())
    {
        return 76;
    }
    const auto sharedSrv = sharedSrvResult.take_value();
    const auto sharedBuffer = sharedBufferResult.take_value();
    auto sharedCbvResult = sharedResources->create_view(sharedBuffer,
        {cue::GpuViewKind::ConstantBuffer, 0, 256});
    if (!sharedCbvResult.has_value() || sharedSrv.index >= 8 ||
        sharedCbvResult.try_value()->index < 8)
    {
        return 77;
    }
    if (!sharedResources->destroy_view(sharedSrv).has_value() ||
        !sharedResources->destroy_view(sharedCbvResult.take_value()).has_value() ||
        !sharedResources->destroy(sharedBuffer).has_value())
    {
        return 78;
    }
    auto resourceViewResult = sharedResources->create_view(sharedTexture, cue::GpuViewKind::RenderTarget);
    auto presentationViewResult = sharedViews->allocate_rtv();
    if (!resourceViewResult.has_value() || !presentationViewResult.has_value())
    {
        return 51;
    }
    const auto resourceView = resourceViewResult.take_value();
    const auto presentationView = presentationViewResult.take_value();
    auto resourceCpu = sharedResources->cpu_handle(resourceView);
    auto presentationCpu = sharedViews->cpu_handle(presentationView);
    if (!resourceCpu.has_value() || !presentationCpu.has_value() ||
        resourceCpu.try_value()->ptr == presentationCpu.try_value()->ptr ||
        !sharedViews->release_rtv(presentationView).has_value() ||
        !sharedResources->destroy_view(resourceView).has_value() ||
        !sharedResources->destroy(sharedTexture).has_value())
    {
        return 52;
    }

    // 別 Owner の同位置・同世代 Slot は受け付けず、表示用 DSV は破棄後に再利用できる
    auto otherAllocatorResult = cue::detail::DescriptorAllocator::create(*device, 1, 1, 1);
    if (!otherAllocatorResult.has_value())
    {
        return 53;
    }
    auto otherAllocator = otherAllocatorResult.take_value();
    auto firstSlotResult = sharedAllocator->allocate(cue::detail::DescriptorHeapKind::ShaderResource);
    auto secondSlotResult = otherAllocator->allocate(cue::detail::DescriptorHeapKind::ShaderResource);
    if (!firstSlotResult.has_value() || !secondSlotResult.has_value())
    {
        return 54;
    }
    const auto firstSlot = firstSlotResult.take_value();
    const auto secondSlot = secondSlotResult.take_value();
    if (sharedAllocator->cpu_handle(secondSlot).has_value() ||
        sharedAllocator->release(secondSlot).has_value() ||
        !sharedAllocator->cpu_handle(firstSlot).has_value() ||
        !sharedAllocator->release(firstSlot).has_value() ||
        !otherAllocator->release(secondSlot).has_value())
    {
        return 55;
    }
    sharedViews.reset();
    auto replacementViewsResult = cue::detail::DX12ViewManager::create(*device, *sharedAllocator);
    if (!replacementViewsResult.has_value())
    {
        return 56;
    }
    return 0;
}
