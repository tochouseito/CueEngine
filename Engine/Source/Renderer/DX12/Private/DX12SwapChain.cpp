#include "DX12SwapChain.h"

#include <string>

#include <d3dcommon.h>

namespace cue::detail
{
/// @brief View Manager より短い寿命で表示資源を作る
DX12SwapChain::DX12SwapChain(DX12ViewManager& a_views) noexcept
    : m_views(a_views)
{
}

/// @brief 借用した RTV Slot を View Manager に返す
DX12SwapChain::~DX12SwapChain()
{
    for (auto slot : m_rtvSlots)
    {
        if (slot.generation != 0)
        {
            [[maybe_unused]] auto result = m_views.release_rtv(slot);
        }
    }
}

/// @brief 有効な Window Handle に結び付く表示資源を作る
Result<std::unique_ptr<DX12SwapChain>> DX12SwapChain::create(DX12RenderDevice& a_device,
                                                                       DX12GpuCommandQueue& a_queue,
                                                                       DX12ViewManager& a_views,
                                                                       void* a_nativeWindow, WindowSize a_size)
{
    using PresentationResult = Result<std::unique_ptr<DX12SwapChain>>;
    auto presentation = std::make_unique<DX12SwapChain>(a_views);

    // Flip Model の Swap Chain は Device ではなく Direct Queue に接続する
    DXGI_SWAP_CHAIN_DESC1 swapDesc{};
    swapDesc.Width = a_size.width;
    swapDesc.Height = a_size.height;
    swapDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapDesc.BufferCount = k_backBufferCount;
    swapDesc.SampleDesc.Count = 1;
    swapDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> swapChain;
    HRESULT result = a_device.factory()->CreateSwapChainForHwnd(a_queue.queue(), static_cast<HWND>(a_nativeWindow),
                                                                 &swapDesc, nullptr, nullptr, &swapChain);
    if (FAILED(result))
    {
        return PresentationResult::failure(gpu_error("IDXGIFactory.CreateSwapChainForHwnd", result));
    }
    result = swapChain.As(&presentation->m_swapChain);
    if (FAILED(result))
    {
        return PresentationResult::failure(gpu_error("IDXGISwapChain.QueryInterface", result));
    }
    constexpr char k_swapChainName[] = "CueEngine Swap Chain";
    result = presentation->m_swapChain->SetPrivateData(WKPDID_D3DDebugObjectName,
                                                       sizeof(k_swapChainName) - 1, k_swapChainName);
    if (FAILED(result))
    {
        return PresentationResult::failure(gpu_error("IDXGISwapChain.SetPrivateData", result));
    }

    auto buffersResult = presentation->acquire_buffers(a_device.device());
    if (!buffersResult.has_value())
    {
        return PresentationResult::failure(*buffersResult.try_error());
    }
    presentation->m_size = a_size;
    return PresentationResult::success(std::move(presentation));
}

/// @brief GPU 完了後、Command List が旧 Buffer を参照しない状態で Surface を更新する
Result<void> DX12SwapChain::resize(DX12RenderDevice& a_device, WindowSize a_size)
{
    // ResizeBuffers の前にこの Owner の旧 Buffer 参照を全て落とす
    for (auto& buffer : m_backBuffers)
    {
        buffer.Reset();
    }
    const HRESULT result = m_swapChain->ResizeBuffers(k_backBufferCount, a_size.width, a_size.height,
                                                       DXGI_FORMAT_R8G8B8A8_UNORM, 0);
    if (FAILED(result))
    {
        return Result<void>::failure(gpu_error("IDXGISwapChain.ResizeBuffers", result));
    }
    auto buffersResult = acquire_buffers(a_device.device());
    if (!buffersResult.has_value())
    {
        return buffersResult;
    }
    m_size = a_size;
    return Result<void>::success();
}

/// @brief Buffer Slot と RTV Slot を同じ Index で結び付ける
Result<void> DX12SwapChain::acquire_buffers(ID3D12Device* a_device)
{
    for (UINT index = 0; index < k_backBufferCount; ++index)
    {
        HRESULT result = m_swapChain->GetBuffer(index, IID_PPV_ARGS(&m_backBuffers[index]));
        if (FAILED(result))
        {
            return Result<void>::failure(gpu_error("IDXGISwapChain.GetBuffer", result));
        }
        const std::wstring name = L"CueEngine Back Buffer " + std::to_wstring(index);
        result = m_backBuffers[index]->SetName(name.c_str());
        if (FAILED(result))
        {
            return Result<void>::failure(gpu_error("ID3D12Resource.SetName.BackBuffer", result));
        }
        if (m_rtvSlots[index].generation == 0)
        {
            auto slotResult = m_views.allocate_rtv();
            if (!slotResult.has_value())
            {
                return Result<void>::failure(*slotResult.try_error());
            }
            m_rtvSlots[index] = slotResult.take_value();
        }
        auto writeResult = m_views.write_rtv(a_device, m_rtvSlots[index], m_backBuffers[index].Get());
        if (!writeResult.has_value())
        {
            return writeResult;
        }
    }
    return Result<void>::success();
}

/// @brief 現在の Back Buffer の Index を返す
UINT DX12SwapChain::current_index() const noexcept
{
    return m_swapChain->GetCurrentBackBufferIndex();
}

/// @brief Presentation が存続し、次の Resize までだけ Buffer を借用する
ID3D12Resource* DX12SwapChain::back_buffer(UINT a_index) const noexcept
{
    return m_backBuffers[a_index].Get();
}

/// @brief Presentation が存続する間だけ RTV の CPU Handle を借用する
Result<D3D12_CPU_DESCRIPTOR_HANDLE> DX12SwapChain::rtv(UINT a_index) const
{
    if (a_index >= k_backBufferCount)
    {
        return Result<D3D12_CPU_DESCRIPTOR_HANDLE>::failure({ErrorCategory::InvalidArgument,
                                                               "DX12SwapChain.rtv"});
    }
    return m_views.cpu_handle(m_rtvSlots[a_index]);
}

/// @brief Submit 済みの Back Buffer を表示する
Result<void> DX12SwapChain::present()
{
    const HRESULT result = m_swapChain->Present(0, 0);
    if (FAILED(result))
    {
        return Result<void>::failure(gpu_error("IDXGISwapChain.Present", result));
    }
    return Result<void>::success();
}

/// @brief 現在適用済みの Client Size を返す
WindowSize DX12SwapChain::size() const noexcept
{
    return m_size;
}
} // namespace cue::detail
