#pragma once

#include <array>
#include <memory>
#include <utility>

#include <FrameGraph/FrameGraph.h>

namespace cue
{
/// @brief Editor が所有する UI の記録を Graphics Pass へ接続する抽象契約
///
/// ImGui と Native API の具体型は実装側へ閉じる。Owner Thread で直列に呼ぶ
class IImGuiRenderer
{
  public:
    /// @brief Pass が所有する実装を抽象型から破棄する
    virtual ~IImGuiRenderer() = default;

    /// @brief 確定済み UI を設定済み RTV へ記録し、失敗は Graph へ伝える
    ///
    /// Context は呼出中だけ借用する。Reset / Close / Submit と Resource 遷移は行わない
    [[nodiscard]] virtual Result<void> record_draw_data(FrameGraphContext &a_context) = 0;
};

/// @brief BackBuffer を Clear し、Host が注入した UI 実装で最後の描画を記録する
///
/// Pass は Adapter を一意所有する。Adapter が借用する UI Owner は Graph 破棄まで生存させる
/// FinalColor の Image 表示を使わない初期構成では BackBuffer のみを宣言する
class ImGuiPass final : public FrameGraphPass
{
  public:
    /// @brief 注入された Adapter の所有権を受け取る
    explicit ImGuiPass(std::unique_ptr<IImGuiRenderer> a_renderer) noexcept : m_renderer(std::move(a_renderer))
    {
    }

    /// @brief Graph と GPU Marker に共通の Pass 名を返す
    [[nodiscard]] const char *name() const noexcept override
    {
        return "ImGui";
    }

    /// @brief BackBuffer と公式描画を同じ Graphics Command へ記録する
    [[nodiscard]] QueueType type() const noexcept override
    {
        return QueueType::Graphics;
    }

    /// @brief BackBuffer の Handle と宣言済み Clear 色を取得し、Adapter の欠落を拒否する
    [[nodiscard]] Result<void> setup(FrameGraphBuilder &a_builder) override
    {
        if (!m_renderer)
        {
            return Result<void>::failure({ErrorCategory::InvalidArgument, "ImGuiPass.renderer"});
        }
        auto buffer = a_builder.get_texture("BackBuffer");
        if (!buffer.has_value())
        {
            return Result<void>::failure(*buffer.try_error());
        }
        auto desc = a_builder.texture_desc(*buffer.try_value());
        if (!desc.has_value())
        {
            return Result<void>::failure(*desc.try_error());
        }
        m_backBuffer = buffer.take_value();
        m_clearColor = desc.try_value()->clearColor;
        return Result<void>::success();
    }

    /// @brief UI 描画と Clear に必要な Write / RenderTarget を Graph へ宣言する
    [[nodiscard]] Result<void> describe_resources(FrameGraphBuilder &a_builder) override
    {
        return a_builder.use(m_backBuffer, FrameGraphAccess::Write, FrameGraphResourceState::RenderTarget);
    }

    /// @brief 宣言済み Target の準備後に UI 記録を委譲し、Present 遷移は Graph に任せる
    [[nodiscard]] Result<void> execute(FrameGraphContext &a_context) override
    {
        if (!m_renderer || !m_backBuffer.is_valid())
        {
            return Result<void>::failure({ErrorCategory::InvalidState, "ImGuiPass.execute"});
        }
        auto clear = a_context.clear_render_target(m_backBuffer, m_clearColor);
        if (!clear.has_value())
        {
            return clear;
        }
        auto target = a_context.set_render_target(m_backBuffer);
        if (!target.has_value())
        {
            return target;
        }
        return m_renderer->record_draw_data(a_context);
    }

  private:
    std::unique_ptr<IImGuiRenderer> m_renderer;
    FrameGraphResourceHandle m_backBuffer;
    std::array<float, 4> m_clearColor{};
};
} // namespace cue
