#include <DX12/DX12FrameGraphFrames.h>

#include <exception>
#include <memory>
#include <new>
#include <utility>
#include <vector>

#include <DX12/DX12FrameGraphResources.h>
#include <DX12/DX12GpuResource.h>
#include <DX12/DX12RenderDevice.h>
#include <Platform/Diagnostics.h>

namespace cue::dx12
{
/// @brief create の内部でだけ空の枠配列を構築する
DX12FrameGraphFrames::DX12FrameGraphFrames(CreateToken) noexcept
{
}

/// @brief Plan 内の一時 Texture すべてに枠ごとの View を割り当てる
Result<std::unique_ptr<DX12FrameGraphFrames>> DX12FrameGraphFrames::create(
    DX12RenderDevice& a_device, const FrameGraphPlan& a_plan, std::uint32_t a_frameCount,
    DX12DescriptorAllocator& a_rtvAllocator, DX12DescriptorAllocator& a_srvAllocator)
{
    using FramesResult = Result<std::unique_ptr<DX12FrameGraphFrames>>;
    if (!a_device.device() || a_plan.id() == 0 || a_frameCount == 0 ||
        a_rtvAllocator.type() != D3D12_DESCRIPTOR_HEAP_TYPE_RTV ||
        a_srvAllocator.type() != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV ||
        !a_srvAllocator.is_shader_visible())
    {
        return FramesResult::failure({ErrorCategory::InvalidArgument, "DX12FrameGraphFrames.create"});
    }

    try
    {
        std::vector<bool> needsRtv(a_plan.resources().size(), false);
        std::vector<bool> needsSrv(a_plan.resources().size(), false);
        for (const auto& planned : a_plan.resources())
        {
            if (!planned.isImported && planned.kind == GpuResourceKind::Texture2D &&
                planned.textureDesc.isShaderReadable)
            {
                needsSrv[planned.handle.index] = true;
            }
        }
        for (const auto& pass : a_plan.passes())
        {
            for (const auto& use : pass.uses)
            {
                if (use.resource.index >= a_plan.resources().size() ||
                    a_plan.resources()[use.resource.index].isImported)
                {
                    continue;
                }
                if (use.state == FrameGraphResourceState::RenderTarget)
                {
                    needsRtv[use.resource.index] = true;
                }
                if (use.state == FrameGraphResourceState::ShaderRead)
                {
                    needsSrv[use.resource.index] = true;
                }
            }
        }
        auto frames = std::make_unique<DX12FrameGraphFrames>(CreateToken{});
        frames->m_rtvAllocator = &a_rtvAllocator;
        frames->m_srvAllocator = &a_srvAllocator;
        frames->m_graphId = a_plan.resources().empty() ? 0 : a_plan.resources().front().handle.graphId;
        frames->m_frames.resize(a_frameCount);
        for (auto& frame : frames->m_frames)
        {
            frame.views.resize(a_plan.resources().size());
            auto graphResult = DX12FrameGraphResources::create(a_device, a_plan);
            if (!graphResult.has_value())
            {
                return FramesResult::failure(*graphResult.try_error());
            }
            frame.graph = graphResult.take_value();
            for (const auto& planned : a_plan.resources())
            {
                if (planned.isImported || planned.kind != GpuResourceKind::Texture2D ||
                    !planned.firstUse)
                {
                    continue;
                }
                auto* texture = frame.graph->resource(planned.handle);
                if (!texture || !texture->resource())
                {
                    return FramesResult::failure({ErrorCategory::InvalidState,
                                                  "DX12FrameGraphFrames.create.resource"});
                }
                auto* native = texture->resource();
                const auto nativeDesc = native->GetDesc();
                auto& views = frame.views[planned.handle.index];
                if (needsRtv[planned.handle.index])
                {
                    if ((nativeDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) == 0)
                    {
                        return FramesResult::failure({ErrorCategory::InvalidState,
                                                      "DX12FrameGraphFrames.create.flags"});
                    }
                    auto rtvResult = a_rtvAllocator.allocate();
                    if (!rtvResult.has_value())
                    {
                        return FramesResult::failure(*rtvResult.try_error());
                    }
                    views.rtv = rtvResult.take_value();
                    auto handleResult = a_rtvAllocator.cpu_handle(views.rtv);
                    if (!handleResult.has_value())
                    {
                        return FramesResult::failure(*handleResult.try_error());
                    }
                    a_device.device()->CreateRenderTargetView(native, nullptr, *handleResult.try_value());
                }

                if (!needsSrv[planned.handle.index])
                {
                    continue;
                }
                // ShaderRead 用の View を同じ論理 Handle に結び付ける
                auto srvResult = a_srvAllocator.allocate();
                if (!srvResult.has_value())
                {
                    return FramesResult::failure(*srvResult.try_error());
                }
                views.srv = srvResult.take_value();
                auto handleResult = a_srvAllocator.cpu_handle(views.srv);
                if (!handleResult.has_value())
                {
                    return FramesResult::failure(*handleResult.try_error());
                }
                D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
                srvDesc.Format = nativeDesc.Format;
                srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                srvDesc.Texture2D.MipLevels = nativeDesc.MipLevels;
                a_device.device()->CreateShaderResourceView(native, &srvDesc, *handleResult.try_value());
            }
        }
        return FramesResult::success(std::move(frames));
    }
    catch (const std::bad_alloc&)
    {
        return FramesResult::failure({ErrorCategory::PlatformFailure,
                                      "DX12FrameGraphFrames.create.allocation"});
    }
}

/// @brief 明示停止のない経路でも GPU 完了より前に Descriptor を返さない
DX12FrameGraphFrames::~DX12FrameGraphFrames()
{
    auto result = shutdown();
    if (!result.has_value())
    {
        report_error("DX12FrameGraphFrames.shutdown", *result.try_error(), DiagnosticSeverity::Fatal);
        std::terminate();
    }
}

/// @brief 同一枠の前回提出が完了してから記録を許可する
Result<void> DX12FrameGraphFrames::begin_frame(std::uint32_t a_frameIndex)
{
    if (m_isClosed || a_frameIndex >= m_frames.size())
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "DX12FrameGraphFrames.begin_frame"});
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
Result<void> DX12FrameGraphFrames::mark_submitted(
    std::uint32_t a_frameIndex, std::shared_ptr<ICommandCompletion> a_completion)
{
    if (m_isClosed || a_frameIndex >= m_frames.size() || !a_completion ||
        m_frames[a_frameIndex].completion)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "DX12FrameGraphFrames.mark_submitted"});
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

