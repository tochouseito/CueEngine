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

/// @brief DXGI Factory、Adapter、D3D12 Device を一意所有する
class D3D12DeviceContext final
{
public:
    /// @brief Factory から GPU 実行基盤を順に初期化する
    D3D12DeviceContext() = default;

    /// @brief Hardware を優先し、対応 Adapter がなければ WARP を試す
    [[nodiscard]] static Result<std::unique_ptr<D3D12DeviceContext>> create();

    D3D12DeviceContext(const D3D12DeviceContext&) = delete;
    D3D12DeviceContext& operator=(const D3D12DeviceContext&) = delete;

    /// @brief Presentation と Frame Context が存続する間だけ Device を借用する
    [[nodiscard]] ID3D12Device* device() const noexcept;

    /// @brief Presentation 作成中だけ Factory を借用する
    [[nodiscard]] IDXGIFactory6* factory() const noexcept;

    /// @brief 選択した Adapter の種類を診断する
    [[nodiscard]] bool is_warp() const noexcept;

    /// @brief Device 生成時に選択した最高の機能レベルを返す
    [[nodiscard]] D3D_FEATURE_LEVEL feature_level() const noexcept;

private:
    Microsoft::WRL::ComPtr<IDXGIFactory6> m_factory;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> m_adapter;
    Microsoft::WRL::ComPtr<ID3D12Device> m_device;
    bool m_isWarp = false;
    D3D_FEATURE_LEVEL m_featureLevel = {};
};
} // namespace cue::detail
