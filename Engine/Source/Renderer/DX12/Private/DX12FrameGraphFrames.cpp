#include <DX12/DX12FrameGraphFrames.h>

#include <algorithm>
#include <exception>
#include <memory>
#include <new>
#include <utility>
#include <vector>

#include <wrl/client.h>

#include <DX12/DX12FrameGraphResources.h>
#include <DX12/DX12GpuResource.h>
#include <DX12/DX12RenderDevice.h>
#include <Platform/Diagnostics.h>

namespace cue::dx12
{
namespace
{
/// @brief 外部 Texture が Graph の宣言と View の用途に一致するか調べる
bool matches_imported_texture(const FrameGraphResourcePlan& a_planned, ID3D12Resource& a_resource,
                              bool a_needsRtv, bool a_needsSrv) noexcept
{
    const auto native = a_resource.GetDesc();
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    switch (a_planned.textureDesc.format)
    {
    case GpuTextureFormat::Rgba8Unorm: format = DXGI_FORMAT_R8G8B8A8_UNORM; break;
    case GpuTextureFormat::Bgra8Unorm: format = DXGI_FORMAT_B8G8R8A8_UNORM; break;
    case GpuTextureFormat::Rgba16Float: format = DXGI_FORMAT_R16G16B16A16_FLOAT; break;
    case GpuTextureFormat::R32Float: format = DXGI_FORMAT_R32_FLOAT; break;
    default: return false;
    }
    return native.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
           native.Width == a_planned.textureDesc.width && native.Height == a_planned.textureDesc.height &&
           native.MipLevels == a_planned.textureDesc.mipLevels && native.DepthOrArraySize == 1 &&
           native.SampleDesc.Count == 1 && native.Format == format &&
           (!a_needsRtv || (native.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0) &&
           (!a_needsSrv || (native.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) == 0);
}
} // namespace

/// @brief create の内部でだけ空の枠配列を構築する
DX12FrameGraphFrames::DX12FrameGraphFrames(CreateToken) noexcept
{
}

/// @brief 一時 Texture の View を作り、外部 Texture の View Slot を枠ごとに予約する
Result<std::unique_ptr<DX12FrameGraphFrames>> DX12FrameGraphFrames::create(
    DX12RenderDevice& a_device, const FrameGraphPlan& a_plan, std::uint32_t a_frameCount,
    DX12DescriptorAllocator& a_rtvAllocator, DX12DescriptorAllocator& a_srvAllocator,
    FrameGraphResourceHandle a_borrowedRtvResource)
{
    using FramesResult = Result<std::unique_ptr<DX12FrameGraphFrames>>;
    if (!a_device.device() || a_plan.id() == 0 || a_frameCount == 0 ||
        a_rtvAllocator.type() != D3D12_DESCRIPTOR_HEAP_TYPE_RTV ||
        a_srvAllocator.type() != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV ||
        !a_srvAllocator.is_shader_visible() ||
        (a_borrowedRtvResource.is_valid() &&
         (a_borrowedRtvResource.index >= a_plan.resources().size() ||
          a_plan.resources()[a_borrowedRtvResource.index].handle.graphId != a_borrowedRtvResource.graphId ||
          !a_plan.resources()[a_borrowedRtvResource.index].isImported)))
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
                    a_plan.resources()[use.resource.index].kind != GpuResourceKind::Texture2D)
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
        frames->m_device = &a_device;
        frames->m_borrowedRtvResource = a_borrowedRtvResource;
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
                auto& views = frame.views[planned.handle.index];
                views.isImported = planned.isImported;
                views.needsRtv = needsRtv[planned.handle.index];
                views.needsSrv = needsSrv[planned.handle.index];
                if (planned.kind != GpuResourceKind::Texture2D || !planned.firstUse)
                {
                    continue;
                }
                ID3D12Resource* native = nullptr;
                D3D12_RESOURCE_DESC nativeDesc{};
                if (!planned.isImported)
                {
                    auto* texture = frame.graph->resource(planned.handle);
                    if (!texture || !texture->resource())
                    {
                        return FramesResult::failure({ErrorCategory::InvalidState,
                                                      "DX12FrameGraphFrames.create.resource"});
                    }
                    native = texture->resource();
                    nativeDesc = native->GetDesc();
                }
                if (views.needsRtv &&
                    !(planned.handle.graphId == a_borrowedRtvResource.graphId &&
                      planned.handle.index == a_borrowedRtvResource.index))
                {
                    if (native && (nativeDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) == 0)
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
                    if (native)
                    {
                        a_device.device()->CreateRenderTargetView(native, nullptr, *handleResult.try_value());
                    }
                }

