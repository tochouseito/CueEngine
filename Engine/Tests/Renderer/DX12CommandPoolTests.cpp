#include <DX12/DX12CommandPool.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>

#include <wrl/client.h>

#include <DX12/DX12QueuePool.h>
#include <DX12/DX12RenderDevice.h>

/// @brief WARP の実コピーで Command 貸出、提出、Fence 後再利用を検証する
int main()
{
    auto deviceResult = cue::dx12::DX12RenderDevice::create(cue::dx12::AdapterSelection::Warp);
    if (!deviceResult.has_value())
    {
        return 1;
    }
    auto device = deviceResult.take_value();
    auto queuePoolResult = cue::dx12::DX12QueuePool::create(*device);
    if (!queuePoolResult.has_value())
    {
        return 2;
    }
    auto queuePool = queuePoolResult.take_value();

    // 別 Device の Queue を指定しても GPU に誤った List を投入しない
    cue::queueLease foreignQueue;
    auto foreignDeviceResult = cue::dx12::DX12RenderDevice::create(cue::dx12::AdapterSelection::HardwarePreferred);
    if (foreignDeviceResult.has_value())
    {
        auto foreignDevice = foreignDeviceResult.take_value();
        // 同じ WARP Adapter は同一 Native Device を返す環境があるため、Hardware がある場合だけ比較する
        if (!foreignDevice->is_warp())
        {
            auto foreignQueuePoolResult = cue::dx12::DX12QueuePool::create(*foreignDevice);
            if (!foreignQueuePoolResult.has_value())
            {
                return 23;
            }
            auto foreignQueuePool = foreignQueuePoolResult.take_value();
            auto foreignQueueResult = foreignQueuePool->acquire(cue::QueueType::Copy);
            if (!foreignQueueResult.has_value())
            {
                return 24;
            }
            foreignQueue = foreignQueueResult.take_value();
        }
    }
    auto poolResult = cue::dx12::DX12CommandPool::create(*device);
    if (!poolResult.has_value())
    {
        return 3;
    }
    std::unique_ptr<cue::ICommandPool> pool = poolResult.take_value();
    auto graphicsResult = queuePool->acquire(cue::QueueType::Graphics);
    auto copyFirstResult = queuePool->acquire(cue::QueueType::Copy);
    auto copySecondResult = queuePool->acquire(cue::QueueType::Copy);
    if (!graphicsResult.has_value() || !copyFirstResult.has_value() || !copySecondResult.has_value())
    {
        return 25;
    }
    auto graphicsQueue = graphicsResult.take_value();
    auto copyFirstQueue = copyFirstResult.take_value();
    auto copySecondQueue = copySecondResult.take_value();
    auto firstResult = pool->acquire(cue::QueueType::Copy);
    auto secondResult = pool->acquire(cue::QueueType::Copy);
    if (!firstResult.has_value() || !secondResult.has_value())
    {
        return 4;
    }
    auto first = firstResult.take_value();
    auto second = secondResult.take_value();
    if (first.get() == second.get() || first->type() != cue::QueueType::Copy ||
        first->state() != cue::CommandState::Recording ||
        pool->submit(*copyFirstQueue, *first).has_value())
    {
        return 5;
    }
    if (first->state() != cue::CommandState::Recording)
    {
        return 26;
    }
    auto* copy = dynamic_cast<cue::dx12::DX12GpuCommandContext*>(first.get());
    if (!copy || !copy->command_list())
    {
        return 6;
    }

    // CommandPool に属さない List の提出を GPU 投入前に拒否する
    auto foreignResult = cue::dx12::DX12GpuCommandContext::create(*device->device(), cue::QueueType::Copy, 99);
    if (!foreignResult.has_value())
    {
        return 7;
    }
    auto foreign = foreignResult.take_value();
    if (!foreign->close().has_value() || pool->submit(*copyFirstQueue, *foreign).has_value())
    {
        return 8;
    }

    constexpr std::array<std::uint32_t, 4> k_source = {0x12345678, 0x9abcdef0, 0x13572468, 0xdeadbeef};
    D3D12_RESOURCE_DESC bufferDesc{};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width = sizeof(k_source);
    bufferDesc.Height = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels = 1;
    bufferDesc.SampleDesc.Count = 1;
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES uploadHeap{};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_HEAP_PROPERTIES readbackHeap{};
    readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;
    Microsoft::WRL::ComPtr<ID3D12Resource> upload;
    Microsoft::WRL::ComPtr<ID3D12Resource> readback;
    if (FAILED(device->device()->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                                                           D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                           IID_PPV_ARGS(&upload))) ||
        FAILED(device->device()->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                                                           D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                           IID_PPV_ARGS(&readback))))
    {
        return 9;
    }
    if (FAILED(upload->SetName(L"CueEngine CommandPool Test Upload Buffer")) ||
        FAILED(readback->SetName(L"CueEngine CommandPool Test Readback Buffer")))
    {
        return 10;
    }
    void* mapped = nullptr;
    D3D12_RANGE noRead{0, 0};
    if (FAILED(upload->Map(0, &noRead, &mapped)))
    {
        return 11;
    }
    std::memcpy(mapped, k_source.data(), sizeof(k_source));
    upload->Unmap(0, nullptr);

    copy->command_list()->CopyBufferRegion(readback.Get(), 0, upload.Get(), 0, sizeof(k_source));
    if (!first->close().has_value() || first->state() != cue::CommandState::Closed ||
        first->close().has_value() || copy->command_list() != nullptr)
    {
        return 12;
    }
    if (pool->submit(*graphicsQueue, *first).has_value() ||
        (foreignQueue && pool->submit(*foreignQueue, *first).has_value()) ||
        first->state() != cue::CommandState::Closed)
    {
        return 27;
    }
    auto submittedResult = pool->submit(*copyFirstQueue, *first);
    if (!submittedResult.has_value() || first->state() != cue::CommandState::Submitted ||
        pool->submit(*copyFirstQueue, *first).has_value())
    {
        return 13;
    }
    auto firstCompletion = submittedResult.take_value();
    if (!second->close().has_value())
    {
        return 28;
    }
    auto secondSubmitResult = pool->submit(*copySecondQueue, *second);
    if (!secondSubmitResult.has_value() || second->state() != cue::CommandState::Submitted)
    {
        return 29;
    }
    auto secondCompletion = secondSubmitResult.take_value();
    if (firstCompletion->type() != cue::QueueType::Copy ||
        secondCompletion->type() != cue::QueueType::Copy)
    {
        return 31;
    }
    cue::ICommandContext* firstAddress = first.get();
    cue::ICommandContext* secondAddress = second.get();
    first.reset();
    second.reset();
    // 提出先 Queue の寿命を終えても、Slot が保持する Fence から完了を確認できる
    graphicsQueue.reset();
    copyFirstQueue.reset();
    copySecondQueue.reset();
    if (!queuePool->shutdown().has_value())
    {
        return 30;
    }
    queuePool.reset();
    if (!firstCompletion->wait().has_value() || !secondCompletion->wait().has_value() ||
        !firstCompletion->is_complete() || !secondCompletion->is_complete())
    {
        return 14;
    }
    D3D12_RANGE readRange{0, sizeof(k_source)};
    if (FAILED(readback->Map(0, &readRange, &mapped)))
    {
        return 15;
    }
    const bool isCopied = std::memcmp(mapped, k_source.data(), sizeof(k_source)) == 0;
    readback->Unmap(0, &noRead);
    if (!isCopied)
    {
        return 16;
    }

    // 完了済み Slot と未提出で返した Slot の両方を Reset して再利用する
    auto reusedFirstResult = pool->acquire(cue::QueueType::Copy);
    auto reusedSecondResult = pool->acquire(cue::QueueType::Copy);
    if (!reusedFirstResult.has_value() || !reusedSecondResult.has_value())
    {
        return 17;
    }
    auto reusedFirst = reusedFirstResult.take_value();
    auto reusedSecond = reusedSecondResult.take_value();
    if (reusedFirst.get() != firstAddress || reusedSecond.get() != secondAddress ||
        reusedFirst->state() != cue::CommandState::Recording ||
        reusedSecond->state() != cue::CommandState::Recording)
    {
        return 18;
    }
    if (pool->shutdown().has_value() || pool->acquire(cue::QueueType::Copy).has_value())
    {
        return 19;
    }
    reusedFirst.reset();
    reusedSecond.reset();
    if (!pool->shutdown().has_value())
    {
        return 20;
    }
    pool.reset();

    // Backend 停止後も残る Command Lease が依存 Object の寿命を維持する
    auto lifetime = std::make_shared<int>(1);
    std::weak_ptr<int> weakLifetime = lifetime;
    auto lifetimePoolResult = cue::dx12::DX12CommandPool::create(*device, lifetime);
    if (!lifetimePoolResult.has_value())
    {
        return 32;
    }
    auto lifetimePool = lifetimePoolResult.take_value();
    lifetime.reset();
    auto lifetimeLeaseResult = lifetimePool->acquire(cue::QueueType::Copy);
    if (!lifetimeLeaseResult.has_value())
    {
        return 33;
    }
    auto lifetimeLease = lifetimeLeaseResult.take_value();
    if (lifetimePool->shutdown().has_value())
    {
        return 34;
    }
    lifetimePool.reset();
    if (weakLifetime.expired() || !lifetimeLease->close().has_value())
    {
        return 35;
    }
    lifetimeLease.reset();
    if (!weakLifetime.expired())
    {
        return 36;
    }
    return 0;
}
