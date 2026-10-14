#pragma once

#include <Runtime/FrameController.h>

#include <memory>
#include <thread>

namespace cue
{
/// @brief Platform Serviceを借用し、Frameの実行寿命を所有する
///
/// 構築 Thread から初期化、wait_for_frame、step、停止を行う
/// 借用 Service と Callback の捕捉先は shutdown 完了まで生存させる
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
    /// 任意の Main Callback は採用 Frame ごとに構築 Thread で Update より先に実行する
    [[nodiscard]] Result<void> initialize(FrameCallback a_update, FrameCallback a_render, FrameCallback a_main = {});

    /// @brief Callback を実行せず開始条件を最大 1 ms の指定時間まで待つ
    ///
    /// 構築 Thread 専用で再入不可。false でも Host は Message を処理して再試行する
    /// true の後に入力を処理して step を呼べる。枠の予約は行わず、失敗後は shutdown を呼ぶ
    [[nodiscard]] Result<bool> wait_for_frame();

    /// @brief 開始条件を短時間待って次の Frame を進め、未準備なら false を返す
    ///
    /// 実行中に構築Threadから呼ぶ。失敗後はshutdownを呼ぶ
    [[nodiscard]] Result<bool> step();

    /// @brief 実行中のFrame進行状態を取得する
    [[nodiscard]] Result<FrameProgress> progress() const;

    /// @brief 実行中の Main / Update / Render と待機時間の分布を返す
    [[nodiscard]] Result<FrameTimingInfo> timing_info() const;

    /// @brief Owner Thread から新規投入せず CPU Frame の完了と非同期失敗を確認する
    [[nodiscard]] Result<bool> is_idle() const;

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
    bool m_isStepping = false;
};
} // namespace cue
