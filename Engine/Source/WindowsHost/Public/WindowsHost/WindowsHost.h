#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <thread>

#include <Foundation/Result.h>
#include <Passes/MainFrameGraph.h>
#include <Platform/Window.h>
#include <Runtime/FrameController.h>
#include <WindowsHost/StoragePaths.h>

namespace cue
{
class IBackend;

/// @brief SwapChain 構築時に適用する表示設定
struct PresentationConfig final
{
    std::uint32_t bufferCount = 2;
    bool isVSyncEnabled = false;
    bool isTearingAllowed = false;
    std::array<float, 4> clearColor{0.2f, 0.4f, 0.6f, 1.0f};
};

/// @brief Host 固有機能の Window 初期化、Main、Update と停止を抽象契約で接続する
///
/// 初期化、Main と停止は構築 Thread、Update は Frame 設定の実行 Thread から呼ぶ
/// 捕捉先は Host の shutdown 完了まで生存させる。Callback は再入しない
struct WindowsHostCallbacks final
{
    std::function<Result<void>(Window&)> initializeWindow;
    FrameCallback update;
    // Runtime と Graph の停止後、Backend と Window の破棄前に呼ぶ
    // 失敗時は下位 Owner を保持し、次の shutdown で再試行する
    std::function<Result<void>()> shutdownWindow;
    // SwapChain 生成後、Graph 構築前に呼ぶ。具体 Backend 型は上位の実装側で解釈する
    std::function<Result<void>(IBackend &, std::uint32_t)> initializeRenderer;
    // Message Pump 後、採用 Frame の Update より先に構築 Thread で呼ぶ
    // ImGui 等の具体型は捕捉先に閉じ、失敗時は Frame の投入を止める
    FrameCallback main;
    // Graph の記録・提出・取消だけを囲む。Present は排他 / Snapshot の借用終了後に実行する
    std::function<Result<void>(std::uint64_t, std::stop_token, const FrameCallback &)> recordFrame;
};

/// @brief 将来の設定 File 読込から Host へ渡す起動設定
struct WindowsHostConfig final
{
    WindowDescriptor window;
    FrameControllerDesc frame;
    PresentationConfig presentation;
    // Editor 等の具体型を公開せず、追加描画と表示 Pass の所有権を受け取る
    MainFrameGraphConfig graph;
    WindowsHostCallbacks callbacks;
    StoragePathsConfig storage;
    // 未指定なら Windows 実装を生成する。指定された実装も Host が所有する
    std::unique_ptr<IFileSystem> fileSystem;
};

/// @brief Windows の Window、Runtime、Renderer Backend を所有し、終了まで Message を処理する
///
/// 構築 Thread が初期化、Message Pump、停止、破棄を行い、再入しない
class WindowsHost final
{
  public:
    /// @brief Frame と SwapChain 設定を所有値で保持する
    explicit WindowsHost(WindowsHostConfig a_config);

    /// @brief 明示停止がない場合も Window を解放する
    ~WindowsHost();

    WindowsHost(const WindowsHost &) = delete;
    WindowsHost &operator=(const WindowsHost &) = delete;

    /// @brief Window、Renderer Backend、Runtime を作成して表示する
    ///
    /// 構築 Thread から一度だけ呼ぶ。失敗時は部分資源を破棄して停止済みにする
    [[nodiscard]] Result<void> initialize();

    /// @brief 初期化 Callback から停止完了まで FileSystem を非所有で借用する
    /// 構築 Thread だけで取得する。停止後は nullptr を返す
    [[nodiscard]] IFileSystem *file_system() const noexcept;
    /// @brief FileSystem と同じ借用期間の保存先を返す
    [[nodiscard]] const StoragePaths *storage_paths() const noexcept;

    /// @brief Window Event を処理し、継続中なら true を返す
    ///
    /// 構築 Thread から呼ぶ。失敗時も shutdown を呼ぶ
    [[nodiscard]] Result<bool> step();

    /// @brief 実行中の CPU Frame 進行状態を構築 Thread へ返す
    ///
    /// Runtime 停止後は InvalidState を返す。GPU 完了状態は表さない
    [[nodiscard]] Result<FrameProgress> frame_progress() const;

    /// @brief 実行中の CPU Frame 時間と FPS 待機の集計を Owner Thread に返す
    [[nodiscard]] Result<FrameTimingInfo> frame_timing_info() const;

    /// @brief Graph 記録・GPU 完了待機・Present と完了済 GPU Pass の統計を返す
    [[nodiscard]] Result<MainFrameGraphPerformance> graph_performance() const;

    /// @brief Runtime と Backend を停止してから Window を破棄し、Message を回収する
    ///
    /// 構築 Thread から複数回呼べる。最初の Error を返す
    /// 上位の停止 Callback が失敗した場合は Backend / Window を保持し、shutdown を再試行する
    [[nodiscard]] Result<void> shutdown();

  private:
    class State;

    enum class Lifecycle
    {
        Uninitialized,
        Running,
        Stopped,
    };

    WindowsHostConfig m_config;
    std::thread::id m_ownerId;
    std::unique_ptr<State> m_state;
    Lifecycle m_lifecycle = Lifecycle::Uninitialized;
    bool m_isStepping = false;
};
} // namespace cue
