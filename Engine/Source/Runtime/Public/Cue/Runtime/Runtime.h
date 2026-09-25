#pragma once

#include <Cue/Runtime/FrameController.h>

#include <memory>
#include <thread>

namespace cue
{
/// @brief Platform Serviceを借用し、Frameの実行寿命を所有する
///
/// 構築Threadから初期化、step、停止を行う。借用ServiceとCallbackの捕捉先はshutdown完了まで生存させる
/// WindowやPlatform固有の実装は所有しない。再入は許可しない
class Runtime final
{
public:
    /// @brief Frame設定とPlatform Serviceの借用先を固定する
    Runtime(FrameControllerDesc a_desc, Clock& a_clock, Waiter& a_waiter, ThreadFactory& a_threadFactory);

    /// @brief Workerを停止してから借用先を解放できる状態にする
    ~Runtime();

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    /// @brief Callbackを登録してFrame実行を開始する
    ///
    /// 構築Threadから一度だけ呼ぶ。失敗時は停止済みとなり、再試行には新しいRuntimeを使う
    [[nodiscard]] Result<void> initialize(FrameCallback a_update, FrameCallback a_render);

    /// @brief 次のFrameを進める
    ///
    /// 実行中に構築Threadから呼ぶ。失敗後はshutdownを呼ぶ
    [[nodiscard]] Result<bool> step();

    /// @brief 実行中のFrame進行状態を取得する
    [[nodiscard]] Result<FrameProgress> progress() const;

    /// @brief Workerを停止・joinして最初の失敗を返す
    ///
    /// 構築Threadから複数回呼べる。失敗してもWorkerは残さない
    [[nodiscard]] Result<void> shutdown();

private:
    enum class Lifecycle
    {
        Uninitialized,
        Running,
        Stopped,
    };

    FrameControllerDesc m_desc;
    Clock& m_clock;
    Waiter& m_waiter;
    ThreadFactory& m_threadFactory;
    std::thread::id m_ownerId;
    std::unique_ptr<FrameController> m_controller;
    Lifecycle m_lifecycle = Lifecycle::Uninitialized;
};
} // namespace cue
