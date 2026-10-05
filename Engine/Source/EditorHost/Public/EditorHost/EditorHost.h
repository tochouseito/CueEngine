#pragma once

#include <WindowsHost/WindowsHost.h>

namespace cue
{
/// @brief Editor 用の Window、Frame と表示の起動設定を所有する
struct EditorHostConfig final
{
    WindowDescriptor window{"CueEngine Editor", {1280, 720}};
    // UI Context と Window Message を同じ Owner Thread で扱う初期構成
    FrameControllerDesc frame{2, false, 60};
    PresentationConfig presentation;
};

/// @brief Editor の起動入口として Windows の表示・Frame 実行基盤を一意所有する
///
/// 全操作と破棄は構築 Thread で行い、再入しない
/// UI の所有先はこの層へ追加し、WindowsHost と Renderer は Editor に依存しない
class EditorHost final
{
  public:
    /// @brief Editor の起動設定を Windows 実行基盤へ所有値で渡す
    explicit EditorHost(EditorHostConfig a_config = {});

    /// @brief 明示停止がない場合も所有する Windows 実行基盤を停止する
    ~EditorHost() = default;

    EditorHost(const EditorHost &) = delete;
    EditorHost &operator=(const EditorHost &) = delete;

    /// @brief Window、Renderer と Runtime を生成して Editor 用 Window を表示する
    ///
    /// 一度だけ呼べる。失敗時は部分資源を回収し、同じ Host の再初期化は拒否する
    [[nodiscard]] Result<void> initialize();

    /// @brief Window Message と Frame を進め、Close 要求時は false を返す
    ///
    /// 実行中だけ呼べる。失敗時も shutdown を呼び、停止を完了する
    [[nodiscard]] Result<bool> step();

    /// @brief 実行中の CPU Frame 進行状態を所有値で返す
    ///
    /// GPU 完了状態は表さない。起動前と停止後は InvalidState を返す
    [[nodiscard]] Result<FrameProgress> frame_progress() const;

    /// @brief Frame 実行と GPU を停止してから Window を破棄する
    ///
    /// 複数回呼べる。失敗時も残りの解放を続け、Windows 実行基盤の Error を返す
    [[nodiscard]] Result<void> shutdown();

  private:
    WindowsHost m_windows;
};
} // namespace cue
