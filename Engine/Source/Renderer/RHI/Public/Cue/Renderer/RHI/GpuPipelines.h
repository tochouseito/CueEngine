#pragma once

#include <Cue/Renderer/RHI/GpuResources.h>

#include <cstdint>
#include <string>
#include <vector>

namespace cue
{
enum class GpuShaderStage : std::uint8_t
{
    Vertex,
    Pixel,
    Compute
};

/// @brief UTF-8 HLSL と ASCII の Entry／任意 Profile を指定する。空 Profile は Stage の 6_0 を使う
struct GpuShaderDesc final
{
    std::string source;
    std::string entry;
    GpuShaderStage stage = GpuShaderStage::Vertex;
    std::string profile;
};

/// @brief 一つの Root Descriptor Table に置く View の種類と Register
struct GpuRootBinding final
{
    GpuViewKind kind = GpuViewKind::ConstantBuffer;
    std::uint32_t shaderRegister = 0;
    std::uint32_t registerSpace = 0;
};

struct GpuRootSignatureDesc final
{
    std::vector<GpuRootBinding> bindings;
};

enum class GpuVertexFormat : std::uint8_t
{
    Float2,
    Float3,
    Float4
};

struct GpuVertexElement final
{
    std::string semantic;
    std::uint32_t semanticIndex = 0;
    GpuVertexFormat format = GpuVertexFormat::Float3;
    std::uint32_t slot = 0;
    std::uint32_t byteOffset = 0;
};

enum class GpuCullMode : std::uint8_t
{
    None,
    Back,
    Front
};

enum class GpuBlendMode : std::uint8_t
{
    Opaque,
    Alpha
};

class IGpuPipelines;

/// @brief Pipeline Owner の破棄または世代更新で失効する Shader 識別子
struct GpuShaderHandle final
{
    std::uint32_t index = 0;
    std::uint64_t generation = 0;
    const IGpuPipelines* owner = nullptr;
};

/// @brief Pipeline Owner の破棄または世代更新で失効する Root Signature 識別子
struct GpuRootSignatureHandle final
{
    std::uint32_t index = 0;
    std::uint64_t generation = 0;
    const IGpuPipelines* owner = nullptr;
};

enum class GpuPipelineKind : std::uint8_t
{
    Graphics,
    Compute
};

/// @brief Pipeline Owner の破棄または世代更新で失効する PSO 識別子
struct GpuPipelineHandle final
{
    std::uint32_t index = 0;
    std::uint64_t generation = 0;
    GpuPipelineKind kind = GpuPipelineKind::Graphics;
    const IGpuPipelines* owner = nullptr;
};

struct GpuGraphicsPipelineDesc final
{
    GpuRootSignatureHandle rootSignature;
    GpuShaderHandle vertexShader;
    GpuShaderHandle pixelShader;
    std::vector<GpuVertexElement> vertexElements;
    GpuTextureFormat colorFormat = GpuTextureFormat::Rgba8Unorm;
    bool hasDepth = false;
    GpuTextureFormat depthFormat = GpuTextureFormat::Depth32Float;
    GpuCullMode cullMode = GpuCullMode::Back;
    GpuBlendMode blendMode = GpuBlendMode::Opaque;
};

struct GpuComputePipelineDesc final
{
    GpuRootSignatureHandle rootSignature;
    GpuShaderHandle computeShader;
};

/// @brief Shader、Root Signature、PSO の所有契約
/// @details Render Thread に専有し、同時呼出や再入は行わない。依存中の破棄を拒否し、GPU 完了後だけ PSO を解放する
class IGpuPipelines
{
public:
    virtual ~IGpuPipelines() = default;

    /// @brief UTF-8 HLSL Source を DXC で Compile し、失敗診断を返す
    [[nodiscard]] virtual Result<GpuShaderHandle> create_shader(GpuShaderDesc a_desc) = 0;

    /// @brief Descriptor Table の並びを持つ Root Signature を生成する
    [[nodiscard]] virtual Result<GpuRootSignatureHandle> create_root_signature(
        GpuRootSignatureDesc a_desc) = 0;

    /// @brief Graphics PSO と依存 Handle の関係を保持する
    [[nodiscard]] virtual Result<GpuPipelineHandle> create_graphics_pipeline(
        GpuGraphicsPipelineDesc a_desc) = 0;

    /// @brief Compute PSO と依存 Handle の関係を保持する
    [[nodiscard]] virtual Result<GpuPipelineHandle> create_compute_pipeline(
        GpuComputePipelineDesc a_desc) = 0;

    /// @brief PSO から参照中の Shader を拒否して破棄する
    [[nodiscard]] virtual Result<void> destroy_shader(GpuShaderHandle a_shader) = 0;

    /// @brief PSO から参照中の Root Signature を拒否して破棄する
    [[nodiscard]] virtual Result<void> destroy_root_signature(GpuRootSignatureHandle a_root) = 0;

    /// @brief GPU 完了後に PSO を破棄する
    [[nodiscard]] virtual Result<void> destroy_pipeline(GpuPipelineHandle a_pipeline) = 0;
};
} // namespace cue
