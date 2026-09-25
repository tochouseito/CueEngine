#pragma once

#include <Cue/Platform/Window.h>
#include <Cue/Runtime/Runtime.h>

#include <memory>
#include <thread>

namespace cue
{
/// @brief WindowとFrameControllerの起動設定をまとめて渡す
struct WindowsHostDesc final
{
    WindowDescriptor window;
    FrameControllerDesc frame;
};

/// @brief WindowsのWindowとServiceを所有し、共通Runtimeを進める
///
/// 構築Threadが初期化、Message Pump、step、停止、破棄を行い、再入しない
/// Callbackの捕捉先はshutdown完了まで呼出側が生存させる
class WindowsHost final
{
public:
    /// @brief WindowとFrame設定、構築Threadを保持する
    explicit WindowsHost(WindowsHostDesc a_desc);

    /// @brief Runtime停止後にWindowとServiceを解放する
    ~WindowsHost();

    WindowsHost(const WindowsHost&) = delete;
    WindowsHost& operator=(const WindowsHost&) = delete;

    /// @brief Windows資源を作り、共通RuntimeへCallbackとServiceを渡す
    ///
    /// 構築Threadから一度だけ呼ぶ。失敗時は部分資源を破棄して停止済みにする
    [[nodiscard]] Result<void> initialize(FrameCallback a_update, FrameCallback a_render);

    /// @brief Window Eventを処理し、継続中ならRuntimeを1回進める
    ///
    /// Close要求を受けた周回ではFrameを進めない。終了時はfalseを返す
    /// 実行中に構築Threadから呼ぶ。失敗時もshutdownを呼ぶ
    [[nodiscard]] Result<bool> step();

    /// @brief 実行中の共通RuntimeのFrame進行状態を取得する
    [[nodiscard]] Result<FrameProgress> progress() const;

    /// @brief RuntimeのWorkerを停止・joinしてからWindowとServiceを解放する
    ///
    /// 構築Threadから複数回呼べる。失敗しても残る解放を続け、最初のErrorを返す
    [[nodiscard]] Result<void> shutdown();

private:
    class State;

    enum class Lifecycle
    {
        Uninitialized,
        Running,
        Stopped,
    };

    WindowsHostDesc m_desc;
    std::thread::id m_ownerId;
    std::unique_ptr<State> m_state;
    Lifecycle m_lifecycle = Lifecycle::Uninitialized;
};
} // namespace cue
