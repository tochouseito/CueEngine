#include <DX12/DX12FinalColorFrames.h>

#include <exception>
#include <memory>
#include <new>
#include <utility>

#include <DX12/DX12FrameGraphResources.h>
#include <DX12/DX12GpuResource.h>
#include <DX12/DX12RenderDevice.h>
#include <Platform/Diagnostics.h>

namespace cue::dx12
{
/// @brief create の内部でだけ空の枠配列を構築する
DX12FinalColorFrames::DX12FinalColorFrames(CreateToken) noexcept
{
}

/// @brief FinalColor の Native Resource と Descriptor が全枠で揃った場合だけ公開する
Result<std::unique_ptr<DX12FinalColorFrames>> DX12FinalColorFrames::create(
    DX12RenderDevice& a_device, const FrameGraphPlan& a_plan, FrameGraphResourceHandle a_finalColor,
    std::uint32_t a_frameCount, DX12DescriptorAllocator& a_rtvAllocator,
    DX12DescriptorAllocator& a_srvAllocator)
{
    using FramesResult = Result<std::unique_ptr<DX12FinalColorFrames>>;
    if (!a_device.device() || !a_finalColor.is_valid() || a_frameCount == 0 ||
        a_finalColor.index >= a_plan.resources().size() ||
        a_plan.resources()[a_finalColor.index].handle.graphId != a_finalColor.graphId ||
        a_plan.resources()[a_finalColor.index].kind != GpuResourceKind::Texture2D ||
        !a_plan.resources()[a_finalColor.index].textureDesc.isRenderTarget ||
        !a_plan.resources()[a_finalColor.index].firstUse ||
        a_rtvAllocator.type() != D3D12_DESCRIPTOR_HEAP_TYPE_RTV ||
        a_srvAllocator.type() != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV ||
        !a_srvAllocator.is_shader_visible())
    {
        return FramesResult::failure({ErrorCategory::InvalidArgument, "DX12FinalColorFrames.create"});
    }

    try
    {
        auto frames = std::make_unique<DX12FinalColorFrames>(CreateToken{});
        frames->m_rtvAllocator = &a_rtvAllocator;
        frames->m_srvAllocator = &a_srvAllocator;
        frames->m_finalColor = a_finalColor;
        frames->m_frames.resize(a_frameCount);
        for (auto& frame : frames->m_frames)
        {
            auto graphResult = DX12FrameGraphResources::create(a_device, a_plan);
            if (!graphResult.has_value())
            {
                return FramesResult::failure(*graphResult.try_error());
            }
            frame.graph = graphResult.take_value();
            auto* color = frame.graph->resource(a_finalColor);
            if (!color || !color->resource())
            {
                return FramesResult::failure({ErrorCategory::InvalidState,
                                              "DX12FinalColorFrames.create.resource"});
            }
            auto* native = color->resource();
            const auto nativeDesc = native->GetDesc();
            if ((nativeDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) == 0)
            {
                return FramesResult::failure({ErrorCategory::InvalidState,
                                              "DX12FinalColorFrames.create.flags"});
            }

            auto rtvResult = a_rtvAllocator.allocate();
            if (!rtvResult.has_value())
            {
                return FramesResult::failure(*rtvResult.try_error());
            }
            frame.rtvHandle = rtvResult.take_value();
            auto rtvHandleResult = a_rtvAllocator.cpu_handle(frame.rtvHandle);
            if (!rtvHandleResult.has_value())
            {
                return FramesResult::failure(*rtvHandleResult.try_error());
            }
            a_device.device()->CreateRenderTargetView(native, nullptr, *rtvHandleResult.try_value());

            auto srvResult = a_srvAllocator.allocate();
            if (!srvResult.has_value())
            {
                return FramesResult::failure(*srvResult.try_error());
            }
            frame.srvHandle = srvResult.take_value();
            auto srvHandleResult = a_srvAllocator.cpu_handle(frame.srvHandle);
            if (!srvHandleResult.has_value())
            {
                return FramesResult::failure(*srvHandleResult.try_error());
            }
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
            srvDesc.Format = nativeDesc.Format;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Texture2D.MipLevels = nativeDesc.MipLevels;
            a_device.device()->CreateShaderResourceView(native, &srvDesc, *srvHandleResult.try_value());
        }
        return FramesResult::success(std::move(frames));
    }
    catch (const std::bad_alloc&)
    {
        return FramesResult::failure({ErrorCategory::PlatformFailure,
                                      "DX12FinalColorFrames.create.allocation"});
    }
}

/// @brief 明示停止のない経路でも GPU 完了より前に Descriptor を返さない
DX12FinalColorFrames::~DX12FinalColorFrames()
{
    auto result = shutdown();
    if (!result.has_value())
    {
        report_error("DX12FinalColorFrames.shutdown", *result.try_error(), DiagnosticSeverity::Fatal);
        std::terminate();
    }
}

/// @brief 同一枠の前回提出が完了してから記録を許可する
Result<void> DX12FinalColorFrames::begin_frame(std::uint32_t a_frameIndex)
{
    if (m_isClosed || a_frameIndex >= m_frames.size())
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "DX12FinalColorFrames.begin_frame"});
    }
    auto& frame = m_frames[a_frameIndex];
    if (frame.completion)
    {
        auto waitResult = frame.completion->wait();
        if (!waitResult.has_value())
        {
            return waitResult;
        }
        frame.completion.reset();
    }
    return Result<void>::success();
}

