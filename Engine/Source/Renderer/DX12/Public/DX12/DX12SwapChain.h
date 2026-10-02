#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <DX12/DX12DescriptorAllocator.h>
#include <Foundation/Result.h>
#include <RHI/Queue.h>

namespace cue::dx12
{
class DX12RenderDevice;

/// @brief Window の初期表示と Present の設定
struct DX12SwapChainConfig final
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t bufferCount = 2;
    DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
    bool isVSyncEnabled = false;
    bool isTearingAllowed = false;
};

/// @brief Window に対応する Back Buffer、RTV、Graphics Queue Lease を一意所有する
///
/// Backend が所有し、Window と RTV Allocator より先に停止する
/// 公開操作は生成 Thread から直列に呼び、GPU 提出中の破棄は shutdown が待機する
class DX12SwapChain final
{
    struct CreateToken final
    {
    };

public:
    /// @brief 部分生成状態を create 内部だけで構築する
    explicit DX12SwapChain(CreateToken) noexcept;

    /// @brief 有効な Window と Graphics Queue から Flip Model SwapChain を生成する
    ///
    /// Queue Lease を受け取り、失敗時も RTV Slot と COM Resource を回収する
    [[nodiscard]] static Result<std::unique_ptr<DX12SwapChain>> create(
        DX12RenderDevice& a_device, queueLease a_queue, DX12DescriptorAllocator& a_rtvAllocator,
        void* a_windowHandle, DX12SwapChainConfig a_config);

    /// @brief GPU 完了後に Back Buffer と Queue Lease を解放する
    ~DX12SwapChain();

    DX12SwapChain(const DX12SwapChain&) = delete;
    DX12SwapChain& operator=(const DX12SwapChain&) = delete;

    /// @brief 現在の Present 対象 Index を返す
    [[nodiscard]] std::uint32_t current_index() const noexcept;

    /// @brief 指定 Index の Back Buffer を本体の生存中だけ借用する
    [[nodiscard]] ID3D12Resource* back_buffer(std::uint32_t a_index) const noexcept;

    /// @brief 指定 Index の有効な RTV Handle を返す
    [[nodiscard]] Result<D3D12_CPU_DESCRIPTOR_HANDLE> rtv(std::uint32_t a_index) const;

    /// @brief 設定した同期方式で表示し、DXGI の失敗を Result で返す
    ///
    /// 呼出側は先に GPU 作業を提出し、Back Buffer を PRESENT State に戻す
    [[nodiscard]] Result<void> present();

    /// @brief 実際に許可されたティアリング設定を返す
    [[nodiscard]] bool is_tearing_enabled() const noexcept;

    /// @brief 発行済み Graphics Queue の完了を待って Back Buffer と RTV を解放する
    [[nodiscard]] Result<void> shutdown();

private:
    DX12DescriptorAllocator* m_rtvAllocator = nullptr;
    queueLease m_queue;
    Microsoft::WRL::ComPtr<IDXGISwapChain3> m_swapChain;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> m_backBuffers;
    std::vector<DX12DescriptorHandle> m_rtvHandles;
    DX12SwapChainConfig m_config;
    bool m_isTearingEnabled = false;
};
} // namespace cue::dx12
