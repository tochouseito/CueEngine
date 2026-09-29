#pragma once

#include <Cue/Renderer/RHI/GpuExecution.h>
#include <Cue/Renderer/RHI/GpuPipelines.h>

#include <array>
#include <cstdint>
#include <optional>

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
    VertexIndex
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

    /// @brief Graphics または Compute PSO を現在の Queue へ設定する
    [[nodiscard]] virtual Result<void> bind_pipeline(GpuPipelineHandle a_pipeline) = 0;

    /// @brief PSO の Root Parameter に共通 Shader Heap の View を設定する
    [[nodiscard]] virtual Result<void> bind_view(std::uint32_t a_parameter, GpuViewHandle a_view) = 0;

    /// @brief Graphics Pass の出力先を設定する
    [[nodiscard]] virtual Result<void> set_render_targets(GpuViewHandle a_color,
                                                          std::optional<GpuViewHandle> a_depth) = 0;

    /// @brief Color View を指定色で消去する
    [[nodiscard]] virtual Result<void> clear_color(GpuViewHandle a_color,
                                                    const std::array<float, 4>& a_value) = 0;

    /// @brief Depth View を指定値で消去する
    [[nodiscard]] virtual Result<void> clear_depth(GpuViewHandle a_depth, float a_value) = 0;

    /// @brief 一つの Vertex Buffer を Input Assembler へ設定する
    [[nodiscard]] virtual Result<void> bind_vertex_buffer(GpuResourceHandle a_buffer, std::uint64_t a_offset,
                                                           std::uint32_t a_stride, std::uint32_t a_size) = 0;

    /// @brief 16 bit Index Buffer を Input Assembler へ設定する
    [[nodiscard]] virtual Result<void> bind_index_buffer(GpuResourceHandle a_buffer, std::uint64_t a_offset,
                                                          std::uint32_t a_size) = 0;

    /// @brief 一つの Viewport と Scissor を設定する
    [[nodiscard]] virtual Result<void> set_viewport(std::uint32_t a_width, std::uint32_t a_height) = 0;

    /// @brief Triangle List の頂点を描画する
    [[nodiscard]] virtual Result<void> draw(std::uint32_t a_vertices, std::uint32_t a_instances = 1) = 0;

    /// @brief 16 bit Index Buffer の要素を描画する
    [[nodiscard]] virtual Result<void> draw_indexed(std::uint32_t a_indices,
                                                     std::uint32_t a_instances = 1) = 0;

    /// @brief Compute Shader の Thread Group を投入する
    [[nodiscard]] virtual Result<void> dispatch(std::uint32_t a_x, std::uint32_t a_y, std::uint32_t a_z) = 0;
};
} // namespace cue