/// @brief Graph Resource と枠の両方に同じ完了点を保持する
Result<void> DX12FinalColorFrames::mark_submitted(
    std::uint32_t a_frameIndex, std::shared_ptr<ICommandCompletion> a_completion)
{
    if (m_isClosed || a_frameIndex >= m_frames.size() || !a_completion ||
        m_frames[a_frameIndex].completion)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "DX12FinalColorFrames.mark_submitted"});
    }
    auto& frame = m_frames[a_frameIndex];
    auto markResult = frame.graph->mark_submitted(a_completion);
    if (!markResult.has_value())
    {
        return markResult;
    }
    frame.completion = std::move(a_completion);
    return Result<void>::success();
}

/// @brief 論理 Handle に対応する指定枠の物理 Texture を貸す
DX12GpuResource* DX12FinalColorFrames::resource(std::uint32_t a_frameIndex) const noexcept
{
    return !m_isClosed && a_frameIndex < m_frames.size() && m_frames[a_frameIndex].graph
               ? m_frames[a_frameIndex].graph->resource(m_finalColor)
               : nullptr;
}

/// @brief Pass 記録に必要な枠の Resource 表を貸す
DX12FrameGraphResources* DX12FinalColorFrames::graph_resources(std::uint32_t a_frameIndex) const noexcept
{
    return !m_isClosed && a_frameIndex < m_frames.size() ? m_frames[a_frameIndex].graph.get() : nullptr;
}

/// @brief 枠の RTV Slot の世代を検証する
Result<D3D12_CPU_DESCRIPTOR_HANDLE> DX12FinalColorFrames::rtv(std::uint32_t a_frameIndex) const
{
    if (m_isClosed || a_frameIndex >= m_frames.size() || !m_rtvAllocator)
    {
        return Result<D3D12_CPU_DESCRIPTOR_HANDLE>::failure(
            {ErrorCategory::InvalidArgument, "DX12FinalColorFrames.rtv"});
    }
    return m_rtvAllocator->cpu_handle(m_frames[a_frameIndex].rtvHandle);
}

/// @brief 枠の Shader 可視 SRV Slot の世代を検証する
Result<D3D12_GPU_DESCRIPTOR_HANDLE> DX12FinalColorFrames::srv(std::uint32_t a_frameIndex) const
{
    if (m_isClosed || a_frameIndex >= m_frames.size() || !m_srvAllocator)
    {
        return Result<D3D12_GPU_DESCRIPTOR_HANDLE>::failure(
            {ErrorCategory::InvalidArgument, "DX12FinalColorFrames.srv"});
    }
    return m_srvAllocator->gpu_handle(m_frames[a_frameIndex].srvHandle);
}

/// @brief 有効な描画枠数を返す
std::size_t DX12FinalColorFrames::frame_count() const noexcept
{
    return m_isClosed ? 0 : m_frames.size();
}

/// @brief 全 Graph の GPU 完了後に Descriptor と Resource を解放する
Result<void> DX12FinalColorFrames::shutdown()
{
    if (m_isClosed)
    {
        return Result<void>::success();
    }
    for (auto& frame : m_frames)
    {
        if (frame.graph)
        {
            auto result = frame.graph->shutdown();
            if (!result.has_value())
            {
                return result;
            }
        }
    }
    for (auto& frame : m_frames)
    {
        if (frame.rtvHandle.is_valid())
        {
            auto result = m_rtvAllocator->release(frame.rtvHandle);
            if (!result.has_value())
            {
                return result;
            }
            frame.rtvHandle = {};
        }
        if (frame.srvHandle.is_valid())
        {
            auto result = m_srvAllocator->release(frame.srvHandle);
            if (!result.has_value())
            {
                return result;
            }
            frame.srvHandle = {};
        }
    }
    m_frames.clear();
    m_rtvAllocator = nullptr;
    m_srvAllocator = nullptr;
    m_isClosed = true;
    return Result<void>::success();
}
} // namespace cue::dx12
