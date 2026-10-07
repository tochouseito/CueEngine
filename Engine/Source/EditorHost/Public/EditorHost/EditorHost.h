#pragma once

#include <cstdint>
#include <memory>
#include <stop_token>
#include <thread>

#include <EditorHost/ImGuiManager.h>
#include <WindowsHost/WindowsHost.h>

namespace cue
{
/// @brief Editor 用の Window、Frame と表示の起動設定を所有する
struct EditorHostConfig final
{
    WindowDescriptor window{"CueEngine Editor", {1280, 720}};
    // UI 構築は MainThread、Update / Render は Worker で並列に進める
    FrameControllerDesc frame{2, true, 60};
    PresentationConfig presentation;
    // Editor が生成した表示 Pass は抽象型で Windows 実行基盤へ渡す
    MainFrameGraphConfig graph;
    ImGuiManagerConfig imgui;
    // Callback は ImGui Context が Current の Owner Thread 上で実行する。未指定なら Test / TEST と FrameController FPS を表示する
    editorUiCallback buildUi;
};

/// @brief Editor の起動入口として Windows の表示・Frame 実行基盤を一意所有する
///
/// 全操作と破棄は構築 Thread で行い、再入しない
/// ImGuiManager を所有し、WindowsHost と Renderer は Editor に依存しない
class EditorHost final
{
  public:
    /// @brief Editor の起動設定を Windows 実行基盤へ所有値で渡す
    explicit EditorHost(EditorHostConfig a_config = {});

    /// @brief 明示停止がない場合も所有する Windows 実行基盤を停止する
    ~EditorHost();

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

    /// @brief 確定済み UI Frame の描画数と入力 Capture を Owner Thread へ返す
    [[nodiscard]] Result<ImGuiFrameInfo> ui_frame_info() const;

    /// @brief CPU 描画 Snapshot の受渡し数を Owner Thread へ返す
    [[nodiscard]] Result<ImGuiTransferInfo> ui_transfer_info() const;

    /// @brief Frame 実行と GPU を停止してから Window を破棄する
    ///
    /// 複数回呼べる。UI 停止失敗では Window を保持し、次の呼出しで回収を再試行する
    [[nodiscard]] Result<void> shutdown();

  private:
    /// @brief Window の生存中に Context と Win32 入力接続を所有する
    [[nodiscard]] Result<void> initialize_ui(Window &a_window);

    /// @brief 採用 Frame の MainThread 段階で UI を構築し、Update 前に描画 Data を確定する
    [[nodiscard]] Result<void> build_ui(std::uint64_t a_frameIndex, std::stop_token a_stopToken);

    /// @brief Pass 破棄後に Handler を解除し、Window 破棄前に Context を解放する
    [[nodiscard]] Result<void> shutdown_ui();

    /// @brief 表示 Pass 未指定時に Manager を借用する ImGuiPass を生成する
    [[nodiscard]] MainFrameGraphConfig prepare_graph(MainFrameGraphConfig a_config);

    ImGuiManagerConfig m_imguiConfig;
    editorUiCallback m_buildUi;
    std::thread::id m_ownerId;
    bool m_isStepping = false;
    // WindowsHost を先に破棄し、Callback と Pass の参照先を最後まで生存させる
    std::unique_ptr<ImGuiManager> m_imgui;
    WindowsHost m_windows;
};
} // namespace cue
