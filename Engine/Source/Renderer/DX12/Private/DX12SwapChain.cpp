#include <DX12/DX12SwapChain.h>

#include <exception>
#include <memory>
#include <new>
#include <string>
#include <utility>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <DX12/DX12QueuePool.h>
#include <DX12/DX12RenderDevice.h>
#include <Platform/Diagnostics.h>

namespace cue::dx12
{
namespace
{
/// @brief DXGI と D3D12 の失敗値を共通診断へ変換する
Error swap_chain_error(const char* a_operation, HRESULT a_result)
{
    return {ErrorCategory::PlatformFailure, a_operation, static_cast<std::int64_t>(a_result)};
}
} // namespace

/// @brief Native Object の生成前に所有状態を空にする
DX12SwapChain::DX12SwapChain(CreateToken) noexcept
{
}

/// @brief SwapChain と全 Back Buffer の RTV が揃った場合だけ公開する
Result<std::unique_ptr<DX12SwapChain>> DX12SwapChain::create(
    DX12RenderDevice& a_device, queueLease a_queue, DX12DescriptorAllocator& a_rtvAllocator,
    void* a_windowHandle, DX12SwapChainConfig a_config)
{
    using SwapResult = Result<std::unique_ptr<DX12SwapChain>>;
    auto* queue = dynamic_cast<DX12GpuCommandQueue*>(a_queue.get());
    if (!a_device.device() || !a_device.factory() || !queue || !queue->command_queue() ||
        queue->device() != a_device.device() || queue->type() != QueueType::Graphics ||
        !a_windowHandle || !IsWindow(static_cast<HWND>(a_windowHandle)) || a_config.width == 0 ||
        a_config.height == 0 || a_config.bufferCount < 2 || a_config.bufferCount > DXGI_MAX_SWAP_CHAIN_BUFFERS ||
        (a_config.format != DXGI_FORMAT_R8G8B8A8_UNORM && a_config.format != DXGI_FORMAT_B8G8R8A8_UNORM) ||
        a_rtvAllocator.type() != D3D12_DESCRIPTOR_HEAP_TYPE_RTV)
    {
        return SwapResult::failure({ErrorCategory::InvalidArgument, "DX12SwapChain.create"});
    }

    BOOL canTear = false;
    const HRESULT featureResult = a_device.factory()->CheckFeatureSupport(
        DXGI_FEATURE_PRESENT_ALLOW_TEARING, &canTear, sizeof(canTear));
    const bool isTearingEnabled = a_config.isTearingAllowed && SUCCEEDED(featureResult) && canTear != false;

    try
    {
        auto result = std::make_unique<DX12SwapChain>(CreateToken{});
        result->m_rtvAllocator = &a_rtvAllocator;
        result->m_queue = std::move(a_queue);
        result->m_config = a_config;
        result->m_isTearingEnabled = isTearingEnabled;
        result->m_backBuffers.reserve(a_config.bufferCount);
        result->m_rtvHandles.reserve(a_config.bufferCount);

        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = a_config.width;
        desc.Height = a_config.height;
        desc.Format = a_config.format;
        desc.Stereo = false;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = a_config.bufferCount;
        desc.Scaling = DXGI_SCALING_STRETCH;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
        desc.Flags = isTearingEnabled ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;

        Microsoft::WRL::ComPtr<IDXGISwapChain1> created;
        const HRESULT createResult = a_device.factory()->CreateSwapChainForHwnd(
            queue->command_queue(), static_cast<HWND>(a_windowHandle), &desc, nullptr, nullptr, &created);
        if (FAILED(createResult))
        {
            return SwapResult::failure(swap_chain_error("IDXGIFactory.CreateSwapChainForHwnd", createResult));
        }
        const HRESULT queryResult = created.As(&result->m_swapChain);
        if (FAILED(queryResult))
        {
            return SwapResult::failure(swap_chain_error("IDXGISwapChain.QueryInterface", queryResult));
        }
        const HRESULT associationResult = a_device.factory()->MakeWindowAssociation(
            static_cast<HWND>(a_windowHandle), DXGI_MWA_NO_ALT_ENTER);
        if (FAILED(associationResult))
        {
            return SwapResult::failure(swap_chain_error("IDXGIFactory.MakeWindowAssociation", associationResult));
        }

        constexpr wchar_t k_name[] = L"CueEngine DX12 SwapChain";
        const HRESULT nameResult = result->m_swapChain->SetPrivateData(
            WKPDID_D3DDebugObjectNameW, static_cast<UINT>(sizeof(k_name)), k_name);
        if (FAILED(nameResult))
        {
            report_error("DX12SwapChain", swap_chain_error("IDXGISwapChain.SetPrivateData", nameResult),
                         DiagnosticSeverity::Warning);
        }
        for (std::uint32_t index = 0; index < a_config.bufferCount; ++index)
        {
            Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
            const HRESULT bufferResult = result->m_swapChain->GetBuffer(index, IID_PPV_ARGS(&buffer));
            if (FAILED(bufferResult))
            {
                return SwapResult::failure(swap_chain_error("IDXGISwapChain.GetBuffer", bufferResult));
            }
            const std::wstring bufferName = L"CueEngine DX12 Back Buffer " + std::to_wstring(index);
            const HRESULT bufferNameResult = buffer->SetName(bufferName.c_str());
            if (FAILED(bufferNameResult))
            {
                report_error("DX12SwapChain", swap_chain_error("ID3D12Resource.SetName", bufferNameResult),
                             DiagnosticSeverity::Warning);
            }
            auto slotResult = a_rtvAllocator.allocate();
            if (!slotResult.has_value())
            {
                return SwapResult::failure(*slotResult.try_error());
            }
            const auto slot = slotResult.take_value();
            result->m_rtvHandles.push_back(slot);
            auto handleResult = a_rtvAllocator.cpu_handle(slot);
            if (!handleResult.has_value())
            {
                return SwapResult::failure(*handleResult.try_error());
            }
            a_device.device()->CreateRenderTargetView(buffer.Get(), nullptr, *handleResult.try_value());
            result->m_backBuffers.push_back(std::move(buffer));
        }
        return SwapResult::success(std::move(result));
    }
    catch (const std::bad_alloc&)
    {
        return SwapResult::failure({ErrorCategory::PlatformFailure, "DX12SwapChain.create.allocation"});
    }
}

/// @brief 明示停止されなかった場合も GPU 完了後に参照を解放する
DX12SwapChain::~DX12SwapChain()
{
    auto result = shutdown();
    if (!result.has_value())
    {
        report_error("DX12SwapChain.shutdown", *result.try_error(), DiagnosticSeverity::Fatal);
        std::terminate();
    }
}

/// @brief DXGI が選んだ次の Back Buffer Index を返す
std::uint32_t DX12SwapChain::current_index() const noexcept
{
    return m_swapChain ? m_swapChain->GetCurrentBackBufferIndex() : 0;
}

/// @brief Index を検証して Native Back Buffer を貸す
ID3D12Resource* DX12SwapChain::back_buffer(std::uint32_t a_index) const noexcept
{
    return a_index < m_backBuffers.size() ? m_backBuffers[a_index].Get() : nullptr;
}

/// @brief RTV Slot の世代を検証して CPU Handle を返す
Result<D3D12_CPU_DESCRIPTOR_HANDLE> DX12SwapChain::rtv(std::uint32_t a_index) const
{
    if (!m_rtvAllocator || a_index >= m_rtvHandles.size())
    {
        return Result<D3D12_CPU_DESCRIPTOR_HANDLE>::failure(
            {ErrorCategory::InvalidArgument, "DX12SwapChain.rtv"});
    }
    return m_rtvAllocator->cpu_handle(m_rtvHandles[a_index]);
}

/// @brief VSync と DXGI のティアリング制約を一箇所で適用する
Result<void> DX12SwapChain::present()
{
    if (!m_swapChain)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "DX12SwapChain.present"});
    }
    const UINT syncInterval = m_config.isVSyncEnabled ? 1u : 0u;
    const UINT flags = !m_config.isVSyncEnabled && m_isTearingEnabled ? DXGI_PRESENT_ALLOW_TEARING : 0u;
    const HRESULT result = m_swapChain->Present(syncInterval, flags);
    return FAILED(result) ? Result<void>::failure(swap_chain_error("IDXGISwapChain.Present", result))
                          : Result<void>::success();
}