                if (!views.needsSrv)
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
                if (native)
                {
                    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
                    srvDesc.Format = nativeDesc.Format;
                    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                    srvDesc.Texture2D.MipLevels = nativeDesc.MipLevels;
                    a_device.device()->CreateShaderResourceView(native, &srvDesc, *handleResult.try_value());
                }
                views.isPrepared = native != nullptr;
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
    // 同じ Slot を再利用するまでに旧 Binding の GPU 参照が終わった
    for (auto& views : frame.views)
    {
        if (views.isImported)
        {
            views.isPrepared = false;
            views.borrowedRtv.reset();
        }
    }
    return Result<void>::success();
}

/// @brief 外部 Texture の実体を一括検証してから、この枠専用の Descriptor を更新する
Result<void> DX12FrameGraphFrames::prepare_imported_views(
    std::uint32_t a_frameIndex, const FrameGraphPlan& a_plan,
    std::span<const DX12FrameGraphExternalResource> a_external,
    D3D12_CPU_DESCRIPTOR_HANDLE a_borrowedRtv)
{
    if (m_isClosed || !m_device || !m_device->device() || a_frameIndex >= m_frames.size() ||
        a_plan.resources().size() != m_frames[a_frameIndex].views.size())
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument,
                                      "DX12FrameGraphFrames.prepare_imported_views"});
    }
    auto& frame = m_frames[a_frameIndex];
    // 提出済み枠と記録準備済み枠の Descriptor を途中で上書きしない
    if (frame.completion ||
        std::any_of(frame.views.begin(), frame.views.end(),
                    [](const Views& a_views) { return a_views.isImported && a_views.isPrepared; }))
    {
        return Result<void>::failure({ErrorCategory::InvalidState,
                                      "DX12FrameGraphFrames.prepare_imported_views.active_frame"});
    }
    // Descriptor の一部だけを書き換えてから検証失敗しないよう、先に全実体を調べる
    for (const auto& planned : a_plan.resources())
    {
        const auto& views = frame.views[planned.handle.index];
        if (!planned.isImported || planned.kind != GpuResourceKind::Texture2D ||
            !planned.firstUse || (!views.needsRtv && !views.needsSrv))
        {
            continue;
        }
        const auto binding = std::find_if(a_external.begin(), a_external.end(),
                                          [&planned](const DX12FrameGraphExternalResource& a_candidate)
                                          { return a_candidate.handle.graphId == planned.handle.graphId &&
                                                   a_candidate.handle.index == planned.handle.index; });
        if (binding == a_external.end() || !binding->resource ||
            !matches_imported_texture(planned, *binding->resource, views.needsRtv, views.needsSrv))
        {
            return Result<void>::failure({ErrorCategory::InvalidArgument,
                                          "DX12FrameGraphFrames.prepare_imported_views.resource"});
        }
        Microsoft::WRL::ComPtr<ID3D12Device> resourceDevice;
        if (FAILED(binding->resource->GetDevice(IID_PPV_ARGS(&resourceDevice))) ||
            resourceDevice.Get() != m_device->device())
        {
            return Result<void>::failure({ErrorCategory::InvalidArgument,
                                          "DX12FrameGraphFrames.prepare_imported_views.device"});
        }
        if (planned.handle.graphId == m_borrowedRtvResource.graphId &&
            planned.handle.index == m_borrowedRtvResource.index && views.needsRtv &&
            a_borrowedRtv.ptr == 0)
        {
            return Result<void>::failure({ErrorCategory::InvalidArgument,
                                          "DX12FrameGraphFrames.prepare_imported_views.borrowed_rtv"});
        }
    }
    for (const auto& planned : a_plan.resources())
    {
        auto& views = frame.views[planned.handle.index];
        if (!planned.isImported || planned.kind != GpuResourceKind::Texture2D ||
            !planned.firstUse || (!views.needsRtv && !views.needsSrv))
        {
            continue;
        }
        const auto binding = std::find_if(a_external.begin(), a_external.end(),
                                          [&planned](const DX12FrameGraphExternalResource& a_candidate)
                                          { return a_candidate.handle.graphId == planned.handle.graphId &&
                                                   a_candidate.handle.index == planned.handle.index; });
        auto* native = binding->resource;
        if (views.needsRtv)
        {
            if (planned.handle.graphId == m_borrowedRtvResource.graphId &&
                planned.handle.index == m_borrowedRtvResource.index)
            {
                views.borrowedRtv = a_borrowedRtv;
            }
            else
            {
                auto handleResult = m_rtvAllocator->cpu_handle(views.rtv);
                if (!handleResult.has_value())
                {
                    return Result<void>::failure(*handleResult.try_error());
                }
                m_device->device()->CreateRenderTargetView(native, nullptr, *handleResult.try_value());
            }
        }
        if (views.needsSrv)
        {
            auto handleResult = m_srvAllocator->cpu_handle(views.srv);
            if (!handleResult.has_value())
            {
                return Result<void>::failure(*handleResult.try_error());
            }
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
            srvDesc.Format = native->GetDesc().Format;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Texture2D.MipLevels = native->GetDesc().MipLevels;
            m_device->device()->CreateShaderResourceView(native, &srvDesc, *handleResult.try_value());
        }
        views.isPrepared = true;
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
    if (!owns(a_frameIndex, a_handle))
    {
        return Result<D3D12_CPU_DESCRIPTOR_HANDLE>::failure(
            {ErrorCategory::InvalidArgument, "DX12FrameGraphFrames.rtv"});
    }
    const auto& views = m_frames[a_frameIndex].views[a_handle.index];
    if (views.isImported && !views.isPrepared)
    {
        return Result<D3D12_CPU_DESCRIPTOR_HANDLE>::failure(
            {ErrorCategory::InvalidState, "DX12FrameGraphFrames.rtv.unprepared"});
    }
    if (views.borrowedRtv)
    {
        return Result<D3D12_CPU_DESCRIPTOR_HANDLE>::success(*views.borrowedRtv);
    }
    return m_rtvAllocator->cpu_handle(views.rtv);
}

/// @brief 論理 Handle に対応する SRV Slot の世代を検証する
Result<D3D12_GPU_DESCRIPTOR_HANDLE> DX12FrameGraphFrames::srv(
    std::uint32_t a_frameIndex, FrameGraphResourceHandle a_handle) const
{
    if (!owns(a_frameIndex, a_handle) ||
        (m_frames[a_frameIndex].views[a_handle.index].isImported &&
         !m_frames[a_frameIndex].views[a_handle.index].isPrepared))
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
