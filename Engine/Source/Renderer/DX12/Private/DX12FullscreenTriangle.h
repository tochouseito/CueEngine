#pragma once

#include <cstdint>
#include <memory>

#include <d3d12.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <Foundation/Result.h>

namespace cue::dx12
{
class DX12RenderDevice;

/// @brief FinalColorTexture を BackBuffer へ描画する固定 Pipeline を所有する
class DX12FullscreenTriangle final
{
public:
    /// @brief BackBuffer の Format に対応する Root Signature と PSO を作成する
    [[nodiscard]] static Result<std::unique_ptr<DX12FullscreenTriangle>> create(
        DX12RenderDevice& a_device, DXGI_FORMAT a_format);

    /// @brief 記録中 Graphics List へ三角形一枚の描画を記録する
    void draw(ID3D12GraphicsCommandList& a_command, ID3D12DescriptorHeap& a_heap,
              D3D12_GPU_DESCRIPTOR_HANDLE a_srv, D3D12_CPU_DESCRIPTOR_HANDLE a_rtv,
              std::uint32_t a_width, std::uint32_t a_height) const noexcept;

    DX12FullscreenTriangle(Microsoft::WRL::ComPtr<ID3D12RootSignature> a_rootSignature,
                           Microsoft::WRL::ComPtr<ID3D12PipelineState> a_pipeline) noexcept;

private:
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pipeline;
};
} // namespace cue::dx12
