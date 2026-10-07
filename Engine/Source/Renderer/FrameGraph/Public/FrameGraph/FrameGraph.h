#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include <Foundation/Result.h>
#include <FrameGraph/FrameGraphBuilder.h>
#include <FrameGraph/FrameGraphPerformance.h>
#include <RHI/Command.h>

namespace cue
{
/// @brief Pass の記録中だけ Command、Frame 情報、Backend 操作を借用する
///
/// Resource Handle は同じ Graph の Plan に属するものを渡す。非対応の操作は Result で失敗する
class FrameGraphContext
{
  public:
    /// @brief Backend 固有 Context を基底 Pointer から破棄する
    virtual ~FrameGraphContext() = default;

    FrameGraphContext(const FrameGraphContext &) = delete;
    FrameGraphContext &operator=(const FrameGraphContext &) = delete;

    /// @brief Graph 構築時の幅を返す
    [[nodiscard]] std::uint32_t width() const noexcept;

    /// @brief Graph 構築時の高さを返す
    [[nodiscard]] std::uint32_t height() const noexcept;

    /// @brief 描画枠の Index を返す
    [[nodiscard]] std::uint32_t frame_index() const noexcept;

    /// @brief 記録中の Command Lease を非所有で返す
    [[nodiscard]] ICommandContext &command_context() const noexcept;

    /// @brief 宣言済み RenderTarget の Clear を Backend に記録する
    ///
    /// 対応する RTV がない場合や宣言した最適化 Clear 色と異なる場合は失敗し、Command を記録しない
    [[nodiscard]] virtual Result<void> clear_render_target(FrameGraphResourceHandle a_target,
                                                           const std::array<float, 4> &a_color) = 0;

    /// @brief 宣言済み RenderTarget を現在の Graphics Pass の描画先に設定する
    [[nodiscard]] virtual Result<void> set_render_target(FrameGraphResourceHandle a_target) = 0;

    /// @brief ShaderRead と宣言した Texture の SRV を Graphics Root Table に設定する
    ///
    /// Root Signature と Pipeline は呼出 Pass が先に設定する
    [[nodiscard]] virtual Result<void> bind_texture2d(FrameGraphResourceHandle a_source,
                                                      std::uint32_t a_rootParameter) = 0;

    /// @brief Build で生成した Graphics Pipeline と対応 Root を設定する
    [[nodiscard]] virtual Result<void> set_graphics_pipeline(PipelineStateHandle a_pipeline);
    /// @brief Build で生成した Compute Pipeline と対応 Root を設定する
    [[nodiscard]] virtual Result<void> set_compute_pipeline(PipelineStateHandle a_pipeline);
    /// @brief 描画範囲を Graph の幅と高さの中へ設定する
    [[nodiscard]] virtual Result<void> set_viewport_scissor(std::uint32_t a_width, std::uint32_t a_height);
    /// @brief 現在の Graphics Pipeline、描画先と Binding を使って Draw を記録する
    [[nodiscard]] virtual Result<void> draw_instanced(std::uint32_t a_vertexCount, std::uint32_t a_instanceCount,
                                                      std::uint32_t a_firstVertex = 0,
                                                      std::uint32_t a_firstInstance = 0);
    /// @brief 現在の Compute Pipeline を指定 Group 数で実行する
    [[nodiscard]] virtual Result<void> dispatch(std::uint32_t a_x, std::uint32_t a_y, std::uint32_t a_z);

    /// @brief 同一形状の宣言済み Texture 間の Copy を Backend に記録する
    ///
    /// 異なる形状や未対応の Queue では失敗し、Command を記録しない
    [[nodiscard]] virtual Result<void> copy_texture2d(FrameGraphResourceHandle a_source,
                                                      FrameGraphResourceHandle a_destination) = 0;

  protected:
    /// @brief Graph が保持する値と借用中の Command を紐付ける
    FrameGraphContext(std::uint32_t a_width, std::uint32_t a_height, std::uint32_t a_frameIndex,
                      ICommandContext &a_command) noexcept;

  private:
    std::uint32_t m_width = 0;
    std::uint32_t m_height = 0;
    std::uint32_t m_frameIndex = 0;
    ICommandContext *m_command = nullptr;
};

/// @brief Legacy と同じ構築、Resource 宣言、記録の段階を持つ Pass 契約
///
/// Pass は Graph が一意所有する。各段階は同一 Thread で直列に呼ばれる
class FrameGraphPass
{
  public:
    /// @brief 派生 Pass を基底 Pointer から破棄する
    virtual ~FrameGraphPass() = default;

    FrameGraphPass(const FrameGraphPass &) = delete;
    FrameGraphPass &operator=(const FrameGraphPass &) = delete;

    /// @brief 診断用の名前を返す
    [[nodiscard]] virtual const char *name() const noexcept = 0;

    /// @brief 記録先 Queue の種類を返す
    [[nodiscard]] virtual QueueType type() const noexcept = 0;

