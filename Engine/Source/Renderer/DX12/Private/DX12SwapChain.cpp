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
Result<std::unique_ptr<DX12SwapChain>> DX12SwapChain::create(const DX12ResourceContext &a_resources, queueLease a_queue,
                                                             void *a_windowHandle, DX12SwapChainConfig a_config)
{
    using SwapResult = Result<std::unique_ptr<DX12SwapChain>>;
    auto &device = a_resources.get_render_device();
    auto &viewManager = a_resources.get_view_manager();
    auto* queue = dynamic_cast<DX12GpuCommandQueue*>(a_queue.get());
    if (!device.device() ||                                   // Device があるか
        !device.factory() ||                                  // Factory があるか
        !queue || !queue->command_queue() ||                  // Queue があるか
        queue->device() != device.device() ||                 // Queue が Device に属するか
        queue->type() != QueueType::Graphics ||               // Queue が Graphics か
        !a_windowHandle ||                                    // Window Handle があるか
        !IsWindow(static_cast<HWND>(a_windowHandle)) ||       // Window Handle が有効か
        a_config.width == 0 ||                                // SwapChain の幅があるか
        a_config.height == 0 ||                               // SwapChain の高さがあるか
        a_config.bufferCount < 2 ||                           // SwapChain の Back Buffer が 2 以上か
        a_config.bufferCount > DXGI_MAX_SWAP_CHAIN_BUFFERS || // SwapChain の Back Buffer が上限以下か
        (a_config.format != DXGI_FORMAT_R8G8B8A8_UNORM &&
         a_config.format != DXGI_FORMAT_B8G8R8A8_UNORM) || // SwapChain のフォーマットが有効か
        viewManager.device() != device.device())           // View の生成基盤が同じか
    {
        return SwapResult::failure({ErrorCategory::InvalidArgument, "DX12SwapChain.create"});
    }

    // DXGI が Tearing を許可するか確認する
    BOOL canTear = false;
    const HRESULT featureResult =
        device.factory()->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &canTear, sizeof(canTear));
    const bool isTearingEnabled = a_config.isTearingAllowed && SUCCEEDED(featureResult) && canTear != false;

    // メモリ確保失敗時は bad_alloc を catch で拾う
    try
    {
        // SwapChain 生成
        auto result = std::make_unique<DX12SwapChain>(CreateToken{});
        result->m_viewManager = &viewManager;
        result->m_queue = std::move(a_queue);
        result->m_config = a_config;
        result->m_isTearingEnabled = isTearingEnabled;
        result->m_backBuffers.reserve(a_config.bufferCount);
        result->m_rtvHandles.reserve(a_config.bufferCount);

        // SwapChain の設定を構築する
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = a_config.width;
        desc.Height = a_config.height;
        desc.Format = a_config.format;
        desc.Stereo = false; // 3D Stereo は未対応
        desc.SampleDesc.Count = 1; // マルチサンプルは未対応
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; // Back Buffer は Render Target で使う
        desc.BufferCount = a_config.bufferCount;            // Back Buffer の数を指定する
        desc.Scaling = DXGI_SCALING_STRETCH;                // Stretch 以外は未対応
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;    // Flip Model で Back Buffer を破棄する
        desc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;       // Back Buffer の Alpha は未対応
        desc.Flags = isTearingEnabled ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0; // Tearing を許可するか

        // SwapChain を生成する
        Microsoft::WRL::ComPtr<IDXGISwapChain1> created;
        const HRESULT createResult = device.factory()->CreateSwapChainForHwnd(
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

        // Alt+Enter でフルスクリーン切替を無効化する
        const HRESULT associationResult =
            device.factory()->MakeWindowAssociation(static_cast<HWND>(a_windowHandle), DXGI_MWA_NO_ALT_ENTER);
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

        // 初回生成と Resize で同じ取得経路を使い、Object 名と RTV の所有を揃える
        auto buffersResult = result->acquire_buffers();
        if (!buffersResult.has_value())
        {
            return SwapResult::failure(*buffersResult.try_error());
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
    if (!m_viewManager || a_index >= m_rtvHandles.size())
    {
        return Result<D3D12_CPU_DESCRIPTOR_HANDLE>::failure(
            {ErrorCategory::InvalidArgument, "DX12SwapChain.rtv"});
    }
    return m_viewManager->cpu_handle(m_rtvHandles[a_index]);
}

/// @brief 描画提出と Present が同じ Queue を使えるように貸し出す
IQueueContext* DX12SwapChain::graphics_queue() const noexcept
{
    return m_queue.get();
}

/// @brief VSync と DXGI のティアリング制約を一箇所で適用する
Result<void> DX12SwapChain::present()
{
    if (!m_swapChain || m_backBuffers.size() != m_config.bufferCount || m_rtvHandles.size() != m_config.bufferCount)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "DX12SwapChain.present"});
    }
    const UINT syncInterval = m_config.isVSyncEnabled ? 1u : 0u;
    const UINT flags = !m_config.isVSyncEnabled && m_isTearingEnabled ? DXGI_PRESENT_ALLOW_TEARING : 0u;
    const HRESULT result = m_swapChain->Present(syncInterval, flags);
    return FAILED(result) ? Result<void>::failure(swap_chain_error("IDXGISwapChain.Present", result))
                          : Result<void>::success();
}