/// @brief 同じ Graph の一時 Resource だけを View 検索に使う
bool DX12FrameGraphFrames::owns(std::uint32_t a_frameIndex,
                                FrameGraphResourceHandle a_handle) const noexcept
{
    return !m_isClosed && a_handle.graphId != 0 && a_handle.graphId == m_graphId &&
           a_frameIndex < m_frames.size() && a_handle.index < m_frames[a_frameIndex].views.size();
}

/// @brief 指定枠の物理 Resource を借用する
DX12GpuResource* DX12FrameGraphFrames::resource(
    std::uint32_t a_frameIndex, FrameGraphResourceHandle a_handle) const noexcept
{
    return owns(a_frameIndex, a_handle) && m_frames[a_frameIndex].graph
               ? m_frames[a_frameIndex].graph->resource(a_handle)
               : nullptr;
}

/// @brief Pass 記録に必要な枠の Resource 表を貸す
DX12FrameGraphResources* DX12FrameGraphFrames::graph_resources(std::uint32_t a_frameIndex) const noexcept
{
    return !m_isClosed && a_frameIndex < m_frames.size() ? m_frames[a_frameIndex].graph.get() : nullptr;
}

/// @brief 論理 Handle に対応する RTV Slot の世代を検証する
Result<D3D12_CPU_DESCRIPTOR_HANDLE> DX12FrameGraphFrames::rtv(
    std::uint32_t a_frameIndex, FrameGraphResourceHandle a_handle) const
{
    if (!owns(a_frameIndex, a_handle) || !m_frames[a_frameIndex].views[a_handle.index].rtv.is_valid())
    {
        return Result<D3D12_CPU_DESCRIPTOR_HANDLE>::failure(
            {ErrorCategory::InvalidArgument, "DX12FrameGraphFrames.rtv"});
    }
    return m_rtvAllocator->cpu_handle(m_frames[a_frameIndex].views[a_handle.index].rtv);
}

/// @brief 論理 Handle に対応する SRV Slot の世代を検証する
Result<D3D12_GPU_DESCRIPTOR_HANDLE> DX12FrameGraphFrames::srv(
    std::uint32_t a_frameIndex, FrameGraphResourceHandle a_handle) const
{
    if (!owns(a_frameIndex, a_handle) || !m_frames[a_frameIndex].views[a_handle.index].srv.is_valid())
    {
        return Result<D3D12_GPU_DESCRIPTOR_HANDLE>::failure(
            {ErrorCategory::InvalidArgument, "DX12FrameGraphFrames.srv"});
    }
    return m_srvAllocator->gpu_handle(m_frames[a_frameIndex].views[a_handle.index].srv);
}

/// @brief Shader 可視 SRV と同じ Heap を返す
ID3D12DescriptorHeap* DX12FrameGraphFrames::srv_heap() const noexcept
{
    return !m_isClosed && m_srvAllocator ? m_srvAllocator->heap() : nullptr;
}

/// @brief 有効な描画枠数を返す
std::size_t DX12FrameGraphFrames::frame_count() const noexcept
{
    return m_isClosed ? 0 : m_frames.size();
}

/// @brief 全枠の GPU 完了後に Descriptor と Resource を解放する
Result<void> DX12FrameGraphFrames::shutdown()
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
        for (auto& views : frame.views)
        {
            if (views.rtv.is_valid())
            {
                auto result = m_rtvAllocator->release(views.rtv);
                if (!result.has_value())
                {
                    return result;
                }
                views.rtv = {};
            }
            if (views.srv.is_valid())
            {
                auto result = m_srvAllocator->release(views.srv);
                if (!result.has_value())
                {
                    return result;
                }
                views.srv = {};
            }
        }
    }
    m_frames.clear();
    m_rtvAllocator = nullptr;
    m_srvAllocator = nullptr;
    m_isClosed = true;
    return Result<void>::success();
}
} // namespace cue::dx12
