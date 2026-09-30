#pragma once

#include <Cue/Renderer/RHI/GpuExecution.h>
#include <Cue/Renderer/RHI/GpuPipelines.h>

#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace cue
{
enum class GpuResourceState : std::uint8_t
{
    Common,
    RenderTarget,
    DepthWrite,
    ShaderResource,
    ComputeShaderResource,
    CopySource,
    CopyDest,
    UnorderedAccess,
    VertexIndex,
    IndirectArgument
};

enum class GpuIndexFormat : std::uint8_t
{
    Uint16,
    Uint32
};

struct GpuBufferCopyRegion final
{
    GpuResourceHandle destination;
    GpuMemory destinationMemory = GpuMemory::Device;
    std::uint32_t destinationIndex = 0;
    std::uint64_t destinationOffset = 0;
    GpuResourceHandle source;
    GpuMemory sourceMemory = GpuMemory::Upload;
    std::uint32_t sourceIndex = 0;
    std::uint64_t sourceOffset = 0;
    std::uint64_t size = 0;
};

struct GpuTextureToBufferCopyRegion final
{
    GpuResourceHandle source;
    std::uint32_t sourceIndex = 0;
    std::uint32_t sourceX = 0;
    std::uint32_t sourceY = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    GpuResourceHandle destination;
    std::uint32_t destinationIndex = 0;
    std::uint64_t destinationOffset = 0;
};

/// @brief 一つの貸出 Command List にだけ記録し、Submit 後には使用しない
/// @details Resource、View、Pipeline Handle の Owner は GPU 完了まで生存させる。記録は Render Thread に専有する
class IGpuCommandRecorder
{
public:
    virtual ~IGpuCommandRecorder() = default;

    /// @brief Resource 全体の状態を遷移する
    [[nodiscard]] virtual Result<void> transition(GpuResourceHandle a_resource, GpuResourceState a_before,
                                                   GpuResourceState a_after) = 0;

    /// @brief 先行 UAV 書込みと後続アクセスを順序付ける
    [[nodiscard]] virtual Result<void> uav_barrier(GpuResourceHandle a_resource) = 0;

    /// @brief Buffer の指定範囲をコピーする
    [[nodiscard]] virtual Result<void> copy_buffer(GpuResourceHandle a_destination, std::uint64_t a_destinationOffset,
                                                   GpuResourceHandle a_source, std::uint64_t a_sourceOffset,
                                                   std::uint64_t a_size) = 0;

    /// @brief 形状が一致する Texture 全体をコピーする
    [[nodiscard]] virtual Result<void> copy_texture(GpuResourceHandle a_destination,
                                                    GpuResourceHandle a_source) = 0;

    /// @brief 論理 Buffer 内の Heap Slice を指定してコピーする
    [[nodiscard]] virtual Result<void> copy_buffer_region(const GpuBufferCopyRegion& a_region) = 0;

    /// @brief 2D Texture の矩形を整列済み Readback Buffer へコピーする
    [[nodiscard]] virtual Result<void> copy_texture_region_to_buffer(
        const GpuTextureToBufferCopyRegion& a_region) = 0;

    /// @brief Graphics または Compute PSO を現在の Queue へ設定する
    [[nodiscard]] virtual Result<void> bind_pipeline(GpuPipelineHandle a_pipeline) = 0;

    /// @brief PSO の Root Parameter に共通 Shader Heap の View を設定する
    [[nodiscard]] virtual Result<void> bind_view(std::uint32_t a_parameter, GpuViewHandle a_view) = 0;

    /// @brief Root Signature の 32 bit 定数へ値を設定する
    [[nodiscard]] virtual Result<void> set_32bit_constant(std::uint32_t a_parameter,
                                                             std::uint32_t a_value) = 0;

    /// @brief Root Descriptor へ物理 Buffer の GPU Address を設定する
    [[nodiscard]] virtual Result<void> set_root_buffer(std::uint32_t a_parameter,
        GpuResourceHandle a_buffer, GpuRootParameterType a_type, GpuMemory a_memory,
        std::uint32_t a_resourceIndex) = 0;

    /// @brief Shader Heap の先頭から Texture Descriptor Table を設定する
    [[nodiscard]] virtual Result<void> set_graphics_texture_table(std::uint32_t a_parameter) = 0;

    /// @brief Graphics Pass の出力先を設定する
    [[nodiscard]] virtual Result<void> set_render_targets(GpuViewHandle a_color,
                                                          std::optional<GpuViewHandle> a_depth) = 0;

    /// @brief 複数 Color Target と任意 Depth Target を設定する
    [[nodiscard]] virtual Result<void> set_render_targets_many(std::span<const GpuViewHandle> a_colors,
        std::optional<GpuViewHandle> a_depth) = 0;

    /// @brief Color View を指定色で消去する
    [[nodiscard]] virtual Result<void> clear_color(GpuViewHandle a_color,
                                                    const std::array<float, 4>& a_value) = 0;

    /// @brief Depth View を指定値で消去する
    [[nodiscard]] virtual Result<void> clear_depth(GpuViewHandle a_depth, float a_value) = 0;

    /// @brief Depth と Stencil を同時に消去する
    [[nodiscard]] virtual Result<void> clear_depth_stencil(GpuViewHandle a_depth, float a_depthValue,
                                                            std::uint8_t a_stencilValue) = 0;

    /// @brief Shader Visible Heap の UAV を整数値で消去する
    [[nodiscard]] virtual Result<void> clear_unordered_access_uint(GpuViewHandle a_view,
        const std::array<std::uint32_t, 4>& a_values) = 0;

    /// @brief 一つの Vertex Buffer を Input Assembler へ設定する
    [[nodiscard]] virtual Result<void> bind_vertex_buffer(GpuResourceHandle a_buffer, std::uint64_t a_offset,
                                                           std::uint32_t a_stride, std::uint32_t a_size) = 0;

    /// @brief 16 bit Index Buffer を Input Assembler へ設定する
    [[nodiscard]] virtual Result<void> bind_index_buffer(GpuResourceHandle a_buffer, std::uint64_t a_offset,
                                                          std::uint32_t a_size) = 0;

    /// @brief 16 bit または 32 bit Index Buffer を設定する
    [[nodiscard]] virtual Result<void> bind_index_buffer_format(GpuResourceHandle a_buffer,
        std::uint64_t a_offset, std::uint32_t a_size, GpuIndexFormat a_format) = 0;

    /// @brief 一つの Viewport と Scissor を設定する
    [[nodiscard]] virtual Result<void> set_viewport(std::uint32_t a_width, std::uint32_t a_height) = 0;

    /// @brief 左上座標を指定して Viewport と Scissor を設定する
    [[nodiscard]] virtual Result<void> set_viewport_rect(std::uint32_t a_x, std::uint32_t a_y,
                                                           std::uint32_t a_width, std::uint32_t a_height) = 0;

    /// @brief Point、Line、Triangle の Input Assembler Topology を切り替える
    [[nodiscard]] virtual Result<void> set_primitive_topology(GpuPrimitiveTopology a_topology) = 0;

    /// @brief Triangle List の頂点を描画する
    [[nodiscard]] virtual Result<void> draw(std::uint32_t a_vertices, std::uint32_t a_instances = 1) = 0;

    /// @brief 16 bit Index Buffer の要素を描画する
    [[nodiscard]] virtual Result<void> draw_indexed(std::uint32_t a_indices,
                                                     std::uint32_t a_instances = 1) = 0;

    /// @brief Mesh Slice 用の開始位置を含めて描画する
    [[nodiscard]] virtual Result<void> draw_indexed_range(std::uint32_t a_indices,
        std::uint32_t a_instances, std::uint32_t a_startIndex, std::int32_t a_baseVertex,
        std::uint32_t a_startInstance) = 0;

    /// @brief Root Parameter 0 の定数と DrawIndexed を 24 byte 単位で間接実行する
    [[nodiscard]] virtual Result<void> execute_indexed_indirect(GpuResourceHandle a_commands,
        GpuResourceHandle a_count, std::uint32_t a_maxCommandCount) = 0;

    /// @brief Compute Shader の Thread Group を投入する
    [[nodiscard]] virtual Result<void> dispatch(std::uint32_t a_x, std::uint32_t a_y, std::uint32_t a_z) = 0;
};
} // namespace cue
