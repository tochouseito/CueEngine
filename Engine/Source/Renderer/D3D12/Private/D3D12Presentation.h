#pragma once

#include "D3D12DeviceContext.h"
#include "D3D12ViewManager.h"

#include <Cue/Platform/WindowEvent.h>

#include <array>
#include <memory>

namespace cue::detail
{
inline constexpr UINT k_backBufferCount = 2;

/// @brief Swap Chain と Back Buffer を所有し、RTV Slot を View Manager から借用する
class D3D12Presentation final
{
public:
    /// @brief View Manager より短い寿命で表示資源を作る
    explicit D3D12Presentation(D3D12ViewManager& a_views) noexcept;

    /// @brief 借用した RTV Slot を View Manager に返す
    ~D3D12Presentation();

    D3D12Presentation(const D3D12Presentation&) = delete;
    D3D12Presentation& operator=(const D3D12Presentation&) = delete;

    /// @brief 有効な Window Handle に結び付く表示資源を作る
    ///
    /// Device Context は本体より長く、Window は本体の停止まで生存する
    [[nodiscard]] static Result<std::unique_ptr<D3D12Presentation>> create(D3D12DeviceContext& a_device,
                                                                              D3D12ViewManager& a_views,
                                                                              void* a_nativeWindow,
                                                                              WindowSize a_size);

    /// @brief GPU 完了後、Command List が旧 Buffer を参照しない状態で Surface を更新する
    [[nodiscard]] Result<void> resize(D3D12DeviceContext& a_device, WindowSize a_size);

    /// @brief 現在の Back Buffer の Index を返す
    [[nodiscard]] UINT current_index() const noexcept;

    /// @brief Presentation が存続し、次の Resize までだけ Buffer を借用する
    [[nodiscard]] ID3D12Resource* back_buffer(UINT a_index) const noexcept;

    /// @brief Presentation が存続する間だけ RTV の CPU Handle を借用する
    [[nodiscard]] Result<D3D12_CPU_DESCRIPTOR_HANDLE> rtv(UINT a_index) const;

    /// @brief Submit 済みの Back Buffer を表示する
    [[nodiscard]] Result<void> present();

    /// @brief 現在適用済みの Client Size を返す
    [[nodiscard]] WindowSize size() const noexcept;

private:
    /// @brief Buffer Slot と RTV Slot を同じ Index で結び付ける
    [[nodiscard]] Result<void> acquire_buffers(ID3D12Device* a_device);

    Microsoft::WRL::ComPtr<IDXGISwapChain3> m_swapChain;
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, k_backBufferCount> m_backBuffers;
    std::array<RtvSlot, k_backBufferCount> m_rtvSlots{};
    D3D12ViewManager& m_views;
    WindowSize m_size{};
};
} // namespace cue::detail
