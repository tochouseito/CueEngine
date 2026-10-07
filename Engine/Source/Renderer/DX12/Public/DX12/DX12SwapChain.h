#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <DX12/DX12Contexts.h>
#include <DX12/DX12ViewManager.h>
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
/// Backend が所有し、Window と Context の参照先より先に停止する
/// 公開操作は呼出側で直列化する。Resize と Present は同じ Render Thread から呼べる
/// Window の Message Thread は DXGI の処理中も Message Pump を続ける
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
    [[nodiscard]] static Result<std::unique_ptr<DX12SwapChain>> create(const DX12ResourceContext &a_resources,
                                                                       queueLease a_queue, void *a_windowHandle,
                                                                       DX12SwapChainConfig a_config);

    /// @brief GPU 完了後に Back Buffer と Queue Lease を解放する
    ~DX12SwapChain();

    DX12SwapChain(const DX12SwapChain&) = delete;
    DX12SwapChain& operator=(const DX12SwapChain&) = delete;

    /// @brief 現在の Present 対象 Index を返す
    [[nodiscard]] std::uint32_t current_index() const noexcept;

    /// @brief 指定 Index の Back Buffer を次の Resize または停止まで借用する
    [[nodiscard]] ID3D12Resource* back_buffer(std::uint32_t a_index) const noexcept;

    /// @brief 指定 Index の有効な RTV Handle を返す
    [[nodiscard]] Result<D3D12_CPU_DESCRIPTOR_HANDLE> rtv(std::uint32_t a_index) const;

    /// @brief SwapChain を作成した Graphics Queue を本体の生存中だけ借用する
    ///
    /// 描画 Command はこの Queue に提出してから Present する
    [[nodiscard]] IQueueContext* graphics_queue() const noexcept;

    /// @brief 設定した同期方式で表示し、DXGI の失敗を Result で返す
    ///
    /// 呼出側は先に GPU 作業を提出し、Back Buffer を PRESENT State に戻す
    [[nodiscard]] Result<void> present();

    /// @brief GPU 完了後に BackBuffer と RTV を新しい非零寸法で作り直す
    ///
    /// 呼出前に Graph、外部 View と Command の旧 BackBuffer 参照を解除する
    /// Queue Lease、Format、Buffer 数、VSync と Tearing 設定は維持する
    /// 再取得が失敗した場合は Present せず、resize の再試行または shutdown を行う
    [[nodiscard]] Result<void> resize(std::uint32_t a_width, std::uint32_t a_height);

    /// @brief 実際に許可されたティアリング設定を返す
    [[nodiscard]] bool is_tearing_enabled() const noexcept;

    /// @brief 発行済み Graphics Queue の完了を待って Back Buffer と RTV を解放する
    [[nodiscard]] Result<void> shutdown();

private:
  /// @brief Native BackBuffer と RTV を全枠分取得し、途中生成も Owner へ保持する
  [[nodiscard]] Result<void> acquire_buffers();

  /// @brief GPU 完了後に RTV を返却し、成功した枠の参照から解除する
  [[nodiscard]] Result<void> release_buffers();

  DX12ViewManager *m_viewManager = nullptr;
  queueLease m_queue;
  Microsoft::WRL::ComPtr<IDXGISwapChain3> m_swapChain;
  std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> m_backBuffers;
  std::vector<DX12ViewHandle> m_rtvHandles;
  DX12SwapChainConfig m_config;
  bool m_isTearingEnabled = false;
};
} // namespace cue::dx12
