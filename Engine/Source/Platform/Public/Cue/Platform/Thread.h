#pragma once

#include <Cue/Foundation/Result.h>

#include <functional>
#include <memory>
#include <stop_token>
#include <thread>

namespace cue
{
using ThreadRoutine = std::function<Result<void>(std::stop_token)>;

/// @brief Workerの停止要求と完了待ちを所有する
///
/// Factoryの成功結果をFrameControllerが一意所有する。request_stopは任意Threadから呼出可能
/// joinと破棄は起動元Threadで行い、Worker自身から呼ばない。join後にRoutineは実行されない
class Thread
{
public:
    /// @brief Workerを停止・joinしてから資源を解放する
    ///
    /// 破棄後にRoutineや借用先へアクセスしない
    virtual ~Thread() = default;

    /// @brief 協調停止を要求する
    virtual void request_stop() noexcept = 0;

    /// @brief Workerの終了を待ち、Routineの失敗を返す
    ///
    /// 複数回呼出しても同じ結果を返す。呼出中の再入は許可しない
    [[nodiscard]] virtual Result<void> join() = 0;

    /// @brief 診断用のWorker識別子を返す
    [[nodiscard]] virtual std::thread::id id() const noexcept = 0;
};

/// @brief Platform別のWorker生成を公開する
///
/// Hostが所有し、生成済みThreadの破棄後に破棄する。startは起動元Threadから呼び再入しない
/// 失敗時にはWorkerや部分生成物を公開しない
class ThreadFactory
{
public:
    /// @brief Factoryの資源を解放する
    virtual ~ThreadFactory() = default;

    /// @brief Routineを別Threadで開始して一意所有のHandleを返す
    [[nodiscard]] virtual Result<std::unique_ptr<Thread>> start(ThreadRoutine a_routine) = 0;
};
} // namespace cue
