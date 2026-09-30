#pragma once

#include <memory>
#include <thread>

#include <Platform/Window.h>

namespace cue
{
/// @brief Windows の Window と Renderer Backend を所有し、終了まで Message を処理する
///
/// 構築 Thread が初期化、Message Pump、停止、破棄を行い、再入しない
class WindowsHost final
{
public:
    /// @brief Window 設定と構築 Thread を保持する
    explicit WindowsHost(WindowDescriptor a_descriptor);

    /// @brief 明示停止がない場合も Window を解放する
    ~WindowsHost();

    WindowsHost(const WindowsHost&) = delete;
    WindowsHost& operator=(const WindowsHost&) = delete;

    /// @brief Window と Renderer Backend を作成して表示する
    ///
    /// 構築 Thread から一度だけ呼ぶ。失敗時は部分資源を破棄して停止済みにする
    [[nodiscard]] Result<void> initialize();

    /// @brief Window Event を処理し、継続中なら true を返す
    ///
    /// 構築 Thread から呼ぶ。失敗時も shutdown を呼ぶ
    [[nodiscard]] Result<bool> step();

    /// @brief Backend を停止してから Window を破棄し、Message を回収する
    ///
    /// 構築 Thread から複数回呼べる。失敗しても残る解放を続け、最初の Error を返す
    [[nodiscard]] Result<void> shutdown();

private:
    class State;

    enum class Lifecycle
    {
        Uninitialized,
        Running,
        Stopped,
    };

    WindowDescriptor m_descriptor;
    std::thread::id m_ownerId;
    std::unique_ptr<State> m_state;
    Lifecycle m_lifecycle = Lifecycle::Uninitialized;
};
} // namespace cue
