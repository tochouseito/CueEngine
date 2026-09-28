#include "D3D12SurfacePool.h"

#include <array>

namespace cue::detail
{
/// @brief View と Resource の Manager を Surface より長く存続させる
D3D12SurfacePool::D3D12SurfacePool(D3D12ViewManager& a_views, D3D12ResourcePool& a_resources) noexcept
    : m_views(a_views), m_resources(a_resources)
{
}

/// @brief GPU 完了後に Surface と RTV Slot を Manager へ返す
D3D12SurfacePool::~D3D12SurfacePool()
{
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
Result<std::unique_ptr<D3D12SurfacePool>> D3D12SurfacePool::create(D3D12DeviceContext& a_device,
                                                                     D3D12ViewManager& a_views,
                                                                     D3D12ResourcePool& a_resources,
                                                                     WindowSize a_size)
{
    using PoolResult = Result<std::unique_ptr<D3D12SurfacePool>>;
    if (a_size.width == 0 || a_size.height == 0)
    {
        return PoolResult::failure({ErrorCategory::InvalidArgument, "D3D12SurfacePool.create"});
    }
    auto pool = std::make_unique<D3D12SurfacePool>(a_views, a_resources);
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
Result<void> D3D12SurfacePool::resize(D3D12DeviceContext& a_device, WindowSize a_size)
{
    if (a_size.width == 0 || a_size.height == 0)
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "D3D12SurfacePool.resize"});
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
    std::array<ID3D12Resource*, k_backBufferCount> colorResources{};
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, k_backBufferCount> colorViews{};
    for (UINT index = 0; index < k_backBufferCount; ++index)
    {
        auto resource = m_resources.resource(color[index]);
        auto view = m_views.cpu_handle(m_colorSlots[index]);
        if (!resource.has_value() || !view.has_value())
        {
            for (const auto handle : color)
            {
                [[maybe_unused]] auto released = m_resources.destroy(handle);
            }
            [[maybe_unused]] auto released = m_resources.destroy(depth);
            return Result<void>::failure({ErrorCategory::InvalidState, "D3D12SurfacePool.resize.view"});
        }
        colorResources[index] = *resource.try_value();
        colorViews[index] = view.take_value();
    }
    auto depthResource = m_resources.resource(depth);
    if (!depthResource.has_value())
    {
        for (const auto handle : color)
        {
            [[maybe_unused]] auto released = m_resources.destroy(handle);
        }
        [[maybe_unused]] auto released = m_resources.destroy(depth);
        return Result<void>::failure(*depthResource.try_error());
    }
    // 全 Handle を検証してから Descriptor を書き換え、失敗時は旧 View を残す
    for (UINT index = 0; index < k_backBufferCount; ++index)
    {
        a_device.device()->CreateRenderTargetView(colorResources[index], nullptr, colorViews[index]);
    }
    a_device.device()->CreateDepthStencilView(*depthResource.try_value(), nullptr, m_views.dsv_handle());
    const auto oldColor = m_color;
    const auto oldDepth = m_depth;
    m_color = color;
    m_depth = depth;
    for (const auto handle : oldColor)
    {
        if (handle.owner)
        {
            auto released = m_resources.destroy(handle);
            if (!released.has_value())
            {
                return released;
            }
        }
    }
    if (oldDepth.owner)
    {
        return m_resources.destroy(oldDepth);
    }
    return Result<void>::success();
}

/// @brief Frame Slot に対応する Color Surface を返す
ID3D12Resource* D3D12SurfacePool::color(UINT a_slot) const noexcept
{
    if (a_slot >= k_backBufferCount)
    {
        return nullptr;
    }
    auto borrowed = m_resources.resource(m_color[a_slot]);
    return borrowed.has_value() ? *borrowed.try_value() : nullptr;
}

/// @brief Depth Surface を返す
ID3D12Resource* D3D12SurfacePool::depth() const noexcept
{
    auto borrowed = m_resources.resource(m_depth);
    return borrowed.has_value() ? *borrowed.try_value() : nullptr;
}

/// @brief Frame Slot に対応する Color RTV を返す
Result<D3D12_CPU_DESCRIPTOR_HANDLE> D3D12SurfacePool::color_rtv(UINT a_slot) const
{
    if (a_slot >= k_backBufferCount)
    {
        return Result<D3D12_CPU_DESCRIPTOR_HANDLE>::failure({ErrorCategory::InvalidArgument,
                                                                "D3D12SurfacePool.color_rtv"});
    }
    return m_views.cpu_handle(m_colorSlots[a_slot]);
}

/// @brief 永続 Depth の DSV を返す
D3D12_CPU_DESCRIPTOR_HANDLE D3D12SurfacePool::depth_dsv() const noexcept
{
    return m_views.dsv_handle();
}
} // namespace cue::detail