    /// @brief Build 時点で Pass が有効か返す。有効状態は実行前にも再検証する
    [[nodiscard]] virtual bool is_enabled() const noexcept;

    /// @brief Resource と Pipeline の構築時宣言を行う
    [[nodiscard]] virtual Result<void> setup(FrameGraphBuilder &a_builder) = 0;

    /// @brief Pass の Resource State と Access を宣言する
    [[nodiscard]] virtual Result<void> describe_resources(FrameGraphBuilder &a_builder) = 0;

    /// @brief Backend Context に Command を記録する
    [[nodiscard]] virtual Result<void> execute(FrameGraphContext &a_context) = 0;

  protected:
    FrameGraphPass() = default;
};

/// @brief Backend の記録と GPU 完了点の登録を Graph 実行へ接続する
///
/// Backend が所有し、execute の間は生存させる。記録と完了登録は順番に呼ぶ
class IFrameGraphRecorder
{
  public:
    virtual ~IFrameGraphRecorder() = default;

    /// @brief 計測を提供する Backend の同期済み所有 Snapshot を返す
    [[nodiscard]] virtual MainFrameGraphPerformance performance() const
    {
        return {};
    }

    /// @brief 借用した Command Context に Graph を記録する
    [[nodiscard]] virtual Result<void> record(std::uint32_t a_frameIndex, ICommandContext &a_command) = 0;

    /// @brief 提出した Command の完了点を資源の Owner へ登録する
    [[nodiscard]] virtual Result<void> mark_submitted(std::uint32_t a_frameIndex,
                                                      std::shared_ptr<ICommandCompletion> a_completion) = 0;

    /// @brief GPU 提出前に破棄した記録が保持する借用を返す
    virtual void discard_unsubmitted(std::uint32_t a_frameIndex) noexcept = 0;

  protected:
    IFrameGraphRecorder() = default;
};

/// @brief Pass を所有し、宣言順と Resource Hazard から実行 Plan を作る
///
/// GPU 資源と Queue は Backend が所有する。Build 後の Pass と Plan は Graph 停止まで有効
class FrameGraph final
{
    struct CreateToken final
    {
    };

  public:
    /// @brief Builder と描画寸法を受け取り、Pass 未登録の Graph を作る
    [[nodiscard]] static Result<std::unique_ptr<FrameGraph>> create(std::unique_ptr<FrameGraphBuilder> a_builder,
                                                                    std::uint32_t a_width, std::uint32_t a_height);

    /// @brief create の内部だけで Graph を構築する
    FrameGraph(CreateToken, std::unique_ptr<FrameGraphBuilder> a_builder, std::uint32_t a_width,
               std::uint32_t a_height) noexcept;

    FrameGraph(const FrameGraph &) = delete;
    FrameGraph &operator=(const FrameGraph &) = delete;

    /// @brief Build 前の Pass を追加し、所有権を Graph へ移す
    [[nodiscard]] Result<void> add_pass(std::unique_ptr<FrameGraphPass> a_pass);

    /// @brief setup、宣言、依存検証を行い Plan を確定する。失敗時は Graph を破棄して再生成する
    [[nodiscard]] Result<void> build();

    /// @brief Plan に含まれる Pass が実行可能なままか検証する
    [[nodiscard]] Result<void> validate_enabled() const;

    /// @brief Pool から Command を借り、記録、提出、完了点の登録を行う
    ///
    /// 現行 DX12 Executor は Graphics Queue のみ記録する。提出直前の取消時は空の成功値を返す
    [[nodiscard]] Result<std::shared_ptr<ICommandCompletion>> execute(std::uint32_t a_frameIndex,
                                                                      ICommandPool &a_commandPool,
                                                                      IQueueContext &a_queue,
                                                                      IFrameGraphRecorder &a_recorder,
                                                                      std::function<bool()> a_shouldCancel = {});

    /// @brief Build 済み Plan を Graph 生存中だけ借用する
    [[nodiscard]] const FrameGraphPlan *plan() const noexcept;

    /// @brief Plan の元 Handle から所有 Pass を借用する
    [[nodiscard]] FrameGraphPass *pass(FrameGraphPassHandle a_handle) const noexcept;

    /// @brief 記録時の Context が使う幅を返す
    [[nodiscard]] std::uint32_t width() const noexcept;

    /// @brief 記録時の Context が使う高さを返す
    [[nodiscard]] std::uint32_t height() const noexcept;

  private:
    /// @brief Pass の宣言と Plan 確定を行い、生成失敗は呼出側の Build 境界で回収する
    [[nodiscard]] Result<void> build_passes();

    std::unique_ptr<FrameGraphBuilder> m_builder;
    std::vector<std::unique_ptr<FrameGraphPass>> m_passes;
    std::optional<FrameGraphPlan> m_plan;
    bool m_buildAttempted = false;
    std::uint32_t m_width = 0;
    std::uint32_t m_height = 0;
};
} // namespace cue
