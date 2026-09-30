#include "DX12SurfacePool.h"

#include <array>

namespace cue::detail
{
/// @brief View と Resource の Manager を Surface より長く存続させる
DX12SurfacePool::DX12SurfacePool(DX12ViewManager& a_views, DX12ResourcePool& a_resources) noexcept
    : m_views(a_views), m_resources(a_resources)
{
}

/// @brief GPU 完了後に Surface と RTV Slot を Manager へ返す
DX12SurfacePool::~DX12SurfacePool()
{
    [[maybe_unused]] auto retired = collect_retired();
    for (const auto view : m_colorViews)
    {
        if (view.owner)
        {
            [[maybe_unused]] auto released = m_resources.destroy_view(view);
        }
    }
    if (m_depthView.owner)
    {
        [[maybe_unused]] auto released = m_resources.destroy_view(m_depthView);
    }
    for (const auto handle : m_color)
    {
        if (handle.owner)
        {
            [[maybe_unused]] auto released = m_resources.destroy(handle);
        }
    }
    if (m_depth.owner)
    {
        [[maybe_unused]] auto released = m_resources.destroy(m_depth);
    }
    for (auto slot : m_colorSlots)
    {
        if (slot.generation != 0)
        {
            [[maybe_unused]] auto result = m_views.release_rtv(slot);
        }
    }
}

/// @brief 各 Frame Slot の Color と永続 Depth を生成する
Result<std::unique_ptr<DX12SurfacePool>> DX12SurfacePool::create(DX12RenderDevice& a_device,
                                                                     DX12ViewManager& a_views,
                                                                     DX12ResourcePool& a_resources,
                                                                     WindowSize a_size)
{
    using PoolResult = Result<std::unique_ptr<DX12SurfacePool>>;
    if (a_size.width == 0 || a_size.height == 0)
    {
        return PoolResult::failure({ErrorCategory::InvalidArgument, "DX12SurfacePool.create"});
    }
    auto pool = std::make_unique<DX12SurfacePool>(a_views, a_resources);
    for (auto& slot : pool->m_colorSlots)
    {
        auto allocated = a_views.allocate_rtv();
        if (!allocated.has_value())
        {
            return PoolResult::failure(*allocated.try_error());
        }
        slot = allocated.take_value();
    }
    auto surfaceResult = pool->resize(a_device, a_size);
    if (!surfaceResult.has_value())
    {
        return PoolResult::failure(*surfaceResult.try_error());
    }
    return PoolResult::success(std::move(pool));
}

/// @brief 新 Surface と View を GPU 完了後に切り替える
Result<void> DX12SurfacePool::resize(DX12RenderDevice& a_device, WindowSize a_size)
{
    if (a_size.width == 0 || a_size.height == 0)
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "DX12SurfacePool.resize"});
    }
    auto retiredResult = collect_retired();
    if (!retiredResult.has_value())
    {
        return retiredResult;
    }
    std::array<GpuResourceHandle, k_backBufferCount> color{};
    for (UINT index = 0; index < k_backBufferCount; ++index)
    {
        auto created = m_resources.create_texture({a_size.width, a_size.height,
                                                   GpuTextureFormat::Rgba8Unorm});
        if (!created.has_value())
        {
            for (const auto handle : color)
            {
                if (handle.owner)
                {
                    [[maybe_unused]] auto released = m_resources.destroy(handle);
                }
            }
            return Result<void>::failure(*created.try_error());
        }
        color[index] = created.take_value();
    }
    auto depthResult = m_resources.create_texture({a_size.width, a_size.height,
                                                   GpuTextureFormat::Depth32Float});
    if (!depthResult.has_value())
    {
        for (const auto handle : color)
        {
            [[maybe_unused]] auto released = m_resources.destroy(handle);
        }
        return Result<void>::failure(*depthResult.try_error());
    }
    const auto depth = depthResult.take_value();
    std::array<GpuViewHandle, k_backBufferCount> colorHandles{};
    for (UINT index = 0; index < k_backBufferCount; ++index)
    {
        auto viewResult = m_resources.create_view(color[index], GpuViewKind::RenderTarget);
        if (!viewResult.has_value())
        {
            for (const auto view : colorHandles)
            {
                if (view.owner)
                {
                    [[maybe_unused]] auto released = m_resources.destroy_view(view);
                }
            }
            for (const auto handle : color)
            {
                [[maybe_unused]] auto released = m_resources.destroy(handle);
            }
            [[maybe_unused]] auto released = m_resources.destroy(depth);
            return Result<void>::failure(*viewResult.try_error());
        }
        colorHandles[index] = viewResult.take_value();
    }
    auto depthViewResult = m_resources.create_view(depth, GpuViewKind::DepthStencil);
    if (!depthViewResult.has_value())
    {
        for (const auto view : colorHandles)
        {
            [[maybe_unused]] auto released = m_resources.destroy_view(view);
        }
        for (const auto handle : color)
        {
            [[maybe_unused]] auto released = m_resources.destroy(handle);
        }
        [[maybe_unused]] auto released = m_resources.destroy(depth);
        return Result<void>::failure(*depthViewResult.try_error());
    }
    const auto depthView = depthViewResult.take_value();
    // 旧 Surface の Descriptor を書き換える前の失敗では新規資源だけを回収する
    const auto releaseNew = [&]() {
        for (const auto view : colorHandles)
        {
            [[maybe_unused]] auto released = m_resources.destroy_view(view);
        }
        [[maybe_unused]] auto depthReleased = m_resources.destroy_view(depthView);
        for (const auto handle : color)
        {
            [[maybe_unused]] auto released = m_resources.destroy(handle);
        }
        [[maybe_unused]] auto released = m_resources.destroy(depth);
    };
    std::array<ID3D12Resource*, k_backBufferCount> colorResources{};
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, k_backBufferCount> colorViews{};
    for (UINT index = 0; index < k_backBufferCount; ++index)
    {
        auto resource = m_resources.resource(color[index]);
        auto view = m_views.cpu_handle(m_colorSlots[index]);
        if (!resource.has_value() || !view.has_value())
        {
            releaseNew();
            return Result<void>::failure({ErrorCategory::InvalidState, "DX12SurfacePool.resize.view"});
        }
        colorResources[index] = *resource.try_value();
        colorViews[index] = view.take_value();
    }
    auto depthResource = m_resources.resource(depth);
    if (!depthResource.has_value())
    {
        releaseNew();
        return Result<void>::failure(*depthResource.try_error());
    }
    // 全 Handle を検証してから Descriptor を書き換え、失敗時は旧 View を残す
    for (UINT index = 0; index < k_backBufferCount; ++index)
    {
        a_device.device()->CreateRenderTargetView(colorResources[index], nullptr, colorViews[index]);
    }
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
    dsv.Format = DXGI_FORMAT_D32_FLOAT;
    dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    a_device.device()->CreateDepthStencilView(*depthResource.try_value(), &dsv, m_views.dsv_handle());
    m_retired.push_back({m_color, m_depth, m_colorViews, m_depthView});
    m_color = color;
    m_depth = depth;
    m_colorViews = colorHandles;
    m_depthView = depthView;
    return collect_retired();
}

