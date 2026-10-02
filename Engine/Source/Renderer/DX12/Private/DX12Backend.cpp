#include <DX12/DX12Backend.h>

#include <array>
#include <cstddef>
#include <utility>

#include <DX12/DX12CommandPool.h>
#include <DX12/DX12DescriptorAllocator.h>
#include <DX12/DX12GpuResourcePool.h>
#include <DX12/DX12RenderDevice.h>
#include <DX12/DX12QueuePool.h>
#include <DX12/DX12SwapChain.h>
#include <Platform/Diagnostics.h>

#include "DX12ResourceLeakChecker.h"

namespace cue::dx12
{
/// @brief Pool の Lease が残る間も Descriptor Heap を GPU 完了まで共有所有する
struct DX12DescriptorHeapState final
{
    std::array<std::unique_ptr<DX12DescriptorAllocator>, static_cast<std::size_t>(DX12DescriptorHeapRole::Count)>
        allocators;
};

/// @brief 検証済み Device の所有権を Backend へ移す
DX12Backend::DX12Backend(CreateToken, std::unique_ptr<DX12RenderDevice> a_device,
                         std::shared_ptr<DX12DescriptorHeapState> a_descriptors,
                         std::unique_ptr<DX12QueuePool> a_queuePool,
                         std::unique_ptr<DX12CommandPool> a_commandPool,
                         std::unique_ptr<DX12GpuResourcePool> a_resourcePool) noexcept
    : m_device(std::move(a_device)), m_descriptors(std::move(a_descriptors)), m_queuePool(std::move(a_queuePool)),
      m_commandPool(std::move(a_commandPool)), m_resourcePool(std::move(a_resourcePool))
{
}

/// @brief Device、全 Descriptor Heap、各 Pool が揃うまで Backend を公開しない
Result<std::unique_ptr<DX12Backend>> DX12Backend::create(const DX12DescriptorHeapConfig& a_descriptorConfig)
{
    using BackendResult = Result<std::unique_ptr<DX12Backend>>;
    if (a_descriptorConfig.cpuViewCapacity == 0 || a_descriptorConfig.shaderViewCapacity == 0 ||
        a_descriptorConfig.cpuSamplerCapacity == 0 || a_descriptorConfig.shaderSamplerCapacity == 0 ||
        a_descriptorConfig.rtvCapacity == 0 || a_descriptorConfig.dsvCapacity == 0)
    {
        return BackendResult::failure({ErrorCategory::InvalidArgument, "DX12Backend.create.descriptor_config"});
    }

    auto deviceResult = DX12RenderDevice::create();
    if (!deviceResult.has_value())
    {
        return BackendResult::failure(*deviceResult.try_error());
    }

    // View の種類ごとに CPU 保管領域と GPU Bind 用領域を分ける
    struct HeapSpec final
    {
        D3D12_DESCRIPTOR_HEAP_TYPE type;
        std::uint32_t capacity;
        bool isShaderVisible;
    };
    const std::array heapSpecs = {
        HeapSpec{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, a_descriptorConfig.cpuViewCapacity, false},
        HeapSpec{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, a_descriptorConfig.shaderViewCapacity, true},
        HeapSpec{D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, a_descriptorConfig.cpuSamplerCapacity, false},
        HeapSpec{D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, a_descriptorConfig.shaderSamplerCapacity, true},
        HeapSpec{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, a_descriptorConfig.rtvCapacity, false},
        HeapSpec{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, a_descriptorConfig.dsvCapacity, false},
    };
    static_assert(heapSpecs.size() == static_cast<std::size_t>(DX12DescriptorHeapRole::Count));
    auto descriptors = std::make_shared<DX12DescriptorHeapState>();
    for (std::size_t index = 0; index < heapSpecs.size(); ++index)
    {
        const HeapSpec& spec = heapSpecs[index];
        auto descriptorResult = DX12DescriptorAllocator::create(*(*deviceResult.try_value())->device(), spec.type,
                                                                spec.capacity, spec.isShaderVisible);
        if (!descriptorResult.has_value())
        {
            return BackendResult::failure(*descriptorResult.try_error());
        }
        descriptors->allocators[index] = descriptorResult.take_value();
    }

    // Device に依存する Queue を全種類生成し、失敗時は部分所有を公開しない
    auto queuePoolResult = DX12QueuePool::create(**deviceResult.try_value(), descriptors);
    if (!queuePoolResult.has_value())
    {
        return BackendResult::failure(*queuePoolResult.try_error());
    }

    // CommandPool は Queue を固定せず、提出時に呼出側が借りた Queue を受け取る
    auto commandPoolResult = DX12CommandPool::create(**deviceResult.try_value(), descriptors);
    if (!commandPoolResult.has_value())
    {
        return BackendResult::failure(*commandPoolResult.try_error());
    }

    // ResourcePool は Device を保持し、生成後の GPU Resource を一元管理する
    auto resourcePoolResult = DX12GpuResourcePool::create(**deviceResult.try_value());
    if (!resourcePoolResult.has_value())
    {
        return BackendResult::failure(*resourcePoolResult.try_error());
    }

    return BackendResult::success(std::make_unique<DX12Backend>(
        CreateToken{}, deviceResult.take_value(), std::move(descriptors), queuePoolResult.take_value(),
        commandPoolResult.take_value(), resourcePoolResult.take_value()));
}

/// @brief 明示停止されていない Queue も GPU 完了後に破棄する
DX12Backend::~DX12Backend()
{
    auto result = shutdown();
    if (!result.has_value())
    {
        report_error("DX12Backend.shutdown", *result.try_error(), DiagnosticSeverity::Error);
    }
}

/// @brief 新規貸出を止め、借用中の Queue の寿命を Lease に引き継ぐ
Result<void> DX12Backend::shutdown()
{
    // CommandPool の Slot 完了を先に確認してから QueuePool を停止する
    const bool hasOwnedDevice = m_device != nullptr;
    Result<void> stopResult = Result<void>::success();
    if (m_swapChain)
    {
        stopResult = m_swapChain->shutdown();
        if (!stopResult.has_value())
        {
            return stopResult;
        }
        m_swapChain.reset();
    }
    if (m_resourcePool)
    {
        stopResult = m_resourcePool->shutdown();
        m_resourcePool.reset();
    }
    if (m_commandPool)
    {
        auto commandStopResult = m_commandPool->shutdown();
        if (stopResult.has_value())
        {
            stopResult = std::move(commandStopResult);
        }
        m_commandPool.reset();
    }
    if (m_queuePool)
    {
        auto queueStopResult = m_queuePool->shutdown();
        if (stopResult.has_value())
        {
            stopResult = std::move(queueStopResult);
        }
        m_queuePool.reset();
    }
    // 借用が残る失敗経路では Pool の共有状態が GPU 完了まで Heap を保持する
    m_descriptors.reset();
    m_device.reset();

    // 借用中の Object が残る失敗経路では誤検出を避け、正常停止した最初の一回だけ報告する
    if (hasOwnedDevice && stopResult.has_value())
    {
        DX12ResourceLeakChecker::report_live_objects();
    }
    return stopResult;
}

/// @brief Backend が稼働する間だけ Device を借用させる
IRenderDevice* DX12Backend::get_render_device() noexcept
{
    return m_device.get();
}

/// @brief Backend が所有する QueuePool の借用 Pointer を返す
IQueuePool* DX12Backend::get_queue_pool() noexcept
{
    return m_queuePool.get();
}

/// @brief Backend が所有する CommandPool の借用 Pointer を返す
ICommandPool* DX12Backend::get_command_pool() noexcept
{
    return m_commandPool.get();
}

/// @brief Backend が所有する ResourcePool の借用 Pointer を返す
IGpuResourcePool* DX12Backend::get_resource_pool() noexcept
{
    return m_resourcePool.get();
}

/// @brief 用途に対応する Heap の Allocator を Backend 稼働中だけ貸す
DX12DescriptorAllocator* DX12Backend::get_descriptor_allocator(DX12DescriptorHeapRole a_role) noexcept
{
    const std::size_t index = static_cast<std::size_t>(a_role);
    return m_descriptors && index < m_descriptors->allocators.size() ? m_descriptors->allocators[index].get() : nullptr;
}

/// @brief Graphics Queue と RTV Allocator を Backend の所有下に保持する
Result<void> DX12Backend::create_swap_chain(void* a_windowHandle, const DX12SwapChainConfig& a_config)
{
    if (!m_device || !m_queuePool || m_swapChain)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "DX12Backend.create_swap_chain"});
    }
    auto* rtvAllocator = get_descriptor_allocator(DX12DescriptorHeapRole::Rtv);
    if (!rtvAllocator)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "DX12Backend.create_swap_chain.rtv"});
    }
    auto leaseResult = m_queuePool->acquire(QueueType::Graphics);
    if (!leaseResult.has_value())
    {
        return Result<void>::failure(*leaseResult.try_error());
    }
    auto result = DX12SwapChain::create(*m_device, leaseResult.take_value(), *rtvAllocator, a_windowHandle, a_config);
    if (!result.has_value())
    {
        return Result<void>::failure(*result.try_error());
    }
    m_swapChain = result.take_value();
    return Result<void>::success();
}

/// @brief Backend 停止後の借用を拒否する
DX12SwapChain* DX12Backend::get_swap_chain() noexcept
{
    return m_swapChain.get();
}
} // namespace cue::dx12
