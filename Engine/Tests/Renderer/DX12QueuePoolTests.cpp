#include <DX12/DX12QueuePool.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <thread>

#include <wrl/client.h>

#include <DX12/DX12RenderDevice.h>
#include <RHI/Queue.h>

/// @brief 実 Device の Queue 貸出、Fence、GPU Queue 間待機を検証する
int main()
{
    auto deviceResult = cue::dx12::DX12RenderDevice::create(cue::dx12::AdapterSelection::Warp);
    if (!deviceResult.has_value())
    {
        return 1;
    }
    auto device = deviceResult.take_value();
    auto poolResult = cue::dx12::DX12QueuePool::create(*device);
    if (!poolResult.has_value())
    {
        return 2;
    }
    std::unique_ptr<cue::IQueuePool> pool = poolResult.take_value();

    auto graphicsResult = pool->acquire(cue::QueueType::Graphics);
    if (!graphicsResult.has_value() || !graphicsResult.try_value() || !*graphicsResult.try_value())
    {
        return 3;
    }
    cue::queueLease graphics = graphicsResult.take_value();
    if (graphics->type() != cue::QueueType::Graphics ||
        pool->acquire(cue::QueueType::Graphics).has_value())
    {
        return 4;
    }

    // Legacy と同じ数の Compute／Copy Queue を借用し、容量超過を拒否する
    std::array<cue::queueLease, 4> computes{};
    std::array<cue::queueLease, 4> copies{};
    for (std::size_t index = 0; index < computes.size(); ++index)
    {
        auto computeResult = pool->acquire(cue::QueueType::Compute);
        auto copyResult = pool->acquire(cue::QueueType::Copy);
        if (!computeResult.has_value() || !copyResult.has_value())
        {
            return 5;
        }
        computes[index] = computeResult.take_value();
        copies[index] = copyResult.take_value();
        if (computes[index]->type() != cue::QueueType::Compute || copies[index]->type() != cue::QueueType::Copy)
        {
            return 6;
        }
    }
    if (pool->acquire(cue::QueueType::Compute).has_value() || pool->acquire(cue::QueueType::Copy).has_value())
    {
        return 7;
    }

    // 未発行値は CPU／GPU 待機のどちらでも受け付けない
    if (graphics->wait_for_fence(1).has_value() || copies[0]->wait_for_queue(*computes[0], 1).has_value())
    {
        return 8;
    }
    auto fenceResult = computes[0]->signal();
    if (!fenceResult.has_value() || fenceResult.try_value() == nullptr || *fenceResult.try_value() == 0)
    {
        return 9;
    }
    const std::uint64_t fenceValue = fenceResult.take_value();
    if (!copies[0]->wait_for_queue(*computes[0], fenceValue).has_value())
    {
        return 10;
    }
    auto copyFenceResult = copies[0]->signal();
    if (!copyFenceResult.has_value() || !copies[0]->wait_for_fence(copyFenceResult.take_value()).has_value() ||
        !computes[0]->is_fence_complete(fenceValue))
    {
        return 11;
    }
    auto frequencyResult = graphics->get_timestamp_frequency();
    if (!frequencyResult.has_value() || *frequencyResult.try_value() == 0)
    {
        return 12;
    }

    // 複数 Thread から Signal しても Fence 値を重複させない
    std::array<std::array<std::uint64_t, 8>, 4> signaledValues{};
    std::array<std::jthread, 4> workers{};
    std::atomic<bool> hasSignalFailure = false;
    for (std::size_t workerIndex = 0; workerIndex < workers.size(); ++workerIndex)
    {
        workers[workerIndex] = std::jthread([&, workerIndex] {
            for (std::uint64_t& value : signaledValues[workerIndex])
            {
                auto result = graphics->signal();
                if (!result.has_value())
                {
                    hasSignalFailure.store(true);
                    return;
                }
                value = result.take_value();
            }
        });
    }
    for (auto& worker : workers)
    {
        worker.join();
    }
    std::array<std::uint64_t, 32> orderedValues{};
    for (std::size_t index = 0; index < signaledValues.size(); ++index)
    {
        std::copy(signaledValues[index].begin(), signaledValues[index].end(),
                  orderedValues.begin() + index * signaledValues[index].size());
    }
    std::sort(orderedValues.begin(), orderedValues.end());
    if (hasSignalFailure.load() || orderedValues.front() != 1 || orderedValues.back() != orderedValues.size())
    {
        return 27;
    }
    for (std::size_t index = 1; index < orderedValues.size(); ++index)
    {
        if (orderedValues[index] != orderedValues[index - 1] + 1)
        {
            return 28;
        }
    }

    // Copy Queue の実転送を Graphics Queue の GPU 待機に接続する
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
        return 16;
    }
    if (FAILED(upload->SetName(L"CueEngine QueuePool Test Upload Buffer")) ||
        FAILED(readback->SetName(L"CueEngine QueuePool Test Readback Buffer")))
    {
        return 25;
    }
    void* mapped = nullptr;
    D3D12_RANGE noRead{0, 0};
    if (FAILED(upload->Map(0, &noRead, &mapped)))
    {
        return 17;
    }
    std::memcpy(mapped, k_source.data(), sizeof(k_source));
    upload->Unmap(0, nullptr);

    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device->device()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->device()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, allocator.Get(), nullptr,
                                                    IID_PPV_ARGS(&list))))
    {
        return 18;
    }
    if (FAILED(allocator->SetName(L"CueEngine QueuePool Test Copy Allocator")) ||
        FAILED(list->SetName(L"CueEngine QueuePool Test Copy Command List")))
    {
        return 26;
    }
    list->CopyBufferRegion(readback.Get(), 0, upload.Get(), 0, sizeof(k_source));
    if (FAILED(list->Close()))
    {
        return 19;
    }
    auto* nativeCopy = dynamic_cast<cue::dx12::DX12GpuCommandQueue*>(copies[0].get());
    if (!nativeCopy)
    {
        return 20;
    }
    ID3D12CommandList* submitted[] = {list.Get()};
    auto transferFenceResult = nativeCopy->submit(submitted);
    if (!transferFenceResult.has_value() ||
        !graphics->wait_for_queue(*copies[0], transferFenceResult.take_value()).has_value())
    {
        return 21;
    }
    auto graphicsFenceResult = graphics->signal();
    if (!graphicsFenceResult.has_value() ||
        !graphics->wait_for_fence(graphicsFenceResult.take_value()).has_value())
    {
        return 22;
    }
    D3D12_RANGE readRange{0, sizeof(k_source)};
    if (FAILED(readback->Map(0, &readRange, &mapped)))
    {
        return 23;
    }
    const bool isCopied = std::memcmp(mapped, k_source.data(), sizeof(k_source)) == 0;
    readback->Unmap(0, &noRead);
    if (!isCopied)
    {
        return 24;
    }

    cue::IQueueContext* previousGraphics = graphics.get();
    for (std::size_t index = 0; index < computes.size(); ++index)
    {
        computes[index].reset();
        copies[index].reset();
    }
    graphics.reset();
    auto reusedResult = pool->acquire(cue::QueueType::Graphics);
    if (!reusedResult.has_value())
    {
        return 14;
    }
    auto reused = reusedResult.take_value();
    if (reused.get() != previousGraphics || pool->wait_idle().has_value())
    {
        return 15;
    }
    reused.reset();
    if (!pool->wait_idle().has_value() || !pool->shutdown().has_value() ||
        pool->acquire(cue::QueueType::Graphics).has_value())
    {
        return 15;
    }
    return 0;
}