/// @brief 破棄に成功した Handle だけを退避記録から除外する
Result<void> DX12SurfacePool::collect_retired()
{
    for (auto& retired : m_retired)
    {
        for (auto& view : retired.colorViews)
        {
            if (!view.owner)
            {
                continue;
            }
            auto released = m_resources.destroy_view(view);
            if (!released.has_value())
            {
                return released;
            }
            view = {};
        }
        if (retired.depthView.owner)
        {
            auto released = m_resources.destroy_view(retired.depthView);
            if (!released.has_value())
            {
                return released;
            }
            retired.depthView = {};
        }
        for (auto& handle : retired.color)
        {
            if (!handle.owner)
            {
                continue;
            }
            auto released = m_resources.destroy(handle);
            if (!released.has_value())
            {
                return released;
            }
            handle = {};
        }
        if (retired.depth.owner)
        {
            auto released = m_resources.destroy(retired.depth);
            if (!released.has_value())
            {
                return released;
            }
            retired.depth = {};
        }
    }
    m_retired.clear();
    return Result<void>::success();
}

/// @brief Frame Slot に対応する Color Surface を返す
ID3D12Resource* DX12SurfacePool::color(UINT a_slot) const noexcept
{
    if (a_slot >= k_backBufferCount)
    {
        return nullptr;
    }
    auto borrowed = m_resources.resource(m_color[a_slot]);
    return borrowed.has_value() ? *borrowed.try_value() : nullptr;
}

/// @brief Depth Surface を返す
ID3D12Resource* DX12SurfacePool::depth() const noexcept
{
    auto borrowed = m_resources.resource(m_depth);
    return borrowed.has_value() ? *borrowed.try_value() : nullptr;
}

/// @brief Frame Slot に対応する Color RTV を返す
Result<D3D12_CPU_DESCRIPTOR_HANDLE> DX12SurfacePool::color_rtv(UINT a_slot) const
{
    if (a_slot >= k_backBufferCount)
    {
        return Result<D3D12_CPU_DESCRIPTOR_HANDLE>::failure({ErrorCategory::InvalidArgument,
                                                                "DX12SurfacePool.color_rtv"});
    }
    return m_views.cpu_handle(m_colorSlots[a_slot]);
}

/// @brief 永続 Depth の DSV を返す
D3D12_CPU_DESCRIPTOR_HANDLE DX12SurfacePool::depth_dsv() const noexcept
{
    return m_views.dsv_handle();
}

/// @brief Slot の有効な Color View を Graph Pass へ貸す
GpuViewHandle DX12SurfacePool::color_view(UINT a_slot) const noexcept
{
    return a_slot < k_backBufferCount ? m_colorViews[a_slot] : GpuViewHandle{};
}

/// @brief 現行 Depth View を Graph Pass へ貸す
GpuViewHandle DX12SurfacePool::depth_view() const noexcept
{
    return m_depthView;
}

/// @brief Resize まで有効な Color Resource Handle を返す
GpuResourceHandle DX12SurfacePool::color_resource(UINT a_slot) const noexcept
{
    return a_slot < k_backBufferCount ? m_color[a_slot] : GpuResourceHandle{};
}
} // namespace cue::detail
