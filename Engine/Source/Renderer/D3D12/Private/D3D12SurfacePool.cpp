#include "D3D12SurfacePool.h"

#include <array>
#include <utility>

namespace cue::detail
{
namespace
{
/// @brief Color と Depth に共通する 2D Texture 設定を作る
D3D12_RESOURCE_DESC texture_desc(WindowSize a_size, DXGI_FORMAT a_format, D3D12_RESOURCE_FLAGS a_flags)
{
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = a_size.width;
    desc.Height = a_size.height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = a_format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = a_flags;
    return desc;
}
} // namespace

/// @brief View Manager を借用し、全 Surface より長く存続させる
D3D12SurfacePool::D3D12SurfacePool(D3D12ViewManager& a_views) noexcept
    : m_views(a_views)
{
}

/// @brief 割り当てた RTV Slot を Surface の寿命末尾で返す
D3D12SurfacePool::~D3D12SurfacePool()
{
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
                                                                     WindowSize a_size)
{
    using PoolResult = Result<std::unique_ptr<D3D12SurfacePool>>;
    if (a_size.width == 0 || a_size.height == 0)
    {
        return PoolResult::failure({ErrorCategory::InvalidArgument, "D3D12SurfacePool.create"});
    }
    auto pool = std::make_unique<D3D12SurfacePool>(a_views);
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
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    const D3D12_CLEAR_VALUE colorClear{DXGI_FORMAT_R8G8B8A8_UNORM, {0.07f, 0.13f, 0.25f, 1.0f}};
    D3D12_CLEAR_VALUE depthClear{};
    depthClear.Format = DXGI_FORMAT_D32_FLOAT;
    depthClear.DepthStencil.Depth = 1.0f;
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, k_backBufferCount> color;
    for (auto& surface : color)
    {
        const auto desc = texture_desc(a_size, DXGI_FORMAT_R8G8B8A8_UNORM,
                                       D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        const HRESULT result = a_device.device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
            &desc, D3D12_RESOURCE_STATE_COMMON, &colorClear, IID_PPV_ARGS(&surface));
        if (FAILED(result))
        {
            return Result<void>::failure(gpu_error("ID3D12Device.CreateCommittedResource.Color", result));
        }
    }
    Microsoft::WRL::ComPtr<ID3D12Resource> depth;
    const auto depthDesc = texture_desc(a_size, DXGI_FORMAT_D32_FLOAT,
                                        D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    const HRESULT depthResult = a_device.device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
        &depthDesc, D3D12_RESOURCE_STATE_COMMON, &depthClear, IID_PPV_ARGS(&depth));
    if (FAILED(depthResult))
    {
        return Result<void>::failure(gpu_error("ID3D12Device.CreateCommittedResource.Depth", depthResult));
    }
    for (UINT index = 0; index < k_backBufferCount; ++index)
    {
        auto writeResult = m_views.write_rtv(a_device.device(), m_colorSlots[index], color[index].Get());
        if (!writeResult.has_value())
        {
            return writeResult;
        }
    }
    auto depthViewResult = m_views.write_dsv(a_device.device(), depth.Get());
    if (!depthViewResult.has_value())
    {
        return depthViewResult;
    }
    m_color = std::move(color);
    m_depth = std::move(depth);
    return Result<void>::success();
}

/// @brief Frame Slot に対応する Color Surface を返す
ID3D12Resource* D3D12SurfacePool::color(UINT a_slot) const noexcept
{
    return a_slot < k_backBufferCount ? m_color[a_slot].Get() : nullptr;
}

/// @brief Depth Surface を返す
ID3D12Resource* D3D12SurfacePool::depth() const noexcept
{
    return m_depth.Get();
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
