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

/// @brief FinalColorTexture を Back Buffer 全面へ描く Root Signature と PSO を所有する
///
/// Shader Bytecode は Build 時に DXC で生成する。記録操作は呼出側が直列化する
/// 描画中の Descriptor Heap と PSO は対応する GPU 完了まで生存させる
class DX12FullscreenPipeline final
{
    struct CreateToken final
    {
    };

public:
    /// @brief create 内部だけで空の Pipeline を構築する
    explicit DX12FullscreenPipeline(CreateToken) noexcept;

    /// @brief SwapChain Format に対応する Root Signature と PSO を生成する
    ///
    /// 失敗時は HRESULT を含む Error を返し、部分生成物を公開しない
    [[nodiscard]] static Result<std::unique_ptr<DX12FullscreenPipeline>> create(
        DX12RenderDevice& a_device, DXGI_FORMAT a_targetFormat);

    /// @brief Root Signature と PSO を解放する
    ~DX12FullscreenPipeline() = default;

    DX12FullscreenPipeline(const DX12FullscreenPipeline&) = delete;
    DX12FullscreenPipeline& operator=(const DX12FullscreenPipeline&) = delete;

    /// @brief FinalColor の SRV を使う全画面三角形 Draw を記録する
    ///
    /// 呼出側は Texture を ShaderRead、Back Buffer を RenderTarget State に遷移させる
    [[nodiscard]] Result<void> draw(ID3D12GraphicsCommandList& a_list, ID3D12DescriptorHeap& a_srvHeap,
                                    D3D12_GPU_DESCRIPTOR_HANDLE a_srv, D3D12_CPU_DESCRIPTOR_HANDLE a_target,
                                    std::uint32_t a_width, std::uint32_t a_height) const;

private:
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pipeline;
};
} // namespace cue::dx12
