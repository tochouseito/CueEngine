#pragma once

#include <Cue/Foundation/Result.h>
#include <Cue/Platform/Clock.h>
#include <Cue/Platform/Thread.h>
#include <Cue/Platform/Waiter.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>

namespace cue
{
/// @brief Frameの先行数、Worker利用、Render間隔の上限を指定する
struct FrameControllerDesc final
{
    std::uint32_t maxFramesInFlight = 2;
    bool useWorkerThreads = true;
    /// Render完了間隔のFPS上限。0は上限なし
    std::uint32_t maxFps = 60;
};

/// @brief Main Threadが進行状況を参照するためのSnapshot
struct FrameProgress final
{
    std::uint64_t submittedFrames = 0;
    std::uint64_t updatedFrames = 0;
    std::uint64_t renderedFrames = 0;
    std::uint64_t lastUpdateFrame = 0;
    std::uint64_t lastRenderFrame = 0;
    std::thread::id updateThreadId{};
    std::thread::id renderThreadId{};
    std::chrono::nanoseconds lastUpdateDuration{};
    std::chrono::nanoseconds lastRenderDuration{};
    std::chrono::nanoseconds lastFrameInterval{};
};

using FrameCallback = std::function<Result<void>(std::uint64_t, std::stop_token)>;

/// @brief Main、Update、RenderのFrame順序とWorker寿命を管理する
///
/// Runtimeが一意所有し、借用するClock、Waiter、ThreadFactoryより先に破棄する
/// start、advance、stop、破棄は構築Threadでのみ行い、再入しない
/// progressはThread-safe。WorkerはWindowや可変Worldを直接参照しない
class FrameController final
{
public:
    /// @brief 借用Serviceを受け取り、開始前のControllerを作る
    FrameController(FrameControllerDesc a_desc, Clock& a_clock, Waiter& a_waiter, ThreadFactory& a_threadFactory);

    /// @brief Callbackを構築時に渡す既存の入口
    FrameController(FrameControllerDesc a_desc, Clock& a_clock, Waiter& a_waiter, ThreadFactory& a_threadFactory,
                    FrameCallback a_update, FrameCallback a_render);

    /// @brief Workerを停止・joinしてから資源を解放する
    ~FrameController();

    FrameController(const FrameController&) = delete;
    FrameController& operator=(const FrameController&) = delete;

    /// @brief Runtimeから渡されたUpdateとRenderの処理を開始前に一度だけ登録する
    ///
    /// Callbackの借用先はstop完了まで有効に保つ。構築Threadから呼び、失敗時は未登録のままにする
    [[nodiscard]] Result<void> register_callbacks(FrameCallback a_update, FrameCallback a_render);

    /// @brief 設定を検証してWorkerを開始する
    ///
    /// 失敗時に開始済みWorkerを停止・joinし、再試行可能な開始前状態へ戻す
    [[nodiscard]] Result<void> start();

    /// @brief 空きがあればFrameを1件投入し、投入時はtrueを返す
    ///
    /// Worker構成では待機しない。単一Thread構成ではUpdateとRenderを同期実行する
    [[nodiscard]] Result<bool> advance();

    /// @brief Frameを進め、満杯時はWorkerの進行を短時間待つ
    [[nodiscard]] Result<bool> step();

    /// @brief 停止を要求してWorkerをjoinし、失敗を呼出側へ返す
    ///
    /// 複数回呼出可能。失敗後もWorkerは残さない
    [[nodiscard]] Result<void> stop();

    /// @brief 完了数、直近のFrame番号、実行Thread、処理時間を取得する
    [[nodiscard]] FrameProgress progress() const;

private:
    /// @brief Update Workerが投入済みFrameを順番に処理する
    [[nodiscard]] Result<void> update_loop(std::stop_token a_stopToken);

    /// @brief Update完了済みFrameをRender Workerが順番に処理する
    [[nodiscard]] Result<void> render_loop(std::stop_token a_stopToken);

    /// @brief 最初のWorker失敗を保存して他の待機Threadを起こす
    void record_failure(Error a_error);

    /// @brief Render完了の間隔を上限FPSに合わせ、停止時は時刻を返さない
    [[nodiscard]] std::optional<std::chrono::steady_clock::time_point>
    wait_for_render_limit(std::stop_token a_stopToken);

    FrameControllerDesc m_desc;
    Clock& m_clock;
    Waiter& m_waiter;
    ThreadFactory& m_threadFactory;
    FrameCallback m_update;
    FrameCallback m_render;
    std::thread::id m_ownerId;
    mutable std::mutex m_mutex;
    FrameProgress m_progress;
    std::uint64_t m_nextUpdateFrame = 0;
    std::uint64_t m_nextRenderFrame = 0;
    std::chrono::steady_clock::time_point m_nextRenderTime{};
    std::chrono::steady_clock::time_point m_lastRenderCompletion{};
    std::optional<Error> m_failure;
    std::unique_ptr<Thread> m_updateThread;
    std::unique_ptr<Thread> m_renderThread;
    bool m_isStarted = false;
    bool m_hasStarted = false;
    bool m_stopRequested = false;
};
} // namespace cue
