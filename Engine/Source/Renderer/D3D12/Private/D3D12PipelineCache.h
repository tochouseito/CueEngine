#pragma once

#include "D3D12Presentation.h"

#include <array>
#include <memory>

namespace cue::detail
{
/// @brief 固定 Mesh Pass の Shader、Root Signature、PSO と CBV を所有して再利用する
class D3D12PipelineCache final
{
public:
    /// @brief Shader のコンパイルと PSO 構築を完了してから公開する
    [[nodiscard]] static Result<std::unique_ptr<D3D12PipelineCache>> create(D3D12DeviceContext& a_device);

    /// @brief 常時 Map した Upload Buffer を解放前に Unmap する
    ~D3D12PipelineCache();

    /// @brief 対応 Frame Slot の Fence 完了後に定数を書き換えて Pipeline を設定する
    [[nodiscard]] Result<void> bind(ID3D12GraphicsCommandList* a_list, UINT a_slot,
                                     const std::array<float, 4>& a_tint);

private:
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pipeline;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_cbvHeap;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_constants;
    UINT8* m_mappedConstants = nullptr;
    UINT m_descriptorStride = 0;
};
} // namespace cue::detail
