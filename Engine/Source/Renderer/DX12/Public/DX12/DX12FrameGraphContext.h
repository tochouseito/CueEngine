#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include <d3d12.h>

#include <DX12/DX12FrameGraphExecutor.h>
#include <FrameGraph/FrameGraph.h>

namespace cue::dx12
{
class DX12FrameGraphFrames;
class DX12PipelineManager;

/// @brief Pass 記録中だけ使う Frame 情報と非所有の依存参照
///
/// 参照先は Pass の execute が戻るまで維持する。Context はこの構造体自体を保持しない
struct DX12FrameGraphRecordContext final
{
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t frameIndex;
    DX12GpuCommandContext &command;
    const DX12FrameGraphPassContext &resources;
    const FrameGraphPlan &plan;
    const FrameGraphPassPlan &pass;
    const DX12FrameGraphFrames &frames;
    DX12PipelineManager &pipelines;
};

/// @brief 論理 Resource 操作を DX12 Command に変換する記録時 Context
///
/// 物理 Resource と Descriptor は Backend が所有し、Pass の execute 中だけ借用する
class DX12FrameGraphContext final : public FrameGraphContext
{
public:
    /// @brief 記録中の List、Pass 宣言、枠ごとの Resource と View を借用する
  explicit DX12FrameGraphContext(const DX12FrameGraphRecordContext &a_record) noexcept;

  /// @brief Pass が RenderTarget と宣言した任意の Texture を Clear する
  [[nodiscard]] Result<void> clear_render_target(FrameGraphResourceHandle a_target,
                                                 const std::array<float, 4> &a_color) override;

  /// @brief Pass の Write 宣言に対応する RTV を描画先に設定する
  [[nodiscard]] Result<void> set_render_target(FrameGraphResourceHandle a_target) override;

  /// @brief Pass の ShaderRead 宣言に対応する SRV を Root Table に設定する
  [[nodiscard]] Result<void> bind_texture2d(FrameGraphResourceHandle a_source, std::uint32_t a_rootParameter) override;

  /// @brief Root と PSO を設定して Command の寿命保持を開始する
  [[nodiscard]] Result<void> set_graphics_pipeline(PipelineStateHandle a_pipeline) override;
  /// @brief Compute Root と PSO を設定する
  [[nodiscard]] Result<void> set_compute_pipeline(PipelineStateHandle a_pipeline) override;
  /// @brief Graph の描画範囲内へ Viewport と Scissor を設定する
  [[nodiscard]] Result<void> set_viewport_scissor(std::uint32_t a_width, std::uint32_t a_height) override;
  /// @brief Graphics の設定と Binding を検証して Draw を記録する
  [[nodiscard]] Result<void> draw_instanced(std::uint32_t a_vertexCount, std::uint32_t a_instanceCount,
                                            std::uint32_t a_firstVertex = 0,
                                            std::uint32_t a_firstInstance = 0) override;
  /// @brief Compute 設定と Group 数を検証して Dispatch を記録する
  [[nodiscard]] Result<void> dispatch(std::uint32_t a_x, std::uint32_t a_y, std::uint32_t a_z) override;

  /// @brief 同一形状の二次元 Texture 間に Copy を記録する
  [[nodiscard]] Result<void> copy_texture2d(FrameGraphResourceHandle a_source,
                                            FrameGraphResourceHandle a_destination) override;

  /// @brief Backend 固有 Pass が必要な場合だけ Native List を借用する
  [[nodiscard]] ID3D12GraphicsCommandList &command_list() const noexcept;

  /// @brief 検証済み RTV へ外部 Graphics 記録を行い、変更された Binding の追跡状態を失効させる
  ///
  /// Callback は記録だけを行い、Reset / Close / Submit / Barrier を実行しない
  /// 成否にかかわらず Pipeline / Root / Heap / Target / Viewport を後続描画前に再設定する
  [[nodiscard]] Result<void> record_external_graphics(
      GpuTextureFormat a_format, const std::function<Result<void>(ID3D12GraphicsCommandList &)> &a_record);

  /// @brief 外部記録前の Texture Upload 等より先に Graphics / Recording / RTV Format を検証する
  [[nodiscard]] Result<void> validate_external_graphics(GpuTextureFormat a_format) const;

  /// @brief Backend 固有 Pass が必要な場合だけ Native Resource を借用する
  [[nodiscard]] ID3D12Resource *resource(FrameGraphResourceHandle a_handle) const noexcept;

private:
    /// @brief 実行中 Pass の宣言と要求された Resource 使用が一致するか判定する
    [[nodiscard]] bool allows(FrameGraphResourceHandle a_handle, FrameGraphAccess a_access,
                              FrameGraphResourceState a_state) const noexcept;

    DX12GpuCommandContext* m_command = nullptr;
    const DX12FrameGraphPassContext* m_resources = nullptr;
    const FrameGraphPlan* m_plan = nullptr;
    const FrameGraphPassPlan* m_pass = nullptr;
    const DX12FrameGraphFrames* m_frames = nullptr;
    DX12PipelineManager *m_pipelines = nullptr;
    PipelineStateHandle m_pipeline;
    std::optional<GpuTextureFormat> m_pipelineFormat;
    std::optional<GpuTextureFormat> m_targetFormat;
    std::vector<std::uint32_t> m_boundParameters;
    bool m_isComputeBound = false;
    bool m_hasViewport = false;
    bool m_isSrvHeapBound = false;
};
} // namespace cue::dx12
