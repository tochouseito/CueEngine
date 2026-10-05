#pragma once

#include <memory>
#include <span>

#include <d3d12.h>

#include <RHI/PipelineManager.h>

namespace cue::dx12
{
class DX12RenderDevice;
class DX12GpuCommandContext;

/// @brief DXC Blob、Root Signature、Graphics／Compute PSO を Registry で所有する
///
/// Backend が所有し、全操作を直列化する。Command は Native Pipeline と Root を共有保持する
class DX12PipelineManager final : public IPipelineManager
{
    struct CreateToken final
    {
    };

  public:
    /// @brief create 内部でのみ空の所有状態を構築する
    explicit DX12PipelineManager(CreateToken) noexcept;
    /// @brief Device を保持し、DXC の生成基盤を準備する
    [[nodiscard]] static Result<std::unique_ptr<DX12PipelineManager>> create(DX12RenderDevice &a_device);
    /// @brief Registry の所有を解放する。記録・提出済み Command の参照は独立して残る
    ~DX12PipelineManager() override;
    /// @brief Registry の複製を禁止する
    DX12PipelineManager(const DX12PipelineManager &) = delete;
    /// @brief Registry の複製を禁止する
    DX12PipelineManager &operator=(const DX12PipelineManager &) = delete;
    /// @brief RHI の Binding を Native Root Signature へ変換する
    [[nodiscard]] Result<RootSignatureHandle> create_root_signature(RootSignatureDesc a_desc) override;
    /// @brief DXC で Stage に対応する Blob を生成する
    [[nodiscard]] Result<ShaderBlobHandle> create_shader_blob(ShaderCompileDesc a_desc) override;
    /// @brief Root と Shader の所属と Stage を検証して Graphics PSO を生成する
    [[nodiscard]] Result<PipelineStateHandle> create_graphics_pipeline(GraphicsPipelineStateDesc a_desc) override;
    /// @brief Root と CS の所属と Stage を検証して Compute PSO を生成する
    [[nodiscard]] Result<PipelineStateHandle> create_compute_pipeline(ComputePipelineStateDesc a_desc) override;
    /// @brief Root Handle を失効させ、依存 PSO が借用する実体は残す
    [[nodiscard]] Result<void> retire(RootSignatureHandle a_handle) override;
    /// @brief Shader Handle を失効させて CPU Blob を解放する
    [[nodiscard]] Result<void> retire(ShaderBlobHandle a_handle) override;
    /// @brief PSO Handle を失効させ、Command の共有参照へ寿命を引き継ぐ
    [[nodiscard]] Result<void> retire(PipelineStateHandle a_handle) override;
    /// @brief Graphics Pipeline を記録し、対応する描画先 Format を返す
    [[nodiscard]] Result<GpuTextureFormat> bind_graphics(DX12GpuCommandContext &a_command,
                                                         PipelineStateHandle a_handle);
    /// @brief Compute Pipeline を記録して Native 実体の寿命を Command へ保持する
    [[nodiscard]] Result<void> bind_compute(DX12GpuCommandContext &a_command, PipelineStateHandle a_handle);
    /// @brief 単一 Texture の Binding 先が SRV Table か検証する
    [[nodiscard]] Result<void> validate_texture_binding(PipelineStateHandle a_handle,
                                                        std::uint32_t a_rootParameter) const;
    /// @brief Draw または Dispatch に必要な Root Parameter が全て設定されているか検証する
    [[nodiscard]] Result<void> validate_bindings(PipelineStateHandle a_handle,
                                                 std::span<const std::uint32_t> a_boundParameters) const;
    /// @brief Context と同じ Device の生成基盤か確認する
    [[nodiscard]] ID3D12Device *device() const noexcept;

  private:
    struct State;
    std::unique_ptr<State> m_state;
};
} // namespace cue::dx12
