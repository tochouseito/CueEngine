#pragma once

#include <memory>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <Foundation/Result.h>
#include <RHI/RenderDevice.h>

namespace cue::dx12
{
/// @brief Device 作成時に優先する Adapter の種類
enum class AdapterSelection
{
    HardwarePreferred,
    Warp,
};

/// @brief DXGI Factory、Adapter、D3D12 Device を一意所有する
///
/// create 成功後にのみ公開する。取得した COM Pointer は本体の破棄まで借用できる
/// 生成と破棄は呼出側で同期し、Device の利用中に本体を破棄しない
class DX12RenderDevice final : public IRenderDevice
{
    struct CreateToken final
    {
    };

public:
    /// @brief create だけが生成できる中間状態を構築する
    explicit DX12RenderDevice(CreateToken);

    /// @brief Hardware を高性能順で選び、対応 Device がなければ WARP を試す
    ///
    /// Warp 指定時は Hardware を試さず WARP を選ぶ
    /// 失敗時は部分生成した COM Resource を公開せず、HRESULT を Error に保持する
    [[nodiscard]] static Result<std::unique_ptr<DX12RenderDevice>> create(
        AdapterSelection a_selection = AdapterSelection::HardwarePreferred);

    /// @brief 所有する DXGI と D3D12 の参照を解放する
    ~DX12RenderDevice() override = default;

    DX12RenderDevice(const DX12RenderDevice&) = delete;
    DX12RenderDevice& operator=(const DX12RenderDevice&) = delete;

    /// @brief Device を本体の生存中だけ借用する
    [[nodiscard]] ID3D12Device* device() const noexcept;

    /// @brief Factory を本体の生存中だけ借用する
    [[nodiscard]] IDXGIFactory6* factory() const noexcept;

    /// @brief 選択した Adapter を本体の生存中だけ借用する
    [[nodiscard]] IDXGIAdapter1* adapter() const noexcept;

    /// @brief 選択した Adapter が WARP か返す
    [[nodiscard]] bool is_warp() const noexcept;

    /// @brief RHI 契約で Software Adapter の選択結果を返す
    [[nodiscard]] bool is_software_adapter() const noexcept override;

    /// @brief Device 生成に成功した最高の Feature Level を返す
    [[nodiscard]] D3D_FEATURE_LEVEL feature_level() const noexcept;

private:
    Microsoft::WRL::ComPtr<IDXGIFactory6> m_factory;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> m_adapter;
    Microsoft::WRL::ComPtr<ID3D12Device> m_device;
    bool m_isWarp = false;
    D3D_FEATURE_LEVEL m_featureLevel = {};
};
} // namespace cue::dx12