/// @brief Factory が許可した場合だけティアリングを有効と示す
bool DX12SwapChain::is_tearing_enabled() const noexcept
{
    return m_isTearingEnabled;
}

/// @brief Graphics Queue の全提出を待ち、RTV と Back Buffer を逆順で解放する
Result<void> DX12SwapChain::shutdown()
{
    if (!m_swapChain && !m_queue)
    {
        return Result<void>::success();
    }
    if (m_queue)
    {
        auto* queue = dynamic_cast<DX12GpuCommandQueue*>(m_queue.get());
        if (!queue)
        {
            return Result<void>::failure({ErrorCategory::InvalidState, "DX12SwapChain.shutdown.queue"});
        }
        auto idleResult = queue->wait_idle();
        if (!idleResult.has_value())
        {
            return idleResult;
        }
    }
    if (m_rtvAllocator)
    {
        for (const auto handle : m_rtvHandles)
        {
            auto releaseResult = m_rtvAllocator->release(handle);
            if (!releaseResult.has_value())
            {
                return releaseResult;
            }
        }
    }
    m_rtvHandles.clear();
    m_backBuffers.clear();
    m_swapChain.Reset();
    m_queue.reset();
    m_rtvAllocator = nullptr;
    return Result<void>::success();
}
} // namespace cue::dx12
