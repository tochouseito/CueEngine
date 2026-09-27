#pragma once

#include <Cue/Foundation/Result.h>

#include <cstdint>
#include <memory>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

namespace cue::detail
{
/// @brief D3D12 の失敗を操作名と HRESULT で呼出側へ渡す
inline Error gpu_error(const char* a_operation, HRESULT a_result)
{
    return {ErrorCategory::PlatformFailure, a_operation, static_cast<std::int64_t>(a_result)};
}

/// @brief Device、Direct Queue、Fence と待機 Event を一意所有する
class D3D12DeviceContext final
{
public:
    /// @brief Factory から GPU 実行基盤を順に初期化する
    D3D12DeviceContext() = default;

    /// @brief Hardware 優先または明示 WARP で GPU の実行基盤を生成する
    [[nodiscard]] static Result<std::unique_ptr<D3D12DeviceContext>> create(bool a_useWarp);

    /// @brief Event を閉じてから COM 資源を解放する
    ~D3D12DeviceContext();

    D3D12DeviceContext(const D3D12DeviceContext&) = delete;
    D3D12DeviceContext& operator=(const D3D12DeviceContext&) = delete;

    /// @brief 指定 Fence 値の GPU 完了を確認する
    [[nodiscard]] Result<void> wait_for(std::uint64_t a_value) const;

    /// @brief Direct Queue に完了印を入れ、先行する全処理を待つ
    [[nodiscard]] Result<void> wait_idle();

    /// @brief Direct Queue 上の現在位置を Fence に記録する
    [[nodiscard]] Result<std::uint64_t> signal();

    /// @brief Presentation と Frame Context が存続する間だけ Device を借用する
    [[nodiscard]] ID3D12Device* device() const noexcept;

    /// @brief Presentation と Frame Context が存続する間だけ Queue を借用する
    [[nodiscard]] ID3D12CommandQueue* queue() const noexcept;

    /// @brief Presentation 作成中だけ Factory を借用する
    [[nodiscard]] IDXGIFactory6* factory() const noexcept;

    /// @brief 選択した Adapter の種類を診断する
    [[nodiscard]] bool is_warp() const noexcept;

private:
    Microsoft::WRL::ComPtr<IDXGIFactory6> m_factory;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> m_adapter;
    Microsoft::WRL::ComPtr<ID3D12Device> m_device;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_queue;
    Microsoft::WRL::ComPtr<ID3D12Fence> m_fence;
    HANDLE m_fenceEvent = nullptr;
    std::uint64_t m_nextFenceValue = 1;
    bool m_isWarp = false;
};
} // namespace cue::detail
