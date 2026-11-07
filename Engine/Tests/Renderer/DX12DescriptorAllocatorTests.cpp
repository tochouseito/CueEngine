#include <DX12/DX12DescriptorAllocator.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <thread>

#include <DX12/DX12RenderDevice.h>

/// @brief 実 WARP Device で Heap、容量上限、世代、複数 Thread の Slot 貸出を検証する
int main()
{
    auto deviceResult = cue::dx12::DX12RenderDevice::create(cue::dx12::AdapterSelection::Warp);
    if (!deviceResult.has_value())
    {
        return 1;
    }
    auto device = deviceResult.take_value();
    using Allocator = cue::dx12::DX12DescriptorAllocator;
    using Handle = cue::dx12::DX12DescriptorHandle;

    // Heap 構成の不正値は Native API に渡す前に拒否する
    if (Allocator::create(*device->device(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 0).has_value() ||
        Allocator::create(*device->device(), D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES, 1).has_value() ||
        Allocator::create(*device->device(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, true).has_value())
    {
        return 2;
    }

    auto rtvResult = Allocator::create(*device->device(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2);
    auto otherRtvResult = Allocator::create(*device->device(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1);
    auto dsvResult = Allocator::create(*device->device(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1);
    auto shaderResult = Allocator::create(*device->device(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2, true);
    auto samplerResult = Allocator::create(*device->device(), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 1, true);
    if (!rtvResult.has_value() || !otherRtvResult.has_value() || !dsvResult.has_value() ||
        !shaderResult.has_value() || !samplerResult.has_value())
    {
        return 3;
    }
    auto rtv = rtvResult.take_value();
    auto otherRtv = otherRtvResult.take_value();
    auto dsv = dsvResult.take_value();
    auto shader = shaderResult.take_value();
    auto sampler = samplerResult.take_value();
    if (!rtv->heap() || rtv->heap()->GetDesc().Type != D3D12_DESCRIPTOR_HEAP_TYPE_RTV ||
        rtv->is_shader_visible() || dsv->type() != D3D12_DESCRIPTOR_HEAP_TYPE_DSV ||
        !shader->is_shader_visible() ||
        shader->heap()->GetDesc().Flags != D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE ||
        !sampler->is_shader_visible())
    {
        return 4;
    }

    auto firstResult = rtv->allocate();
    auto secondResult = rtv->allocate();
    if (!firstResult.has_value() || !secondResult.has_value() || rtv->allocate().has_value())
    {
        return 5;
    }
    const Handle first = firstResult.take_value();
    const Handle second = secondResult.take_value();
    auto firstCpu = rtv->cpu_handle(first);
    auto secondCpu = rtv->cpu_handle(second);
    if (!first.is_valid() || !second.is_valid() || !firstCpu.has_value() || !secondCpu.has_value() ||
        secondCpu.try_value()->ptr - firstCpu.try_value()->ptr !=
            device->device()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV) ||
        rtv->gpu_handle(first).has_value() || otherRtv->cpu_handle(first).has_value() ||
        otherRtv->release(first).has_value())
    {
        return 6;
    }

    // 返却した Handle は CPU/GPU Handle 取得と二重返却の双方で拒否する
    if (!rtv->release(first).has_value() || rtv->cpu_handle(first).has_value() ||
        rtv->release(first).has_value())
    {
        return 7;
    }
    auto reusedResult = rtv->allocate();
    if (!reusedResult.has_value())
    {
        return 8;
    }
    const Handle reused = reusedResult.take_value();
    if (reused.index != first.index || reused.generation == first.generation ||
        rtv->cpu_handle(first).has_value() || !rtv->cpu_handle(reused).has_value() ||
        !rtv->release(second).has_value() || !rtv->release(reused).has_value())
    {
        return 9;
    }

    auto shaderSlotResult = shader->allocate();
    if (!shaderSlotResult.has_value())
    {
        return 10;
    }
    const Handle shaderSlot = shaderSlotResult.take_value();
    auto shaderCpu = shader->cpu_handle(shaderSlot);
    auto shaderGpu = shader->gpu_handle(shaderSlot);
    if (!shaderCpu.has_value() || !shaderGpu.has_value() || shaderCpu.try_value()->ptr == 0 ||
        shaderGpu.try_value()->ptr == 0 || !shader->release(shaderSlot).has_value() ||
        shader->gpu_handle(shaderSlot).has_value())
    {
        return 11;
    }

    // Update と Render が別 Thread から要求しても Slot が重複しない
    auto parallelResult = Allocator::create(*device->device(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 32);
    if (!parallelResult.has_value())
    {
        return 12;
    }
    auto parallel = parallelResult.take_value();
    std::array<Handle, 32> handles{};
    std::array<std::jthread, 4> workers{};
    std::atomic<bool> hasFailure = false;
    for (std::size_t worker = 0; worker < workers.size(); ++worker)
    {
        workers[worker] = std::jthread([&, worker] {
            for (std::size_t slot = 0; slot < 8; ++slot)
            {
                auto allocation = parallel->allocate();
                if (!allocation.has_value())
                {
                    hasFailure.store(true);
                    return;
                }
                handles[worker * 8 + slot] = allocation.take_value();
            }
        });
    }
    for (auto& worker : workers)
    {
        worker.join();
    }
    if (hasFailure || parallel->allocate().has_value())
    {
        return 13;
    }
    std::array<std::uint32_t, 32> indices{};
    for (std::size_t index = 0; index < handles.size(); ++index)
    {
        indices[index] = handles[index].index;
        if (!handles[index].is_valid() || !parallel->release(handles[index]).has_value())
        {
            return 14;
        }
    }
    std::sort(indices.begin(), indices.end());
    for (std::uint32_t index = 0; index < indices.size(); ++index)
    {
        if (indices[index] != index)
        {
            return 15;
        }
    }
    return 0;
}
