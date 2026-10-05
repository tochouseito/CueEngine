#include <DX12/DX12Backend.h>

#include <array>
#include <cstddef>
#include <new>
#include <utility>

#include <DX12/DX12CommandPool.h>
#include <DX12/DX12DescriptorAllocator.h>
#include <DX12/DX12GpuResourcePool.h>
#include <DX12/DX12PipelineManager.h>
#include <DX12/DX12QueuePool.h>
#include <DX12/DX12RenderDevice.h>
#include <DX12/DX12SwapChain.h>
#include <DX12/DX12ViewManager.h>
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

/// @brief Factory が成功するまで空の所有状態を保持する
DX12Backend::DX12Backend(CreateToken) noexcept
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

    auto pipelineResult = DX12PipelineManager::create(**deviceResult.try_value());
    if (!pipelineResult.has_value())
        return BackendResult::failure(*pipelineResult.try_error());

    auto viewResult = DX12ViewManager::create(
        **deviceResult.try_value(), *descriptors->allocators[static_cast<std::size_t>(DX12DescriptorHeapRole::Rtv)],
        *descriptors->allocators[static_cast<std::size_t>(DX12DescriptorHeapRole::ShaderView)]);
    if (!viewResult.has_value())
        return BackendResult::failure(*viewResult.try_error());
    try
    {
        auto backend = std::make_unique<DX12Backend>(CreateToken{});
        backend->m_device = deviceResult.take_value();
        backend->m_descriptors = std::move(descriptors);
        backend->m_viewManager = viewResult.take_value();
        backend->m_pipelineManager = pipelineResult.take_value();
        backend->m_queuePool = queuePoolResult.take_value();
        backend->m_commandPool = commandPoolResult.take_value();
        backend->m_resourcePool = resourcePoolResult.take_value();
        backend->m_resourceContext.emplace(
            DX12ResourceContext{*backend->m_device, *backend->m_viewManager, *backend->m_pipelineManager});
        backend->m_executionContext.emplace(DX12ExecutionContext{*backend->m_commandPool, *backend->m_queuePool});
        return BackendResult::success(std::move(backend));
    }
    catch (const std::bad_alloc &)
    {
        return BackendResult::failure({ErrorCategory::PlatformFailure, "DX12Backend.create.allocation"});
    }
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
    // Graph と SwapChain の停止後に借用入口を閉じ、Manager は Heap より先に破棄する
    m_resourceContext.reset();
    m_executionContext.reset();
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
    m_pipelineManager.reset();
    m_viewManager.reset();
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

/// @brief 全生成基盤が揃う稼働期間だけ Resource Context を貸す
const DX12ResourceContext *DX12Backend::get_resource_context() const noexcept
{
    return m_resourceContext ? &*m_resourceContext : nullptr;
}

/// @brief Pool の生成後から停止まで Execution Context を貸す
const DX12ExecutionContext *DX12Backend::get_execution_context() const noexcept
{
    return m_executionContext ? &*m_executionContext : nullptr;
}

/// @brief Graphics Queue と RTV Allocator を Backend の所有下に保持する
Result<void> DX12Backend::create_swap_chain(void* a_windowHandle, const DX12SwapChainConfig& a_config)
{
    // SwapChain は Backend が所有する Graphics Queue と RTV Allocator を借用する
    if (!m_device || !m_queuePool || m_swapChain)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "DX12Backend.create_swap_chain"});
    }
    const auto *resources = get_resource_context();
    if (!resources)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "DX12Backend.create_swap_chain.context"});
    }
    auto leaseResult = m_queuePool->acquire(QueueType::Graphics);
    if (!leaseResult.has_value())
    {
        return Result<void>::failure(*leaseResult.try_error());
    }

    // SwapChain を 生成する
    auto result = DX12SwapChain::create(*resources, leaseResult.take_value(), a_windowHandle, a_config);
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
