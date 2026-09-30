#pragma once

#include "DX12RenderDevice.h"
#include "DX12QueuePool.h"
#include "HLSLCompiler.h"

#include <Cue/Renderer/RHI/PipelineManager.h>

#include <dxcapi.h>

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace cue::detail
{
/// @brief 任意の DXC Shader、Root Signature、Graphics／Compute PSO を所有する
class DX12PipelineManager final : public IPipelineManager
{
public:
    /// @brief Device と Queue Owner を借用して空の Registry を生成する
    [[nodiscard]] static Result<std::unique_ptr<DX12PipelineManager>> create(DX12RenderDevice& a_device,
                                                                                DX12QueuePool& a_queues);

    /// @brief UTF-8 HLSL Source を DXC で Compile する
    [[nodiscard]] Result<GpuShaderHandle> create_shader(GpuShaderDesc a_desc) override;

    /// @brief CBV／SRV／UAV Descriptor Table を生成する
    [[nodiscard]] Result<GpuRootSignatureHandle> create_root_signature(GpuRootSignatureDesc a_desc) override;

    /// @brief Graphics PSO の依存 Handle を検証して生成する
    [[nodiscard]] Result<GpuPipelineHandle> create_graphics_pipeline(GpuGraphicsPipelineDesc a_desc) override;

    /// @brief Compute PSO の依存 Handle を検証して生成する
    [[nodiscard]] Result<GpuPipelineHandle> create_compute_pipeline(GpuComputePipelineDesc a_desc) override;

    /// @brief 名前付き Shader の現行世代を返す
    [[nodiscard]] Result<GpuShaderHandle> get_shader(std::string_view a_name) const override;

    /// @brief 名前付き Root Signature の現行世代を返す
    [[nodiscard]] Result<GpuRootSignatureHandle> get_root_signature(std::string_view a_name) const override;

    /// @brief 名前付き Graphics PSO の現行世代を返す
    [[nodiscard]] Result<GpuPipelineHandle> get_graphics_pipeline(std::string_view a_name) const override;

    /// @brief 名前付き Compute PSO の現行世代を返す
    [[nodiscard]] Result<GpuPipelineHandle> get_compute_pipeline(std::string_view a_name) const override;

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

    /// @brief 生存 PSO の指定 Root Parameter Type を照合する
    [[nodiscard]] bool accepts_root_parameter(GpuPipelineHandle a_pipeline,
        std::uint32_t a_parameter, GpuRootParameterType a_type) const noexcept;

    /// @brief Root Parameter 0 に定数を持つ Graphics PSO の間接描画 Signature を貸す
    [[nodiscard]] Result<ID3D12CommandSignature*> indirect_signature(GpuPipelineHandle a_pipeline);

    /// @brief Root Table が指定容量内の Texture 範囲を参照するか検証する
    [[nodiscard]] bool accepts_texture_table(GpuPipelineHandle a_pipeline,
        std::uint32_t a_parameter, UINT a_capacity) const noexcept;

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
        std::vector<GpuRootParameterDesc> parameters;
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
        Microsoft::WRL::ComPtr<ID3D12CommandSignature> indirectSignature;
    };

    /// @brief Owner、Index、世代と有効 Resource を照合する
    [[nodiscard]] bool owns(GpuShaderHandle a_shader) const noexcept;
    [[nodiscard]] bool owns(GpuRootSignatureHandle a_root) const noexcept;
    [[nodiscard]] bool owns(GpuPipelineHandle a_pipeline) const noexcept;

    /// @brief 解放済み Slot を世代更新して PSO を登録する
    [[nodiscard]] GpuPipelineHandle store_pipeline(PipelineRecord a_record);

    ID3D12Device* m_device = nullptr;
    DX12QueuePool* m_queues = nullptr;
    std::unique_ptr<HLSLCompiler> m_compiler;
    std::vector<ShaderRecord> m_shaders;
    std::vector<RootRecord> m_roots;
    std::vector<PipelineRecord> m_pipelines;
    std::unordered_map<std::string, GpuShaderHandle> m_namedShaders;
    std::unordered_map<std::string, GpuRootSignatureHandle> m_namedRoots;
    std::unordered_map<std::string, GpuPipelineHandle> m_namedGraphicsPipelines;
    std::unordered_map<std::string, GpuPipelineHandle> m_namedComputePipelines;
};
} // namespace cue::detail
