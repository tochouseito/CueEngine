#pragma once

#include <Cue/Platform/Window.h>
#include <Cue/Runtime/FrameController.h>

#include <cstdint>
#include <memory>
#include <thread>

namespace cue
{
struct RuntimeHostDesc final
{
    WindowDescriptor window;
    FrameControllerDesc frame;
    std::uint64_t testFrameLimit = 0;
};

/// @brief Window、Platform Service、FrameControllerのProcess寿命を所有する
///
/// 構築Threadが初期化、Message Pump、停止、破棄を行い、再入しない
/// Callbackの捕捉先はshutdown完了まで呼出側が生存させる。公開契約はWin32型を含まない
class RuntimeHost final
{
public:
    /// @brief 初期化前のHost設定と構築Threadを保持する
    explicit RuntimeHost(RuntimeHostDesc a_desc);

    /// @brief Worker停止後にWindowとSystemを解放する
    ~RuntimeHost();

    RuntimeHost(const RuntimeHost&) = delete;
    RuntimeHost& operator=(const RuntimeHost&) = delete;

    /// @brief WindowとServiceを作り、Callback登録後にFrameControllerを開始する
    ///
    /// 構築Threadから一度だけ呼ぶ。失敗時は部分資源を破棄して停止済みにし、再試行には新しいHostを使う
    [[nodiscard]] Result<void> initialize(FrameCallback a_update, FrameCallback a_render);

    /// @brief Window EventとTest終了条件を処理し、次のFrameを進める場合にtrueを返す
    ///
    /// 初期化成功後からshutdown前まで構築Threadから呼ぶ。失敗時もshutdownを呼ぶ
    [[nodiscard]] Result<bool> pump_events();

    /// @brief 実行中のControllerを非所有で返す
    ///
    /// 初期化成功後からshutdown前まで構築Threadだけで呼ぶ。返した参照はshutdownで失効する
    [[nodiscard]] FrameController& frame_controller() noexcept;

    /// @brief Workerを停止・joinしてからWindowとSystemを破棄する
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

    RuntimeHostDesc m_desc;
    std::thread::id m_ownerId;
    std::unique_ptr<State> m_state;
    Lifecycle m_lifecycle = Lifecycle::Uninitialized;
};
} // namespace cue
