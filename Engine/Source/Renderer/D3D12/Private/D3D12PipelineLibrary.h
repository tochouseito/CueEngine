#pragma once

#include "D3D12DeviceContext.h"
#include "D3D12QueuePool.h"

#include <Cue/Renderer/RHI/GpuPipelines.h>

#include <dxcapi.h>

#include <memory>
#include <vector>

namespace cue::detail
{
/// @brief 任意の DXC Shader、Root Signature、Graphics／Compute PSO を所有する
class D3D12PipelineLibrary final : public IGpuPipelines
{
public:
    /// @brief Device と Queue Owner を借用して空の Registry を生成する
    [[nodiscard]] static std::unique_ptr<D3D12PipelineLibrary> create(D3D12DeviceContext& a_device,
                                                                        D3D12QueuePool& a_queues);

    /// @brief UTF-8 HLSL Source を DXC で Compile する
    [[nodiscard]] Result<GpuShaderHandle> create_shader(GpuShaderDesc a_desc) override;

    /// @brief CBV／SRV／UAV Descriptor Table を生成する
    [[nodiscard]] Result<GpuRootSignatureHandle> create_root_signature(GpuRootSignatureDesc a_desc) override;

    /// @brief Graphics PSO の依存 Handle を検証して生成する
    [[nodiscard]] Result<GpuPipelineHandle> create_graphics_pipeline(GpuGraphicsPipelineDesc a_desc) override;

    /// @brief Compute PSO の依存 Handle を検証して生成する
    [[nodiscard]] Result<GpuPipelineHandle> create_compute_pipeline(GpuComputePipelineDesc a_desc) override;

    /// @brief 生存 PSO の参照がなければ Shader を破棄する
    [[nodiscard]] Result<void> destroy_shader(GpuShaderHandle a_shader) override;

    /// @brief 生存 PSO の参照がなければ Root Signature を破棄する
    [[nodiscard]] Result<void> destroy_root_signature(GpuRootSignatureHandle a_root) override;

    /// @brief GPU 完了後に PSO Handle を失効させる
    [[nodiscard]] Result<void> destroy_pipeline(GpuPipelineHandle a_pipeline) override;

    /// @brief 有効な Graphics PSO と Root Signature を記録中 List へ設定する
    [[nodiscard]] Result<void> bind_graphics(ID3D12GraphicsCommandList* a_list,
                                              GpuPipelineHandle a_pipeline) const;

    /// @brief 有効な Compute PSO と Root Signature を記録中 List へ設定する
    [[nodiscard]] Result<void> bind_compute(ID3D12GraphicsCommandList* a_list,
                                             GpuPipelineHandle a_pipeline) const;

    /// @brief PSO の Root Parameter が指定 View 種別を受け付けるか確認する
    [[nodiscard]] bool accepts_view(GpuPipelineHandle a_pipeline, std::uint32_t a_parameter,
                                    GpuViewKind a_kind) const noexcept;

private:
    struct ShaderRecord final
    {
        Microsoft::WRL::ComPtr<IDxcBlob> blob;
        GpuShaderStage stage = GpuShaderStage::Vertex;
        std::uint64_t generation = 0;
    };

    struct RootRecord final
    {
        Microsoft::WRL::ComPtr<ID3D12RootSignature> signature;
        std::vector<GpuRootBinding> bindings;
        std::uint64_t generation = 0;
    };

    struct PipelineRecord final
    {
        Microsoft::WRL::ComPtr<ID3D12PipelineState> state;
        GpuRootSignatureHandle root;
        GpuShaderHandle firstShader;
        GpuShaderHandle secondShader;
        GpuPipelineKind kind = GpuPipelineKind::Graphics;
        std::uint64_t generation = 0;
    };

    /// @brief Owner、Index、世代と有効 Resource を照合する
    [[nodiscard]] bool owns(GpuShaderHandle a_shader) const noexcept;
    [[nodiscard]] bool owns(GpuRootSignatureHandle a_root) const noexcept;
    [[nodiscard]] bool owns(GpuPipelineHandle a_pipeline) const noexcept;

    /// @brief 解放済み Slot を世代更新して PSO を登録する
    [[nodiscard]] GpuPipelineHandle store_pipeline(PipelineRecord a_record);

    ID3D12Device* m_device = nullptr;
    D3D12QueuePool* m_queues = nullptr;
    std::vector<ShaderRecord> m_shaders;
    std::vector<RootRecord> m_roots;
    std::vector<PipelineRecord> m_pipelines;
};
} // namespace cue::detail
