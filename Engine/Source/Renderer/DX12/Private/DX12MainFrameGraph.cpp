#include <DX12/DX12MainFrameGraph.h>

#include <array>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <new>
#include <utility>

#include <DX12/DX12CommandPool.h>
#include <DX12/DX12DescriptorAllocator.h>
#include <DX12/DX12FinalColorFrames.h>
#include <DX12/DX12FrameGraphExecutor.h>
#include <DX12/DX12FullscreenPipeline.h>
#include <DX12/DX12GpuResource.h>
#include <DX12/DX12RenderDevice.h>
#include <DX12/DX12SwapChain.h>
#include <Platform/Diagnostics.h>

namespace cue::dx12
{
/// @brief 二つの固定 Pass と枠ごとの描画資源を受け取る
DX12MainFrameGraph::DX12MainFrameGraph(CreateToken, FrameGraphPlan a_plan, FrameGraphResourceHandle a_finalColor,
                                       FrameGraphResourceHandle a_backBuffer, FrameGraphPassHandle a_clearPass,
                                       FrameGraphPassHandle a_displayPass,
                                       std::vector<dx12FrameGraphPassCallback> a_customCallbacks,
                                       std::unique_ptr<DX12FinalColorFrames> a_frames,
                                       std::unique_ptr<DX12FullscreenPipeline> a_pipeline, DX12SwapChain &a_swapChain,
                                       DX12DescriptorAllocator &a_srvAllocator, std::array<float, 4> a_clearColor,
                                       std::uint32_t a_width, std::uint32_t a_height) noexcept
    : m_plan(std::move(a_plan)), m_finalColor(a_finalColor), m_backBuffer(a_backBuffer), m_clearPass(a_clearPass),
      m_displayPass(a_displayPass), m_customCallbacks(std::move(a_customCallbacks)), m_frames(std::move(a_frames)),
      m_pipeline(std::move(a_pipeline)), m_swapChain(&a_swapChain), m_srvAllocator(&a_srvAllocator),
      m_clearColor(a_clearColor), m_width(a_width), m_height(a_height)
{
}

/// @brief Clear を先頭、表示を最後に固定し、Back Buffer を Present 境界で Import する
Result<std::unique_ptr<DX12MainFrameGraph>> DX12MainFrameGraph::create(
    DX12RenderDevice &a_device, DX12SwapChain &a_swapChain, std::uint32_t a_frameCount,
    DX12DescriptorAllocator &a_rtvAllocator, DX12DescriptorAllocator &a_srvAllocator, std::array<float, 4> a_clearColor,
    dx12MainGraphConfigure a_configure)
{
    using GraphResult = Result<std::unique_ptr<DX12MainFrameGraph>>;
    auto *backBuffer = a_swapChain.back_buffer(0);
    if (!a_device.device() || !backBuffer || !a_swapChain.graphics_queue() || a_frameCount == 0)
    {
        return GraphResult::failure({ErrorCategory::InvalidArgument, "DX12MainFrameGraph.create"});
    }
    const auto nativeDesc = backBuffer->GetDesc();
    if (nativeDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || nativeDesc.Width == 0 ||
        nativeDesc.Width > (std::numeric_limits<std::uint32_t>::max)() || nativeDesc.Height == 0 ||
        nativeDesc.Height > (std::numeric_limits<std::uint32_t>::max)() || nativeDesc.MipLevels != 1 ||
        nativeDesc.DepthOrArraySize != 1 || nativeDesc.SampleDesc.Count != 1 ||
        (nativeDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) == 0 ||
        (nativeDesc.Format != DXGI_FORMAT_R8G8B8A8_UNORM && nativeDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM))
    {
        return GraphResult::failure({ErrorCategory::InvalidArgument, "DX12MainFrameGraph.create.back_buffer"});
    }
    GpuTexture2DDesc colorDesc{static_cast<std::uint32_t>(nativeDesc.Width),
                               static_cast<std::uint32_t>(nativeDesc.Height)};
    colorDesc.format =
        nativeDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM ? GpuTextureFormat::Rgba8Unorm : GpuTextureFormat::Bgra8Unorm;
    colorDesc.isRenderTarget = true;
    colorDesc.clearColor = a_clearColor;
    auto builderResult = FrameGraphBuilder::create_main(colorDesc);
    if (!builderResult.has_value())
    {
        return GraphResult::failure(*builderResult.try_error());
    }
    auto builder = builderResult.take_value();
    const auto finalColor = builder->final_color();
    auto backResult =
        builder->import_texture2d(colorDesc, FrameGraphResourceState::Present, FrameGraphResourceState::Present);
    if (!backResult.has_value())
    {
        return GraphResult::failure(*backResult.try_error());
    }
    const auto backHandle = backResult.take_value();
    auto clearResult = builder->add_pass("ClearFinalColor");
    if (!clearResult.has_value())
    {
        return GraphResult::failure(*clearResult.try_error());
    }
    const auto clearPass = clearResult.take_value();
    auto clearUseResult =
        builder->use(clearPass, finalColor, FrameGraphAccess::Write, FrameGraphResourceState::RenderTarget);
    if (!clearUseResult.has_value())
    {
        return GraphResult::failure(*clearUseResult.try_error());
    }
    try
    {
        std::vector<DX12MainGraphPass> customPasses;
        if (a_configure)
        {
            auto configureResult = a_configure(*builder, finalColor, customPasses);
            if (!configureResult.has_value())
            {
                return GraphResult::failure(*configureResult.try_error());
            }
        }
        auto displayResult = builder->add_pass("DisplayFinalColor");
        if (!displayResult.has_value())
        {
            return GraphResult::failure(*displayResult.try_error());
        }
        const auto displayPass = displayResult.take_value();
        auto displayReadResult =
            builder->use(displayPass, finalColor, FrameGraphAccess::Read, FrameGraphResourceState::ShaderRead);
        if (!displayReadResult.has_value())
        {
            return GraphResult::failure(*displayReadResult.try_error());
        }
        auto displayWriteResult =
            builder->use(displayPass, backHandle, FrameGraphAccess::Write, FrameGraphResourceState::RenderTarget);
        if (!displayWriteResult.has_value())
        {
            return GraphResult::failure(*displayWriteResult.try_error());
        }
        auto dependencyResult = builder->depends_on(displayPass, clearPass);
        if (!dependencyResult.has_value())
        {
            return GraphResult::failure(*dependencyResult.try_error());
        }
        for (const auto &pass : customPasses)
        {
            if (pass.handle.graphId != clearPass.graphId || pass.handle.index <= clearPass.index ||
                pass.handle.index >= displayPass.index || !pass.callback)
            {
                return GraphResult::failure({ErrorCategory::InvalidArgument, "DX12MainFrameGraph.create.custom_pass"});
            }
            auto afterClearResult = builder->depends_on(pass.handle, clearPass);
            if (!afterClearResult.has_value())
            {
                return GraphResult::failure(*afterClearResult.try_error());
            }
            auto beforeDisplayResult = builder->depends_on(displayPass, pass.handle);
            if (!beforeDisplayResult.has_value())
            {
                return GraphResult::failure(*beforeDisplayResult.try_error());
            }
        }
        auto planResult = builder->build();
        if (!planResult.has_value())
        {
            return GraphResult::failure(*planResult.try_error());
        }
        auto plan = planResult.take_value();
        if (plan.passes().size() != customPasses.size() + 2 || plan.passes().front().handle.index != clearPass.index ||
            plan.passes().back().handle.index != displayPass.index || plan.final_barriers().size() != 2)
        {
            return GraphResult::failure({ErrorCategory::InvalidState, "DX12MainFrameGraph.create.plan"});
        }
        std::vector<dx12FrameGraphPassCallback> customCallbacks(plan.passes().size());
        for (auto &pass : customPasses)
        {
            if (customCallbacks[pass.handle.index])
            {
                return GraphResult::failure(
                    {ErrorCategory::InvalidArgument, "DX12MainFrameGraph.create.duplicate_callback"});
            }
            customCallbacks[pass.handle.index] = std::move(pass.callback);
        }
        for (const auto &pass : plan.passes())
        {
            if (pass.handle.index != clearPass.index && pass.handle.index != displayPass.index &&
                !customCallbacks[pass.handle.index])
            {
                return GraphResult::failure(
                    {ErrorCategory::InvalidArgument, "DX12MainFrameGraph.create.missing_callback"});
            }
        }
        auto framesResult =
            DX12FinalColorFrames::create(a_device, plan, finalColor, a_frameCount, a_rtvAllocator, a_srvAllocator);
        if (!framesResult.has_value())
        {
            return GraphResult::failure(*framesResult.try_error());
        }
        auto pipelineResult = DX12FullscreenPipeline::create(a_device, nativeDesc.Format);
        if (!pipelineResult.has_value())
        {
            return GraphResult::failure(*pipelineResult.try_error());
        }
        return GraphResult::success(std::make_unique<DX12MainFrameGraph>(
            CreateToken{}, std::move(plan), finalColor, backHandle, clearPass, displayPass, std::move(customCallbacks),
            framesResult.take_value(), pipelineResult.take_value(), a_swapChain, a_srvAllocator, a_clearColor,
            colorDesc.width, colorDesc.height));
    }
    catch (const std::bad_alloc &)
    {
        return GraphResult::failure({ErrorCategory::PlatformFailure, "DX12MainFrameGraph.create.allocation"});
    }
}

/// @brief 明示停止されなかった枠も GPU 完了後に破棄する
DX12MainFrameGraph::~DX12MainFrameGraph()
{
    auto result = shutdown();
    if (!result.has_value())
    {
        report_error("DX12MainFrameGraph.shutdown", *result.try_error(), DiagnosticSeverity::Fatal);
        std::terminate();
    }
}

/// @brief Graph の計画を Executor に渡し、固定 Pass の Command を記録する
Result<void> DX12MainFrameGraph::record(std::uint32_t a_frameIndex, DX12GpuCommandContext &a_context)
{
    if (!m_frames || !m_pipeline || !m_swapChain || !m_srvAllocator || !m_srvAllocator->heap() ||
        a_frameIndex >= m_frames->frame_count() || a_context.type() != QueueType::Graphics ||
        a_context.state() != CommandState::Recording)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "DX12MainFrameGraph.record"});
    }
    const auto backIndex = m_swapChain->current_index();
    auto *backBuffer = m_swapChain->back_buffer(backIndex);
    auto *finalColor = m_frames->resource(a_frameIndex);
    auto *graphResources = m_frames->graph_resources(a_frameIndex);
    auto colorRtvResult = m_frames->rtv(a_frameIndex);
    auto colorSrvResult = m_frames->srv(a_frameIndex);
    auto backRtvResult = m_swapChain->rtv(backIndex);
    if (!backBuffer || !finalColor || !finalColor->resource() || !graphResources || !colorRtvResult.has_value() ||
        !colorSrvResult.has_value() || !backRtvResult.has_value())
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "DX12MainFrameGraph.record.resources"});
    }
    auto beginResult = m_frames->begin_frame(a_frameIndex);
    if (!beginResult.has_value())
    {
        return beginResult;
    }
    const auto colorRtv = *colorRtvResult.try_value();
    const auto colorSrv = *colorSrvResult.try_value();
    const auto backRtv = *backRtvResult.try_value();
    const auto *colorResource = finalColor->resource();
    try
    {
        const std::array external{DX12FrameGraphExternalResource{m_backBuffer, backBuffer}};
        const dx12FrameGraphPassCallback clearCallback =
            [this, colorRtv, colorResource](ID3D12GraphicsCommandList &a_list,
                                            const DX12FrameGraphPassContext &a_pass) -> Result<void>
        {
            if (a_pass.resource(m_finalColor) != colorResource)
            {
                return Result<void>::failure({ErrorCategory::InvalidState, "DX12MainFrameGraph.record.clear_binding"});
            }
            a_list.ClearRenderTargetView(colorRtv, m_clearColor.data(), 0, nullptr);
            return Result<void>::success();
        };
        const dx12FrameGraphPassCallback displayCallback =
            [this, colorResource, backBuffer, colorSrv,
             backRtv](ID3D12GraphicsCommandList &a_list, const DX12FrameGraphPassContext &a_pass) -> Result<void>
        {
            if (a_pass.resource(m_finalColor) != colorResource || a_pass.resource(m_backBuffer) != backBuffer)
            {
                return Result<void>::failure(
                    {ErrorCategory::InvalidState, "DX12MainFrameGraph.record.display_binding"});
            }
            return m_pipeline->draw(a_list, *m_srvAllocator->heap(), colorSrv, backRtv, m_width, m_height);
        };
        std::vector<dx12FrameGraphPassCallback> callbacks;
        callbacks.reserve(m_plan.passes().size());
        for (const auto &pass : m_plan.passes())
        {
            if (pass.handle.index == m_clearPass.index)
            {
                callbacks.push_back(clearCallback);
            }
            else if (pass.handle.index == m_displayPass.index)
            {
                callbacks.push_back(displayCallback);
            }
            else
            {
                callbacks.push_back(m_customCallbacks[pass.handle.index]);
            }
        }
        return DX12FrameGraphExecutor::record(m_plan, *graphResources, external, callbacks, a_context);
    }
    catch (const std::bad_alloc &)
    {
        return Result<void>::failure({ErrorCategory::PlatformFailure, "DX12MainFrameGraph.record.allocation"});
    }
}

/// @brief 枠を再利用する前に待つ完了点を FinalColor 所有者へ渡す
Result<void> DX12MainFrameGraph::mark_submitted(std::uint32_t a_frameIndex,
                                                std::shared_ptr<ICommandCompletion> a_completion)
{
    if (!m_frames)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "DX12MainFrameGraph.mark_submitted"});
    }
    return m_frames->mark_submitted(a_frameIndex, std::move(a_completion));
}

/// @brief 固定 Pass と Resource の実行順を検査できるよう貸し出す
const FrameGraphPlan &DX12MainFrameGraph::plan() const noexcept
{
    return m_plan;
}

/// @brief GPU 完了後に描画資源を解放し、再入時は何もしない
Result<void> DX12MainFrameGraph::shutdown()
{
    if (!m_frames)
    {
        return Result<void>::success();
    }
    auto result = m_frames->shutdown();
    if (!result.has_value())
    {
        return result;
    }
    m_customCallbacks.clear();
    m_frames.reset();
    m_pipeline.reset();
    m_swapChain = nullptr;
    m_srvAllocator = nullptr;
    return Result<void>::success();
}
} // namespace cue::dx12
