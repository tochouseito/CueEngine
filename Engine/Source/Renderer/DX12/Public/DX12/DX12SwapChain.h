#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <vector>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <DX12/DX12Contexts.h>
#include <DX12/DX12ViewManager.h>
#include <Foundation/Result.h>
#include <RHI/Queue.h>
#include <RHI/Presentation.h>

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
    bool isWaitableEnabled = true;
    std::uint32_t maxQueuedFrames = 1;
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

    /// @brief 描画開始の表示枠を取得する。停止時は false、取得済み枠は Present まで保持する
    /// Render Thread 専用。呼出終了前に shutdown / resize を実行しない
    [[nodiscard]] Result<bool> wait_for_frame(std::stop_token a_stopToken,
                                              const std::function<bool()> &a_shouldCancel = {},
                                              bool a_isNonBlocking = false, bool a_wakesForMessages = false);

    /// @brief 実際に作成された表示待機機能の有無を返す
    [[nodiscard]] bool is_waitable_enabled() const noexcept;

    /// @brief SetMaximumFrameLatency が成功した上限を返す。Waitable 無効 / 停止後は未適用
    [[nodiscard]] std::optional<std::uint32_t> queued_frame_limit() const noexcept;

    /// @brief Render Thread で Present API と DXGI 統計の所有値を取得する
    [[nodiscard]] PresentationTiming timing() const noexcept;

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

  /// @brief 不連続と Monitor 変更で予測履歴を無効化し、取得できた DXGI 統計だけ保持する
  void update_timing();

  DX12ViewManager *m_viewManager = nullptr;
  queueLease m_queue;
  Microsoft::WRL::ComPtr<IDXGISwapChain3> m_swapChain;
  std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> m_backBuffers;
  std::vector<DX12ViewHandle> m_rtvHandles;
  DX12SwapChainConfig m_config;
  bool m_isTearingEnabled = false;
  // DXGI から受け取った Handle を一意所有する。停止前に利用 Thread を join する
  HANDLE m_frameLatencyEvent = nullptr;
  bool m_hasFramePermit = false;
  PresentationTiming m_timing;
  std::optional<DXGI_FRAME_STATISTICS> m_previousStatistics;
  HWND m_window = nullptr;
  HMONITOR m_monitor = nullptr;
};
} // namespace cue::dx12
