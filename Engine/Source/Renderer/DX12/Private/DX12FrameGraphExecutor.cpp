#include <DX12/DX12FrameGraphExecutor.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <vector>

#include <wrl/client.h>

#include <DX12/DX12CommandPool.h>
#include <DX12/DX12FrameGraphResources.h>
#include <DX12/DX12GpuResource.h>

namespace cue::dx12
{
namespace
{
/// @brief 論理 State を従来の ResourceBarrier API が受け取る State に変換する
bool native_state(FrameGraphResourceState a_state, D3D12_RESOURCE_STATES& a_native) noexcept
{
    switch (a_state)
    {
    case FrameGraphResourceState::Common:
        a_native = D3D12_RESOURCE_STATE_COMMON;
        return true;
    case FrameGraphResourceState::GenericRead:
        a_native = D3D12_RESOURCE_STATE_GENERIC_READ;
        return true;
    case FrameGraphResourceState::CopySource:
        a_native = D3D12_RESOURCE_STATE_COPY_SOURCE;
        return true;
    case FrameGraphResourceState::CopyDestination:
        a_native = D3D12_RESOURCE_STATE_COPY_DEST;
        return true;
    case FrameGraphResourceState::ShaderRead:
        a_native = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        return true;
    case FrameGraphResourceState::UnorderedAccess:
        a_native = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        return true;
    case FrameGraphResourceState::RenderTarget:
        a_native = D3D12_RESOURCE_STATE_RENDER_TARGET;
        return true;
    case FrameGraphResourceState::DepthRead:
        a_native = D3D12_RESOURCE_STATE_DEPTH_READ;
        return true;
    case FrameGraphResourceState::DepthWrite:
        a_native = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        return true;
    case FrameGraphResourceState::Present:
        a_native = D3D12_RESOURCE_STATE_PRESENT;
        return true;
    default:
        return false;
    }
}

/// @brief 外部 Texture の論理 Format と Native Format の一致を検証する
bool matches_format(GpuTextureFormat a_format, DXGI_FORMAT a_native) noexcept
{
    switch (a_format)
    {
    case GpuTextureFormat::Rgba8Unorm:
        return a_native == DXGI_FORMAT_R8G8B8A8_UNORM;
    case GpuTextureFormat::Bgra8Unorm:
        return a_native == DXGI_FORMAT_B8G8R8A8_UNORM;
    case GpuTextureFormat::Rgba16Float:
        return a_native == DXGI_FORMAT_R16G16B16A16_FLOAT;
    case GpuTextureFormat::R32Float:
        return a_native == DXGI_FORMAT_R32_FLOAT;
    default:
        return false;
    }
}

/// @brief 使用時と Graph 境界の State に必要な Resource Flag を確認する
bool supports_state(const D3D12_RESOURCE_DESC& a_desc, FrameGraphResourceState a_state) noexcept
{
    if (a_state == FrameGraphResourceState::RenderTarget)
    {
        return (a_desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0;
    }
    if (a_state == FrameGraphResourceState::DepthRead || a_state == FrameGraphResourceState::DepthWrite)
    {
        return (a_desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0;
    }
    if (a_state == FrameGraphResourceState::UnorderedAccess)
    {
        return (a_desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0;
    }
    return true;
}

/// @brief 誤った Native Resource を Binding しないよう形状と必要 Flag を調べる
bool matches_resource(const FrameGraphPlan& a_plan, const FrameGraphResourcePlan& a_planned,
                      ID3D12Resource& a_resource) noexcept
{
    const auto desc = a_resource.GetDesc();
    if (!supports_state(desc, a_planned.initialState) || !supports_state(desc, a_planned.finalState))
    {
        return false;
    }
    if (a_planned.kind == GpuResourceKind::Buffer)
    {
        if (desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || desc.Width < a_planned.bufferDesc.byteSize)
        {
            return false;
        }
        D3D12_HEAP_PROPERTIES heap{};
        if (FAILED(a_resource.GetHeapProperties(&heap, nullptr)))
        {
            return false;
        }
        const auto memory = a_planned.bufferDesc.memory;
        if ((memory == GpuMemoryUsage::Default && heap.Type != D3D12_HEAP_TYPE_DEFAULT) ||
            (memory == GpuMemoryUsage::Upload && heap.Type != D3D12_HEAP_TYPE_UPLOAD) ||
            (memory == GpuMemoryUsage::Readback && heap.Type != D3D12_HEAP_TYPE_READBACK))
        {
            return false;
        }
    }
    else if (a_planned.kind == GpuResourceKind::Texture2D)
    {
        if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
            desc.Width != a_planned.textureDesc.width || desc.Height != a_planned.textureDesc.height ||
            desc.MipLevels != a_planned.textureDesc.mipLevels || desc.DepthOrArraySize != 1 ||
            desc.SampleDesc.Count != 1 || !matches_format(a_planned.textureDesc.format, desc.Format))
        {
            return false;
        }
    }
    else
    {
        return false;
    }

    for (const auto& pass : a_plan.passes())
    {
        for (const auto& use : pass.uses)
        {
            if (use.resource.index != a_planned.handle.index)
            {
                continue;
            }
            if (!supports_state(desc, use.state))
            {
                return false;
            }
        }
    }
    return true;
}

/// @brief 検証済み論理 Barrier を Native Barrier へ変換する
bool native_barrier(const FrameGraphBarrierPlan& a_planned,
                    std::span<ID3D12Resource* const> a_resources,
                    std::uint64_t a_graphId, D3D12_RESOURCE_BARRIER& a_native) noexcept
{
    if (a_planned.resource.graphId != a_graphId || a_planned.resource.index >= a_resources.size())
    {
        return false;
    }
    auto* resource = a_resources[a_planned.resource.index];
    if (!resource)
    {
        return false;
    }
    if (a_planned.kind == FrameGraphBarrierKind::UnorderedAccess)
    {
        a_native.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        a_native.UAV.pResource = resource;
        return true;
    }
    if (a_planned.kind != FrameGraphBarrierKind::Transition ||
        !native_state(a_planned.before, a_native.Transition.StateBefore) ||
        !native_state(a_planned.after, a_native.Transition.StateAfter))
    {
        return false;
    }
    a_native.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    a_native.Transition.pResource = resource;
    a_native.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    return true;
}
} // namespace

/// @brief Executor の一回の記録に限って Native Resource 表を借りる
DX12FrameGraphPassContext::DX12FrameGraphPassContext(
    std::uint64_t a_graphId, std::span<ID3D12Resource* const> a_resources) noexcept
    : m_graphId(a_graphId), m_resources(a_resources)
{
}

/// @brief Callback 内で他 Graph の Handle を誤用した場合は Resource を渡さない
ID3D12Resource* DX12FrameGraphPassContext::resource(FrameGraphResourceHandle a_handle) const noexcept
{
    if (a_handle.graphId == 0 || a_handle.graphId != m_graphId || a_handle.index >= m_resources.size())
    {
        return nullptr;
    }
    return m_resources[a_handle.index];
}

/// @brief 不正な Binding や Barrier を記録前に拒否し、各 Pass の Activation を先に発行する
Result<void> DX12FrameGraphExecutor::record(const FrameGraphPlan& a_plan, DX12FrameGraphResources& a_resources,
                                            std::span<const DX12FrameGraphExternalResource> a_external,
                                            std::span<const dx12FrameGraphPassCallback> a_callbacks,
                                            DX12GpuCommandContext& a_context)
{
    return record_range(a_plan, a_resources, a_external, a_callbacks, a_context,
                        0, a_plan.passes().size(), true);
}

/// @brief 複数 Queue の提出単位ごとに同じ Plan の一部分を記録する
Result<void> DX12FrameGraphExecutor::record_range(
    const FrameGraphPlan& a_plan, DX12FrameGraphResources& a_resources,
    std::span<const DX12FrameGraphExternalResource> a_external,
    std::span<const dx12FrameGraphPassCallback> a_callbacks, DX12GpuCommandContext& a_context,
    std::size_t a_firstPass, std::size_t a_passCount, bool a_includeFinal)
{
    using RecordResult = Result<void>;
    if (!a_resources.matches_plan(a_plan) ||
        a_context.state() != CommandState::Recording ||
        !a_context.command_list() || a_callbacks.size() != a_plan.passes().size() ||
        a_plan.passes().size() > (std::numeric_limits<UINT>::max)() ||
        a_firstPass > a_plan.passes().size() ||
        a_passCount > a_plan.passes().size() - a_firstPass ||
        (a_includeFinal && a_context.type() != QueueType::Graphics))
    {
        return RecordResult::failure({ErrorCategory::InvalidArgument, "DX12FrameGraphExecutor.record.context"});
    }

    const std::uint64_t graphId = !a_plan.resources().empty() ? a_plan.resources().front().handle.graphId :
                                  !a_plan.passes().empty() ? a_plan.passes().front().handle.graphId : 0;
    Microsoft::WRL::ComPtr<ID3D12Device> commandDevice;
    const HRESULT deviceResult = a_context.command_list()->GetDevice(IID_PPV_ARGS(&commandDevice));
    if (FAILED(deviceResult))
    {
        return RecordResult::failure({ErrorCategory::PlatformFailure,
                                      "ID3D12GraphicsCommandList.GetDevice", deviceResult});
    }

    try
    {
        std::vector<ID3D12Resource*> nativeResources(a_plan.resources().size());
        std::vector<bool> hasBinding(a_plan.resources().size());
        for (std::size_t index = 0; index < a_plan.resources().size(); ++index)
        {
            const auto& planned = a_plan.resources()[index];
            if (planned.handle.graphId != graphId || planned.handle.index != index)
            {
                return RecordResult::failure({ErrorCategory::InvalidArgument,
                                              "DX12FrameGraphExecutor.record.plan"});
            }
            if (!planned.isImported && planned.firstUse)
            {
                auto* transient = a_resources.resource(planned.handle);
                if (!transient)
                {
                    return RecordResult::failure({ErrorCategory::InvalidArgument,
                                                  "DX12FrameGraphExecutor.record.transient"});
                }
                Microsoft::WRL::ComPtr<ID3D12Device> resourceDevice;
                if (FAILED(transient->resource()->GetDevice(IID_PPV_ARGS(&resourceDevice))) ||
                    resourceDevice.Get() != commandDevice.Get())
                {
                    return RecordResult::failure({ErrorCategory::InvalidArgument,
                                                  "DX12FrameGraphExecutor.record.transient_device"});
                }
                nativeResources[index] = transient->resource();
            }
        }
        for (const auto& binding : a_external)
        {
            if (binding.handle.graphId != graphId || binding.handle.index >= a_plan.resources().size() ||
                !a_plan.resources()[binding.handle.index].isImported ||
                hasBinding[binding.handle.index] || !binding.resource)
            {
                return RecordResult::failure({ErrorCategory::InvalidArgument,
                                              "DX12FrameGraphExecutor.record.binding"});
            }
            // 複数の論理 Handle が同じ Native Resource を指すと State 計画が分岐する
            for (const auto* bound : nativeResources)
            {
                if (bound == binding.resource)
                {
                    return RecordResult::failure({ErrorCategory::InvalidArgument,
                                                  "DX12FrameGraphExecutor.record.duplicate_resource"});
                }
            }
            Microsoft::WRL::ComPtr<ID3D12Device> resourceDevice;
            if (FAILED(binding.resource->GetDevice(IID_PPV_ARGS(&resourceDevice))) ||
                resourceDevice.Get() != commandDevice.Get() ||
                !matches_resource(a_plan, a_plan.resources()[binding.handle.index], *binding.resource))
            {
                return RecordResult::failure({ErrorCategory::InvalidArgument,
                                              "DX12FrameGraphExecutor.record.external_resource"});
            }
            nativeResources[binding.handle.index] = binding.resource;
            hasBinding[binding.handle.index] = true;
        }
        for (std::size_t index = 0; index < a_plan.resources().size(); ++index)
        {
            if (a_plan.resources()[index].isImported && !hasBinding[index])
            {
                return RecordResult::failure({ErrorCategory::InvalidArgument,
                                              "DX12FrameGraphExecutor.record.missing_binding"});
            }
        }

        std::vector<std::vector<D3D12_RESOURCE_BARRIER>> passBarriers(a_plan.passes().size());
        std::vector<D3D12_RESOURCE_BARRIER> finalBarriers;
        for (std::size_t index = a_firstPass; index < a_firstPass + a_passCount; ++index)
        {
            if (!a_callbacks[index] || a_plan.passes()[index].queue != a_context.type() ||
                a_plan.passes()[index].handle.graphId != graphId ||
                a_plan.passes()[index].barriersBefore.size() > (std::numeric_limits<UINT>::max)() ||
                a_resources.barriers_before_pass(index).size() > (std::numeric_limits<UINT>::max)())
            {
                return RecordResult::failure({ErrorCategory::InvalidArgument,
                                              "DX12FrameGraphExecutor.record.pass"});
            }
            for (const auto& barrier : a_plan.passes()[index].barriersBefore)
            {
                D3D12_RESOURCE_BARRIER native{};
                if (!native_barrier(barrier, nativeResources, graphId, native))
                {
                    return RecordResult::failure({ErrorCategory::InvalidArgument,
                                                  "DX12FrameGraphExecutor.record.barrier"});
                }
                passBarriers[index].push_back(native);
            }
        }
        if (a_includeFinal && a_plan.final_barriers().size() > (std::numeric_limits<UINT>::max)())
        {
            return RecordResult::failure({ErrorCategory::InvalidArgument,
                                          "DX12FrameGraphExecutor.record.final_barrier_count"});
        }
        for (const auto& barrier : a_plan.final_barriers())
        {
            if (!a_includeFinal)
            {
                break;
            }
            D3D12_RESOURCE_BARRIER native{};
            if (!native_barrier(barrier, nativeResources, graphId, native))
            {
                return RecordResult::failure({ErrorCategory::InvalidArgument,
                                              "DX12FrameGraphExecutor.record.final_barrier"});
            }
            finalBarriers.push_back(native);
        }

        DX12FrameGraphPassContext passContext(graphId, nativeResources);
        auto& list = *a_context.command_list();
        for (std::size_t index = a_firstPass; index < a_firstPass + a_passCount; ++index)
        {
            const auto aliasing = a_resources.barriers_before_pass(index);
            if (!aliasing.empty())
            {
                list.ResourceBarrier(static_cast<UINT>(aliasing.size()), aliasing.data());
            }
            const auto& transitions = passBarriers[index];
            if (!transitions.empty())
            {
                list.ResourceBarrier(static_cast<UINT>(transitions.size()), transitions.data());
            }
            auto callbackResult = a_callbacks[index](list, passContext);
            if (!callbackResult.has_value())
            {
                return callbackResult;
            }
        }
        if (!finalBarriers.empty())
        {
            list.ResourceBarrier(static_cast<UINT>(finalBarriers.size()), finalBarriers.data());
        }
        return RecordResult::success();
    }
    catch (const std::bad_alloc&)
    {
        return RecordResult::failure({ErrorCategory::PlatformFailure,
                                      "DX12FrameGraphExecutor.record.allocation"});
    }
}
} // namespace cue::dx12
