#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <Foundation/Result.h>
#include <RHI/GpuResource.h>

namespace cue
{
/// @brief Manager の所属と Slot の世代を識別する非所有 Root Signature Handle
struct RootSignatureHandle final
{
    std::uint32_t index = 0;
    std::uint64_t generation = 0;
    std::uint64_t managerId = 0;

    /// @brief 初期値のままか判定する
    [[nodiscard]] bool is_valid() const noexcept
    {
        return generation != 0 && managerId != 0;
    }
};

/// @brief Manager の所属と Slot の世代を識別する非所有 Shader Blob Handle
struct ShaderBlobHandle final
{
    std::uint32_t index = 0;
    std::uint64_t generation = 0;
    std::uint64_t managerId = 0;

    /// @brief 初期値のままか判定する
    [[nodiscard]] bool is_valid() const noexcept
    {
        return generation != 0 && managerId != 0;
    }
};

/// @brief Manager の所属と Slot の世代を識別する非所有 Pipeline Handle
struct PipelineStateHandle final
{
    std::uint32_t index = 0;
    std::uint64_t generation = 0;
    std::uint64_t managerId = 0;

    /// @brief 初期値のままか判定する
    [[nodiscard]] bool is_valid() const noexcept
    {
        return generation != 0 && managerId != 0;
    }
};

/// @brief コンパイル対象の Shader Stage
enum class ShaderStage
{
    Vertex,
    Pixel,
    Compute
};
/// @brief Root Parameter の GPU Binding 方式
enum class RootParameterType
{
    SrvTable,
    CbvTable,
    UavTable,
    Constants,
    ConstantBuffer,
    ShaderResource,
    UnorderedAccess
};
/// @brief Root Parameter が参照される Shader Stage
enum class ShaderVisibility
{
    All,
    Vertex,
    Pixel
};
/// @brief Primitive の接続方式
enum class PrimitiveTopology
{
    Triangle,
    Line,
    Point
};
/// @brief Rasterizer が除外する面
enum class CullMode
{
    None,
    Front,
    Back
};
/// @brief Texture Sample の補間方式
enum class SamplerFilter
{
    Point,
    Linear
};
/// @brief Texture 座標の境界処理
enum class SamplerAddressMode
{
    Clamp,
    Wrap
};

/// @brief Backend に依存しない一つの Root Binding
struct RootParameterDesc final
{
    RootParameterType type = RootParameterType::SrvTable;
    ShaderVisibility visibility = ShaderVisibility::All;
    std::uint32_t shaderRegister = 0;
    std::uint32_t registerSpace = 0;
    std::uint32_t count = 1;
};

/// @brief Root Signature に固定する Sampler
struct StaticSamplerDesc final
{
    std::uint32_t shaderRegister = 0;
    std::uint32_t registerSpace = 0;
    ShaderVisibility visibility = ShaderVisibility::Pixel;
    SamplerFilter filter = SamplerFilter::Linear;
    SamplerAddressMode address = SamplerAddressMode::Clamp;
};

/// @brief Root Signature の構成を値として所有する
struct RootSignatureDesc final
{
    std::string name;
    std::vector<RootParameterDesc> parameters;
    std::vector<StaticSamplerDesc> samplers;
};

/// @brief HLSL とコンパイル条件を値として所有する
struct ShaderCompileDesc final
{
    std::string name;
    std::string filePath; // 相対 Path は Engine/Shader を基準に解決する
    std::string entryPoint;
    ShaderStage stage = ShaderStage::Vertex;
    std::string targetProfile;        // 空なら Stage に対応する Shader Model 6.0
    std::vector<std::string> defines; // NAME または NAME=VALUE
};

/// @brief 非 MSAA、単一 Color Target の Graphics Pipeline 設定
struct GraphicsPipelineStateDesc final
{
    std::string name;
    RootSignatureHandle rootSignature;
    ShaderBlobHandle vertexShader;
    ShaderBlobHandle pixelShader;
    GpuTextureFormat renderTargetFormat = GpuTextureFormat::Rgba8Unorm;
    PrimitiveTopology topology = PrimitiveTopology::Triangle;
    CullMode cullMode = CullMode::None;
    bool isWireframe = false;
    bool isAlphaBlendEnabled = false;
};

/// @brief Compute Pipeline の依存 Handle と設定
struct ComputePipelineStateDesc final
{
    std::string name;
    RootSignatureHandle rootSignature;
    ShaderBlobHandle computeShader;
};

/// @brief Shader、Root Signature と Pipeline の生成・所有・世代管理の契約
///
/// Backend が所有し、呼出側は操作を直列化する。Handle は非所有で retire 時に失効する
/// 記録中と提出済み Command が Pipeline と Root の実体を GPU 完了まで保持する
/// Shader Blob は CPU の生成入力であり、PSO 生成後の retire は PSO を無効化しない
class IPipelineManager
{
  public:
    /// @brief 具体 Manager の所有状態を解放する
    virtual ~IPipelineManager() = default;
    /// @brief 所有 Registry の複製を禁止する
    IPipelineManager(const IPipelineManager &) = delete;
    /// @brief 所有 Registry の複製を禁止する
    IPipelineManager &operator=(const IPipelineManager &) = delete;

    /// @brief Root の構成を検証して生成し、失敗時は何も公開しない
    [[nodiscard]] virtual Result<RootSignatureHandle> create_root_signature(RootSignatureDesc a_desc) = 0;
    /// @brief HLSL をコンパイルして Blob を保持し、診断を Error へ返す
    [[nodiscard]] virtual Result<ShaderBlobHandle> create_shader_blob(ShaderCompileDesc a_desc) = 0;
    /// @brief 同じ Manager の Root と VS／PS を使って Graphics PSO を生成する
    [[nodiscard]] virtual Result<PipelineStateHandle> create_graphics_pipeline(GraphicsPipelineStateDesc a_desc) = 0;
    /// @brief 同じ Manager の Root と CS を使って Compute PSO を生成する
    [[nodiscard]] virtual Result<PipelineStateHandle> create_compute_pipeline(ComputePipelineStateDesc a_desc) = 0;
    /// @brief Handle を失効させ、参照中の Pipeline が持つ Root の実体は維持する
    [[nodiscard]] virtual Result<void> retire(RootSignatureHandle a_handle) = 0;
    /// @brief CPU 入力の Blob を解放する
    [[nodiscard]] virtual Result<void> retire(ShaderBlobHandle a_handle) = 0;
    /// @brief Handle を失効させ、Command が参照する実体は GPU 完了まで維持する
    [[nodiscard]] virtual Result<void> retire(PipelineStateHandle a_handle) = 0;

  protected:
    /// @brief 具体 Manager の生成経路だけが基底契約を構築する
    IPipelineManager() = default;
};
} // namespace cue
