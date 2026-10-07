#pragma once

#include <cstdint>
#include <memory>

#include <EditorHost/ImGuiManager.h>

struct ImGuiContext;
struct ImDrawData;

namespace cue
{
/// @brief Manager の Context を借用する Editor 内部の公式 DX12 Backend 接続
///
/// Manager と Native Backend はこの Adapter より長く生存させる
/// Manager の Context Lock 内で直列に操作し、記録だけは固定 RenderThread に許可する
class DX12ImGuiBackend final
{
    struct CreateToken final
    {
    };

  public:
    /// @brief static create の部分生成状態を構築する
    explicit DX12ImGuiBackend(CreateToken) noexcept;
    /// @brief Device、Queue、Format を検証し、専用 Heap と Device Objects を生成する
    [[nodiscard]] static Result<std::unique_ptr<DX12ImGuiBackend>> create(IBackend &a_backend, ImGuiContext &a_context,
                                                                          std::uint32_t a_frameCount,
                                                                          std::uint32_t a_capacity);
    /// @brief GPU 完了を確認できない借用資源を暗黙破棄しない
    ~DX12ImGuiBackend();
    /// @brief 生成済み Device Objects の公式 Frame 開始処理を実行する
    [[nodiscard]] Result<void> new_frame();
    /// @brief MainThread 上で Texture 更新を完了し、Snapshot が固定 ID を参照できる状態にする
    [[nodiscard]] Result<void> prepare(ImDrawData &a_draw);
    /// @brief 必要な VB/IB 枠の GPU 完了後に Texture 更新と描画を記録する
    [[nodiscard]] Result<void> record(ImDrawData &a_draw, FrameGraphContext &a_context);
    /// @brief 同期 Graph Callback の提出済 Fence を VB/IB Ring と Texture の最終利用へ接続する
    /// 失敗 Callback でも呼ぶ。Queue の致命的な提出失敗では Resource を保全して失敗する
    [[nodiscard]] Result<void> finish_submission();
    /// @brief 所有 Heap と公式 Backend の描画枠の概要を返す
    [[nodiscard]] ImGuiRendererInfo info() const noexcept;
    /// @brief 新しい Fence の完了後に公式 Backend と専用 Heap を解放する
    [[nodiscard]] Result<void> shutdown();

  private:
    class State;
    std::unique_ptr<State> m_state;
};
} // namespace cue
