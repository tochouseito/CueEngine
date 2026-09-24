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
struct FrameControllerDesc final
{
    std::uint32_t maxFramesInFlight = 2;
    bool useWorkerThreads = true;
};

struct FrameProgress final
{
    std::uint64_t submittedFrames = 0;
    std::uint64_t updatedFrames = 0;
    std::uint64_t renderedFrames = 0;
    std::chrono::nanoseconds lastUpdateDuration{};
    std::chrono::nanoseconds lastRenderDuration{};
};

using FrameCallback = std::function<Result<void>(std::uint64_t, std::stop_token)>;

/// @brief Main、Update、RenderのFrame順序とWorker寿命を管理する
///
/// Hostが一意所有し、借用するClock、Waiter、ThreadFactoryより先に破棄する
/// start、advance、stop、破棄は構築Threadでのみ行い、再入しない
/// progressはThread-safe。WorkerはWindowや可変Worldを直接参照しない
class FrameController final
{
public:
    /// @brief 借用ServiceとFrame処理を受け取り、開始前のControllerを作る
    FrameController(FrameControllerDesc a_desc, Clock& a_clock, Waiter& a_waiter, ThreadFactory& a_threadFactory,
                    FrameCallback a_update, FrameCallback a_render);

    /// @brief Workerを停止・joinしてから資源を解放する
    ~FrameController();

    FrameController(const FrameController&) = delete;
    FrameController& operator=(const FrameController&) = delete;

    /// @brief 設定を検証してWorkerを開始する
    ///
    /// 失敗時に開始済みWorkerを停止・joinし、再試行可能な開始前状態へ戻す
    [[nodiscard]] Result<void> start();

    /// @brief 空きがあればFrameを1件投入し、投入時はtrueを返す
    ///
    /// Worker構成では待機しない。単一Thread構成ではUpdateとRenderを同期実行する
    [[nodiscard]] Result<bool> advance();

    /// @brief 停止を要求してWorkerをjoinし、失敗を呼出側へ返す
    ///
    /// 複数回呼出可能。失敗後もWorkerは残さない
    [[nodiscard]] Result<void> stop();

    /// @brief 完了したFrame数と直近の処理時間を取得する
    [[nodiscard]] FrameProgress progress() const;

private:
    /// @brief Update Workerが投入済みFrameを順番に処理する
    [[nodiscard]] Result<void> update_loop(std::stop_token a_stopToken);

    /// @brief Update完了済みFrameをRender Workerが順番に処理する
    [[nodiscard]] Result<void> render_loop(std::stop_token a_stopToken);

    /// @brief 最初のWorker失敗を保存して他の待機Threadを起こす
    void record_failure(Error a_error);

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
    std::optional<Error> m_failure;
    std::unique_ptr<Thread> m_updateThread;
    std::unique_ptr<Thread> m_renderThread;
    bool m_isStarted = false;
    bool m_hasStarted = false;
    bool m_stopRequested = false;
};
} // namespace cue