/// @brief Graph が借用を解除した後に、同じ Native SwapChain の表示枠だけを更新する
Result<void> DX12SwapChain::resize(std::uint32_t a_width, std::uint32_t a_height)
{
    // 最小化は Host が表示を停止して扱う。DXGI の暗黙 ClientSize 取得へ零寸法を渡さない
    if (a_width == 0 || a_height == 0)
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "DX12SwapChain.resize.size"});
    }
    auto *queue = dynamic_cast<DX12GpuCommandQueue *>(m_queue.get());
    if (!m_swapChain || !m_viewManager || !queue)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "DX12SwapChain.resize"});
    }
    // 再取得途中の失敗では同じ寸法でも再試行する。正常な同一サイズだけを省略する
    if (a_width == m_config.width && a_height == m_config.height && m_backBuffers.size() == m_config.bufferCount &&
        m_rtvHandles.size() == m_config.bufferCount)
    {
        return Result<void>::success();
    }
    // Present を含む同一 Queue の処理完了まで、旧 RTV と BackBuffer を残す
    auto idleResult = queue->wait_idle();
    if (!idleResult.has_value())
    {
        return idleResult;
    }
    auto releaseResult = release_buffers();
    if (!releaseResult.has_value())
    {
        return releaseResult;
    }
    // Flip Model の枠数と Format、生成時の Tearing Flag を維持する
    const UINT flags = m_isTearingEnabled ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u;
    const HRESULT resizeResult =
        m_swapChain->ResizeBuffers(m_config.bufferCount, a_width, a_height, m_config.format, flags);
    if (FAILED(resizeResult))
    {
        // 旧 Buffer を公開し直さず、元の DXGI Error を保持して停止または再試行する
        return Result<void>::failure(swap_chain_error("IDXGISwapChain.ResizeBuffers", resizeResult));
    }
    m_config.width = a_width;
    m_config.height = a_height;
    try
    {
        return acquire_buffers();
    }
    catch (const std::bad_alloc &)
    {
        // 部分取得した Buffer と View は Owner に残し、次の resize / shutdown で回収する
        return Result<void>::failure({ErrorCategory::PlatformFailure, "DX12SwapChain.resize.allocation"});
    }
}

/// @brief 全 BackBuffer を再取得し、同じ命名と View 生成規則を適用する
Result<void> DX12SwapChain::acquire_buffers()
{
    for (std::uint32_t index = 0; index < m_config.bufferCount; ++index)
    {
        // Native Resource は RTV 生成の成功まで Local の COM 所有で保護する
        Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
        const HRESULT bufferResult = m_swapChain->GetBuffer(index, IID_PPV_ARGS(&buffer));
        if (FAILED(bufferResult))
        {
            return Result<void>::failure(swap_chain_error("IDXGISwapChain.GetBuffer", bufferResult));
        }
        const std::wstring bufferName = L"CueEngine DX12 Back Buffer " + std::to_wstring(index);
        const HRESULT nameResult = buffer->SetName(bufferName.c_str());
        if (FAILED(nameResult))
        {
            report_error("DX12SwapChain", swap_chain_error("ID3D12Resource.SetName", nameResult),
                         DiagnosticSeverity::Warning);
        }
        auto viewResult = m_viewManager->create_rtv(*buffer.Get());
        if (!viewResult.has_value())
        {
            return Result<void>::failure(*viewResult.try_error());
        }
        // create で全枠分の capacity を確保済みのため、View 取得後の push は Allocation しない
        m_rtvHandles.push_back(viewResult.take_value());
        m_backBuffers.push_back(std::move(buffer));
    }
    return Result<void>::success();
}

/// @brief 解放済み Handle を残さず、途中失敗後の再試行で二重返却しない
Result<void> DX12SwapChain::release_buffers()
{
    while (!m_rtvHandles.empty())
    {
        auto releaseResult = m_viewManager->release(m_rtvHandles.back());
        if (!releaseResult.has_value())
        {
            return releaseResult;
        }
        m_rtvHandles.pop_back();
    }
    // Descriptor Slot を返してから Owner の BackBuffer 直接参照を解除する
    m_backBuffers.clear();
    return Result<void>::success();
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
    auto releaseResult = release_buffers();
    if (!releaseResult.has_value())
    {
        return releaseResult;
    }
    m_swapChain.Reset();
    m_queue.reset();
    m_viewManager = nullptr;
    return Result<void>::success();
}
} // namespace cue::dx12
