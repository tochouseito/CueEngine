#include <Runtime/FrameController.h>

#include <Profiler/TimingTrace.h>

#include <algorithm>
#include <chrono>
#include <exception>
#include <utility>

namespace cue
{
namespace
{
/// @brief Callbackの例外をResultへ変換しWorker境界を越えさせない
Result<void> invoke_callback(FrameCallback& a_callback, std::uint64_t a_frame, std::stop_token a_stopToken,
                             const char* a_operation)
{
    try
    {
        return a_callback(a_frame, a_stopToken);
    }
    catch (...)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, a_operation});
    }
}
} // namespace

/// @brief 借用Serviceを保持する
FrameController::FrameController(FrameControllerDesc a_desc, Clock &a_clock, Waiter &a_waiter,
                                 ThreadFactory &a_threadFactory, SimulationConfig a_simulation, Profiler *a_profiler)
    : m_desc(a_desc), m_clock(a_clock), m_waiter(a_waiter), m_threadFactory(a_threadFactory),
      m_ownerId(std::this_thread::get_id()), m_profiler(a_profiler ? *a_profiler : m_ownedProfiler),
      m_mainTimings(m_profiler.create_metric("Frame.Main")), m_updateTimings(m_profiler.create_metric("Frame.Update")),
      m_renderTimings(m_profiler.create_metric("Frame.Render")),
      m_limitTimings(m_profiler.create_metric("Frame.FpsWait")),
      m_intervalTimings(m_profiler.create_metric("Frame.Interval")),
      m_cycleTimings(m_profiler.create_metric("Frame.CycleInterval")),
      m_inputToRenderStartTimings(m_profiler.create_metric("Frame.AdoptedToRenderStart")),
      m_inputToRenderCompleteTimings(m_profiler.create_metric("Frame.AdoptedToRenderComplete")),
      m_presentationWaitTimings(m_profiler.create_metric("Frame.PresentationWait")), m_simulationConfig(a_simulation),
      m_simulation(a_simulation)
{
}

/// @brief 構築時にCallbackを渡す既存の入口を維持する
FrameController::FrameController(FrameControllerDesc a_desc, Clock &a_clock, Waiter &a_waiter,
                                 ThreadFactory &a_threadFactory, FrameCallback a_update, FrameCallback a_render,
                                 SimulationConfig a_simulation, Profiler *a_profiler)
    : FrameController(a_desc, a_clock, a_waiter, a_threadFactory, a_simulation, a_profiler)
{
    m_update = std::move(a_update);
    m_render = std::move(a_render);
}

/// @brief 停止とjoinを完了してCallbackの借用先を失効させる
FrameController::~FrameController()
{
    [[maybe_unused]] auto stopResult = stop();
}

/// @brief 設定検証後にUpdateとRenderのWorkerを開始する
Result<void> FrameController::start()
{
    // Worker の生成と停止は構築 Thread に固定する
    if (std::this_thread::get_id() != m_ownerId)
    {
        return Result<void>::failure({ErrorCategory::WrongThread, "FrameController.start"});
    }
    if (m_isStarted || m_hasStarted)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "FrameController.start"});
    }
    // Worker を起動する前に固定更新の条件も検証する
    auto simulationValid = validate_simulation_config(m_simulationConfig);
    if (!simulationValid.has_value() || (m_simulationConfig.frequencyHz != 0 && !m_simulationUpdate))
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "FrameController.simulation_config"});
    }
    if (!m_update || !m_render || m_desc.maxFramesInFlight == 0 || m_desc.maxFramesInFlight > 8 ||
        (m_desc.maxUpdateLeadFrames && *m_desc.maxUpdateLeadFrames >= m_desc.maxFramesInFlight) ||
        (m_desc.waitMode != FrameWaitMode::Sleep && m_desc.waitMode != FrameWaitMode::Hybrid) ||
        m_desc.spinDuration < std::chrono::nanoseconds::zero() || m_desc.spinDuration > std::chrono::milliseconds(5) ||
        m_desc.predictionSlack < std::chrono::nanoseconds::zero() ||
        m_desc.predictionSlack > std::chrono::milliseconds(100) ||
        (m_desc.syncMode != FrameSyncMode::FixedPeriod && m_desc.syncMode != FrameSyncMode::PresentationReady &&
         m_desc.syncMode != FrameSyncMode::DisplayPrediction))
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "FrameController.start"});
    }
    // 再開始を許さない一方、開始失敗時の停止要求は毎回初期化する
    {
        std::lock_guard lock(m_mutex);
        m_stopRequested = false;
    }
    // 起動だけ lead + 1 件を許可し、以後は Render の周期完了ごとに 1 件を許可する
    // Worker の有無に応じて、Update が先行できる最大数を決める
    const auto lead = m_desc.useWorkerThreads
                          ? m_desc.maxUpdateLeadFrames.value_or(m_desc.maxFramesInFlight - 1)
                          : 0;
    m_startPermits = m_progress.submittedFrames + lead + 1;
    m_nextUpdateFrame = m_progress.updatedFrames;
    m_nextRenderFrame = m_progress.renderedFrames;
    m_cycleStarted = m_clock.now();
    m_isFirstFrameOfRun = true;
    m_nextFrameTime = m_clock.now();
    m_limitWaitStarted.reset();
    m_singleFpsCompletion.reset();
    m_mainStopSource = std::stop_source{};
    m_cycleStopSource = std::stop_source{};

    if (m_desc.useWorkerThreads)
    {
        // Render Worker より先に Update Worker を起動する
        // Update 用 Worker を起動し、停止 Token を更新 Loop へ渡す
        auto updateResult = m_threadFactory.start([this](std::stop_token a_token) { return update_loop(a_token); });
        if (!updateResult.has_value())
        {
            return Result<void>::failure(*updateResult.try_error());
        }
        m_updateThread = updateResult.take_value();

        // Render 用 Worker を起動し、更新済み Snapshot を順に描画する
        auto renderResult = m_threadFactory.start([this](std::stop_token a_token) { return render_loop(a_token); });
        if (!renderResult.has_value())
        {
            // 片方だけ動かしたまま返さず、Update を停止・join する
            {
                std::lock_guard lock(m_mutex);
                m_stopRequested = true;
            }
            m_updateThread->request_stop();
            m_waiter.notify_all();
            // Render 起動失敗時は、先に起動した Update Worker も回収する
            auto joinResult = m_updateThread->join();
            m_updateThread.reset();
            {
                std::lock_guard lock(m_mutex);
                m_stopRequested = false;
            }
            if (!joinResult.has_value())
            {
                return Result<void>::failure(*joinResult.try_error());
            }
            return Result<void>::failure(*renderResult.try_error());
        }
        m_renderThread = renderResult.take_value();
    }

    // 両 Worker の開始、または単一 Thread 準備が完了してから投入を許可する
    m_isStarted = true;
    m_hasStarted = true;
    return Result<void>::success();
}

/// @brief MainThreadから容量を確認して次のFrameを投入する
Result<bool> FrameController::advance()
{
    auto ready = frame_ready();
    if (!ready.has_value() || !*ready.try_value())
    {
        return ready;
    }
    // 入力採取の基準となる Frame 開始時刻を共通時計で記録する
    const auto frameStart = m_clock.now();
    std::uint64_t frame = 0;
    {
        std::lock_guard lock(m_mutex);
        frame = m_progress.submittedFrames;
    }

    TimingTraceFrame traceFrame(frame);
    // Main の準備が完了するまで投入数を公開せず、Worker に未完成の Frame を渡さない
    // Callback は共有 Lock 外で呼び、進行状態の参照を妨げない。再入による投入・停止は許可しない
    if (m_main)
    {
        m_isExecutingMain = true;
        const auto mainStart = m_clock.now();
        TimingTraceScope traceMain("Frame.Main");
        // 採用した Frame の Main 処理を Update より先に実行する
        auto mainResult =
            invoke_callback(m_main, frame, m_mainStopSource.get_token(), "FrameController.main.exception");
        traceMain.finish();
        m_isExecutingMain = false;
        // Main Callback の実行時間を独立した系列へ記録する
        const auto mainDuration = std::chrono::duration_cast<std::chrono::nanoseconds>(m_clock.now() - mainStart);
        {
            std::lock_guard lock(m_mutex);
            m_progress.mainThreadId = std::this_thread::get_id();
            m_progress.lastMainDuration = mainDuration;
            m_mainTimings.add(mainDuration);
        }
        if (!mainResult.has_value())
        {
            Error error = std::move(*mainResult.try_error());
            record_failure(error);
            std::lock_guard lock(m_mutex);
            return Result<bool>::failure(*m_failure);
        }
    }
    {
        std::lock_guard lock(m_mutex);
        // Main の実行中に先行 Frame が失敗した場合も、後続を Worker に公開しない
        if (m_failure)
        {
            return Result<bool>::failure(*m_failure);
        }
        ++m_progress.submittedFrames;
        m_frameInputTimes[frame % m_frameInputTimes.size()] = frameStart;
        // 再開後の最初も実採用時刻へ合わせる。単一 Thread は表示枠待ちの遅延を旧期限へ押し込まない
        if (m_isFirstFrameOfRun || !m_desc.useWorkerThreads)
        {
            m_cycleStarted = frameStart;
        }
        m_isFirstFrameOfRun = false;
    }
    if (m_desc.useWorkerThreads)
    {
        // Worker が次の Frame を取得できるよう通知する
        m_waiter.notify_all();
        return Result<bool>::success(true);
    }

    // Fallback は同じ Frame の Update 完了後に Render を実行する
    // 単一 Thread 経路でも同じ Simulation と Update 処理を実行する
    const auto updateStart = m_clock.now();
    TimingTraceScope traceUpdate("Frame.Update");
    auto updateResult = perform_update(frame, {});
    traceUpdate.finish();
    const auto updateDuration = std::chrono::duration_cast<std::chrono::nanoseconds>(m_clock.now() - updateStart);
    if (!updateResult.has_value())
    {
        // 同期実行でも最初の失敗を共有状態へ残し、後続の投入を止める
        Error error = std::move(*updateResult.try_error());
        record_failure(error);
        return Result<bool>::failure(std::move(error));
    }
    {
        std::lock_guard lock(m_mutex);
        ++m_progress.updatedFrames;
        m_progress.lastUpdateFrame = frame;
        m_progress.updateThreadId = std::this_thread::get_id();
        m_progress.lastUpdateDuration = updateDuration;
        m_updateTimings.add(updateDuration);
    }

    // Update が確定した後に Render 開始時刻を採取する
    const auto renderStart = m_clock.now();
    {
        std::lock_guard lock(m_mutex);
        m_inputToRenderStartTimings.add(renderStart - frameStart);
    }
    TimingTraceScope traceRender("Frame.Render");
    auto renderResult = invoke_callback(m_render, frame, {}, "FrameController.render.exception");
    traceRender.finish();
    const auto renderDuration = std::chrono::duration_cast<std::chrono::nanoseconds>(m_clock.now() - renderStart);
    if (!renderResult.has_value())
    {
        Error error = std::move(*renderResult.try_error());
        record_failure(error);
        return Result<bool>::failure(std::move(error));
    }
    const auto completion = m_clock.now();
    {
        // Present の戻りを先に記録し、GPU 完了や周期完了と区別する
        std::lock_guard lock(m_mutex);
        if (m_hasRenderCompletionSample)
        {
            m_progress.lastFrameInterval =
                std::chrono::duration_cast<std::chrono::nanoseconds>(completion - m_lastRenderCompletion);
            m_intervalTimings.add(m_progress.lastFrameInterval);
        }
        m_lastRenderCompletion = completion;
        m_hasRenderCompletionSample = true;
        ++m_progress.renderedFrames;
        m_progress.lastRenderFrame = frame;
        m_progress.renderThreadId = std::this_thread::get_id();
        m_progress.lastRenderDuration = renderDuration;
        m_renderTimings.add(renderDuration);
        m_inputToRenderCompleteTimings.add(completion - frameStart);
    }
    // Render Worker のない経路は期限を登録して戻り、Main が Pump を継続する
    // 今回の完了時刻から次の開始期限を計画する
    auto plan = cycle_plan(completion);
    if (!plan.has_value())
    {
        record_failure(*plan.try_error());
        return Result<bool>::failure(*plan.try_error());
    }
    m_singlePlan = *plan.try_value();
    m_nextFrameTime = std::max(completion, m_singlePlan.deadline);
    m_limitWaitStarted = completion;
    m_singleFpsCompletion.reset();
    return Result<bool>::success(true);
}

Result<bool> FrameController::frame_ready()
{
    // 投入数の上限と最初の失敗は、Callback や待機計測を変更する前に確認する
    if (std::this_thread::get_id() != m_ownerId)
    {
        return Result<bool>::failure({ErrorCategory::WrongThread, "FrameController.advance"});
    }
    if (m_isExecutingMain)
    {
        return Result<bool>::failure({ErrorCategory::InvalidState, "FrameController.advance"});
    }
    // Snapshot 容量と Update の先行数の両方に空きがあるか確認する
    bool hasCapacity = false;
    {
        std::lock_guard lock(m_mutex);
        if (m_failure)
        {
            return Result<bool>::failure(*m_failure);
        }
        if (!m_isStarted || m_stopRequested)
        {
            return Result<bool>::failure({ErrorCategory::InvalidState, "FrameController.advance"});
        }
        hasCapacity = m_progress.submittedFrames - m_progress.renderedFrames < m_desc.maxFramesInFlight;
        if (m_desc.useWorkerThreads)
            return Result<bool>::success(hasCapacity && m_progress.submittedFrames < m_startPermits);
    }

    // 単一 Thread でも次の開始期限までは新しい Frame を採用しない
    const auto now = m_clock.now();
    if (m_limitWaitStarted && now < m_nextFrameTime)
    {
        return Result<bool>::success(false);
    }
    if (m_limitWaitStarted)
    {
        if (!m_singleFpsCompletion)
        {
            m_singleFpsCompletion = now;
        }
        if (m_singlePlan.waitsForPresentation && !m_cycleStopSource.stop_requested())
        {
            // 表示枠の取得結果を、未準備と取得失敗に分けて扱う
            Result<bool> ready = Result<bool>::success(false);
            try
            {
                ready = m_pacingSource.waitForPresentation(m_mainStopSource.get_token(), true);
            }
            catch (...)
            {
                ready = Result<bool>::failure({ErrorCategory::PlatformFailure, "FrameController.presentation.exception"});
            }
            if (!ready.has_value())
            {
                record_failure(*ready.try_error());
                return ready;
            }
            if (!*ready.try_value())
            {
                return Result<bool>::success(false);
            }
        }
        std::lock_guard lock(m_mutex);
        const auto waited = std::chrono::duration_cast<std::chrono::nanoseconds>(*m_singleFpsCompletion -
                                                                               *m_limitWaitStarted);
        m_progress.lastLimitWaitDuration = waited;
        m_limitTimings.add(waited);
        m_presentationWaitTimings.add(m_singlePlan.waitsForPresentation ? now - *m_singleFpsCompletion
                                                                       : std::chrono::steady_clock::duration::zero());
        m_progress.lastCycleInterval = std::chrono::duration_cast<std::chrono::nanoseconds>(now - m_cycleStarted);
        m_cycleTimings.add(m_progress.lastCycleInterval);
        ++m_progress.completedCycles;
        m_progress.effectiveSyncMode = m_singlePlan.effectiveMode;
        m_progress.syncFallback = m_singlePlan.fallback;
        m_limitWaitStarted.reset();
        m_singleFpsCompletion.reset();
        m_cycleStarted = now;
    }
    return Result<bool>::success(hasCapacity);
}

/// @brief Native 同期 Callback の借用契約を実行開始前に固定する
Result<void> FrameController::register_pacing_source(FramePacingSource a_source)
{
    if (std::this_thread::get_id() != m_ownerId)
    {
        return Result<void>::failure({ErrorCategory::WrongThread, "FrameController.pacing_source"});
    }
    if (m_hasStarted || m_isStarted)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "FrameController.pacing_source"});
    }
    m_pacingSource = std::move(a_source);
    return Result<void>::success();
}

/// @brief HostのCallbackを開始前に一度だけ登録する
Result<void> FrameController::register_callbacks(FrameCallback a_update, FrameCallback a_render, FrameCallback a_main)
{
    if (std::this_thread::get_id() != m_ownerId)
    {
        return Result<void>::failure({ErrorCategory::WrongThread, "FrameController.register_callbacks"});
    }
    if (m_isStarted || m_hasStarted || m_update || m_render)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "FrameController.register_callbacks"});
    }
    if (!a_update || !a_render)
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "FrameController.register_callbacks"});
    }
    m_update = std::move(a_update);
    m_render = std::move(a_render);
    m_main = std::move(a_main);
    return Result<void>::success();
}

/// @brief MainThreadの一回分のFrame進行と容量待機をまとめる
Result<bool> FrameController::step()
{
    auto ready = wait_for_frame();
    return ready.has_value() && *ready.try_value() ? advance() : std::move(ready);
}

/// @brief 協調停止を通知してWorkerをjoinする
Result<void> FrameController::stop()
{
    // 停止要求を共有状態と各 Worker の Stop Token の両方へ渡す
    if (std::this_thread::get_id() != m_ownerId)
    {
        return Result<void>::failure({ErrorCategory::WrongThread, "FrameController.stop"});
    }
    if (m_isExecutingMain)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "FrameController.stop"});
    }
    // stop_callback は同期実行されるため、Controller の共有 Lock 外で通知する
    m_mainStopSource.request_stop();
    m_cycleStopSource.request_stop();
    {
        std::lock_guard lock(m_mutex);
        m_stopRequested = true;
    }
    if (m_updateThread)
    {
        m_updateThread->request_stop();
    }
    if (m_renderThread)
    {
        m_renderThread->request_stop();
    }
    m_waiter.notify_all();

    // 両 Worker の終了を待ってから Callback の借用先を解放可能にする
    // 停止を通知した両 Worker の終了結果を回収する
    Result<void> updateResult = m_updateThread ? m_updateThread->join() : Result<void>::success();
    Result<void> renderResult = m_renderThread ? m_renderThread->join() : Result<void>::success();
    m_updateThread.reset();
    m_renderThread.reset();
    m_isStarted = false;

    // Callback の失敗を優先し、次に Worker 自体の終了失敗を返す
    std::lock_guard lock(m_mutex);
    if (m_failure)
    {
        return Result<void>::failure(*m_failure);
    }
    if (!updateResult.has_value())
    {
        return Result<void>::failure(*updateResult.try_error());
    }
    if (!renderResult.has_value())
    {
        return Result<void>::failure(*renderResult.try_error());
    }
    return Result<void>::success();
}

/// @brief Thread-safeなFrame進行状態のSnapshotを返す
FrameProgress FrameController::progress() const
{
    std::lock_guard lock(m_mutex);
    return m_progress;
}

/// @brief 更新時に正確な分布を維持し、共有 Lock 内では固定サイズの結果だけを複写する
FrameTimingInfo FrameController::timing_info() const
{
    std::lock_guard lock(m_mutex);
    return {m_mainTimings.statistics(),
            m_updateTimings.statistics(),
            m_renderTimings.statistics(),
            m_limitTimings.statistics(),
            m_intervalTimings.statistics(),
            m_cycleTimings.statistics(),
            m_inputToRenderStartTimings.statistics(),
            m_inputToRenderCompleteTimings.statistics(),
            m_presentationWaitTimings.statistics()};
}

/// @brief Frame 投入を増やさず、非同期 Callback の完了または最初の失敗を返す
Result<bool> FrameController::is_idle() const
{
    std::lock_guard lock(m_mutex);
    // 失敗した Frame は完了数に加わらないため、静止数の比較より先に失敗を確認する
    if (m_failure)
    {
        return Result<bool>::failure(*m_failure);
    }
    if (!m_isStarted || m_stopRequested)
    {
        return Result<bool>::failure({ErrorCategory::InvalidState, "FrameController.is_idle"});
    }
    return Result<bool>::success(m_progress.submittedFrames == m_progress.updatedFrames &&
                                 m_progress.submittedFrames == m_progress.renderedFrames);
}

/// @brief 投入済みFrameをUpdate Workerが順番に処理する
Result<void> FrameController::update_loop(std::stop_token a_stopToken)
{
    while (!a_stopToken.stop_requested())
    {
        // 通知世代と投入済み Frame を確認し、未投入なら停止可能な待機へ入る
        // 通知世代を先に採取し、確認と待機の間の通知を取りこぼさない
        const auto generation = m_waiter.generation();
        std::uint64_t frame = 0;
        bool hasFrame = false;
        {
            std::lock_guard lock(m_mutex);
            if (m_stopRequested)
            {
                break;
            }
            // Main が投入済みで、Update がまだ取得していない Frame だけを選ぶ
            if (m_nextUpdateFrame < m_progress.submittedFrames)
            {
                frame = m_nextUpdateFrame++;
                hasFrame = true;
            }
        }
        // 未投入なら通知世代の変化か停止要求まで待機する
        if (!hasFrame)
        {
            [[maybe_unused]] const auto waitStatus =
                m_waiter.wait_for_change(generation, std::chrono::hours(24), a_stopToken);
            continue;
        }

        // 共有状態の Lock を外してゲームの Update を実行する
        const auto start = m_clock.now();
        TimingTraceFrame traceFrame(frame);
        TimingTraceScope traceUpdate("Frame.Update");
        auto result = perform_update(frame, a_stopToken);
        traceUpdate.finish();
        const auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(m_clock.now() - start);
        if (!result.has_value())
        {
            // Worker の Error を共有し、Main Thread に次回 Step で通知する
            record_failure(std::move(*result.try_error()));
            return Result<void>::failure({ErrorCategory::InvalidState, "FrameController.update"});
        }
        {
            // 停止開始後の Update 完了は新たな Render 対象にしない
            std::lock_guard lock(m_mutex);
            if (m_stopRequested || a_stopToken.stop_requested())
            {
                break;
            }
            // Update の成功結果を Render が取得できる状態として公開する
            ++m_progress.updatedFrames;
            m_progress.lastUpdateFrame = frame;
            m_progress.updateThreadId = std::this_thread::get_id();
            m_progress.lastUpdateDuration = duration;
            m_updateTimings.add(duration);
        }
        // Update 完了を待つ Render と、容量を待つ Main を起こす
        m_waiter.notify_all();
    }
    return Result<void>::success();
}

/// @brief Update完了済みFrameをRender Workerが順番に処理する
Result<void> FrameController::render_loop(std::stop_token a_stopToken)
{
    while (!a_stopToken.stop_requested())
    {
        // Update 完了済みの Frame だけを順番に取得する
        // 更新済み Frame の有無を、待機前の通知世代と組み合わせて確認する
        const auto generation = m_waiter.generation();
        std::uint64_t frame = 0;
        std::chrono::steady_clock::time_point inputSample;
        bool hasFrame = false;
        {
            std::lock_guard lock(m_mutex);
            if (m_stopRequested)
            {
                break;
            }
            if (m_nextRenderFrame < m_progress.updatedFrames)
            {
                frame = m_nextRenderFrame++;
                inputSample = m_frameInputTimes[frame % m_frameInputTimes.size()];
                hasFrame = true;
            }
        }
        if (!hasFrame)
        {
            [[maybe_unused]] const auto waitStatus =
                m_waiter.wait_for_change(generation, std::chrono::hours(24), a_stopToken);
            continue;
        }

        TimingTraceFrame traceFrame(frame);
        // 更新結果を採用した Frame の入力時刻と Render 開始を対応付ける
        const auto start = m_clock.now();
        {
            std::lock_guard lock(m_mutex);
            m_inputToRenderStartTimings.add(start - inputSample);
        }
        TimingTraceScope traceRender("Frame.Render");
        auto result = invoke_callback(m_render, frame, a_stopToken, "FrameController.render.exception");
        traceRender.finish();
        const auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(m_clock.now() - start);
        if (!result.has_value())
        {
            record_failure(std::move(*result.try_error()));
            return Result<void>::failure({ErrorCategory::InvalidState, "FrameController.render"});
        }
        const auto completion = m_clock.now();
        {
            // Render の完了情報は停止要求を再確認してから公開する
            std::lock_guard lock(m_mutex);
            if (m_stopRequested || a_stopToken.stop_requested())
            {
                break;
            }
            // Render 完了時刻の差から CPU の Frame 間隔を求める
            if (m_hasRenderCompletionSample)
            {
                m_progress.lastFrameInterval =
                    std::chrono::duration_cast<std::chrono::nanoseconds>(completion - m_lastRenderCompletion);
                m_intervalTimings.add(m_progress.lastFrameInterval);
            }
            m_lastRenderCompletion = completion;
            m_hasRenderCompletionSample = true;
            // CPU 描画完了数と今回の計測値をまとめて公開する
            ++m_progress.renderedFrames;
            m_progress.lastRenderFrame = frame;
            m_progress.renderThreadId = std::this_thread::get_id();
            m_progress.lastRenderDuration = duration;
            m_renderTimings.add(duration);
            m_inputToRenderCompleteTimings.add(completion - inputSample);
        }
        // 描画完了を容量と周期の待機側へ通知する
        m_waiter.notify_all();
        // FPS 待機は Render 側だけに置き、Main / Update へは許可の通知だけを渡す
        finish_cycle(completion, m_cycleStopSource.get_token());
    }
    return Result<void>::success();
}

/// @brief 最初の失敗を保存して停止へ移る
void FrameController::record_failure(Error a_error)
{
    // 最初の失敗を保持し、以後の Frame 生成を停止する
    {
        std::lock_guard lock(m_mutex);
        if (!m_failure)
        {
            m_failure = std::move(a_error);
        }
        m_stopRequested = true;
    }
    // Main が Texture の旧 Snapshot 回収を待っていても Worker 失敗を伝える
    m_mainStopSource.request_stop();
    m_cycleStopSource.request_stop();
    m_waiter.notify_all();
}

/// @brief 周期の Sleep / Spin を解除し、残った CPU Callback だけを完了させる
void FrameController::request_drain()
{
    if (std::this_thread::get_id() == m_ownerId)
    {
        m_cycleStopSource.request_stop();
        m_waiter.notify_all();
    }
}

/// @brief Idle の確認後だけ停止し、GPU / UI を再構築できる状態にする
Result<void> FrameController::pause()
{
    if (std::this_thread::get_id() != m_ownerId)
    {
        return Result<void>::failure({ErrorCategory::WrongThread, "FrameController.pause"});
    }
    // 既存の Frame がすべて排出された状態でのみ一時停止する
    auto idle = is_idle();
    if (!idle.has_value() || !*idle.try_value() || m_isExecutingMain)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "FrameController.pause"});
    }
    return stop();
}

/// @brief 登録済み Callback と単調 Frame ID を再利用し、古い構成の計測窓を捨てる
Result<void> FrameController::resume(FrameControllerDesc a_desc, std::optional<SimulationConfig> a_simulation)
{
    if (std::this_thread::get_id() != m_ownerId)
    {
        return Result<void>::failure({ErrorCategory::WrongThread, "FrameController.resume"});
    }
    if (m_isStarted || !m_hasStarted || m_isExecutingMain || m_failure)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "FrameController.resume"});
    }
    if (a_simulation)
    {
        // 更新方式も変える場合は、再開前に Simulation の蓄積条件を更新する
        auto configured = m_simulation.configure(*a_simulation);
        if (!configured.has_value())
        {
            return configured;
        }
        m_simulationConfig = *a_simulation;
    }
    else
    {
        m_simulation.reset_sample_time();
    }
    m_desc = a_desc;
    m_hasStarted = false;
    {
        std::lock_guard lock(m_mutex);
        m_mainTimings.reset();
        m_updateTimings.reset();
        m_renderTimings.reset();
        m_limitTimings.reset();
        m_intervalTimings.reset();
        m_hasRenderCompletionSample = false;
        m_progress.lastFrameInterval = {};
        m_progress.lastCycleInterval = {};
        m_progress.lastLimitWaitDuration = {};
        m_cycleTimings.reset();
        m_inputToRenderStartTimings.reset();
        m_inputToRenderCompleteTimings.reset();
        m_presentationWaitTimings.reset();
    }
    return start();
}

Result<bool> FrameController::wait_for_frame()
{
    // 確認前の世代を使い、枠の回収通知を取りこぼさない
    // 開始可能かを調べる前に通知世代を保存する
    const auto generation = m_waiter.generation();
    auto ready = frame_ready();
    if (!ready.has_value() || *ready.try_value())
    {
        return ready;
    }
    const auto token = m_mainStopSource.get_token();
    if (!m_desc.useWorkerThreads)
    {
        if (m_limitWaitStarted && m_clock.now() < m_nextFrameTime)
        {
            wait_for_deadline(m_nextFrameTime, token, true);
        }
        else
        {
            // 表示通知などの再確認も粗い Condition Variable の Timeout へ依存させない
            [[maybe_unused]] const auto status = m_waiter.sleep_for(std::chrono::milliseconds(1), token);
        }
    }
    else
    {
        [[maybe_unused]] const auto status = m_waiter.wait_for_change(generation, std::chrono::milliseconds(1), token);
    }
    return frame_ready();
}

void FrameController::wait_for_deadline(std::chrono::steady_clock::time_point a_deadline, std::stop_token a_token,
                                        bool a_isMain)
{
    const auto now = m_clock.now();
    if (a_token.stop_requested() || now >= a_deadline)
    {
        return;
    }
    // 残り時間を Sleep 部分と短い Spin 部分へ分ける
    const auto remaining = std::chrono::duration_cast<std::chrono::nanoseconds>(a_deadline - now);
    const auto spin = effective_spin_duration(m_desc);
    if (remaining > spin)
    {
        const auto duration = a_isMain
                                  ? (std::min)(remaining - spin, std::chrono::nanoseconds(std::chrono::milliseconds(1)))
                                  : remaining - spin;
        [[maybe_unused]] const auto status = m_waiter.sleep_for(duration, a_token);
    }
    else
    {
        // 大きい Spin 設定でも Main の入力処理を長時間止めない
        // Main Thread の Spin は短く区切り、Window の処理へ戻れるようにする
        const auto until = a_isMain ? (std::min)(a_deadline, now + std::chrono::milliseconds(1)) : a_deadline;
        while (!a_token.stop_requested() && m_clock.now() < until)
        {
            m_waiter.relax();
        }
    }
}

/// @brief Present 後の残り時間を停止可能に待ち、完了と次周期の許可を公開する
void FrameController::finish_cycle(std::chrono::steady_clock::time_point a_renderCompletion,
                                   std::stop_token a_stopToken)
{
    // CPU 完了の計測点を使い、完了公開後の Thread 実行遅延も待機区間へ含める
    const auto started = a_renderCompletion;
    // FPS 上限と表示同期から今回の待機計画を作る
    auto planned = cycle_plan(a_renderCompletion);
    if (!planned.has_value())
    {
        record_failure(*planned.try_error());
        return;
    }
    auto plan = *planned.try_value();
    const auto deadline = plan.deadline;
    // 停止要求を確認しながら周期の期限まで待つ
    while (!a_stopToken.stop_requested() && m_clock.now() < deadline)
    {
        wait_for_deadline(deadline, a_stopToken, false);
    }
    // 時間制限による待機の終了時刻を、表示待機と分離して記録する
    const auto fpsCompletion = m_clock.now();
    if (plan.waitsForPresentation && !a_stopToken.stop_requested())
    {
        Result<bool> ready = Result<bool>::success(false);
        try
        {
            ready = m_pacingSource.waitForPresentation(a_stopToken, false);
        }
        catch (...)
        {
            ready = Result<bool>::failure({ErrorCategory::PlatformFailure, "FrameController.presentation.exception"});
        }
        if (!ready.has_value())
        {
            record_failure(*ready.try_error());
            return;
        }
        if (!*ready.try_value())
        {
            // Resize の取消では周期の許可を失わず、復帰後の Frame 投入を可能にする
            plan.effectiveMode = FrameSyncMode::FixedPeriod;
            plan.fallback = FrameSyncFallback::PresentationUnavailable;
        }
    }
    const auto completion = m_clock.now();
    {
        std::lock_guard lock(m_mutex);
        if (m_stopRequested || a_stopToken.stop_requested())
            return;
        const auto waited = std::chrono::duration_cast<std::chrono::nanoseconds>(fpsCompletion - started);
        m_progress.lastLimitWaitDuration = waited;
        m_limitTimings.add(waited);
        m_presentationWaitTimings.add(completion - fpsCompletion);
        m_progress.lastCycleInterval = std::chrono::duration_cast<std::chrono::nanoseconds>(completion - m_cycleStarted);
        m_cycleTimings.add(m_progress.lastCycleInterval);
        ++m_progress.completedCycles;
        m_progress.effectiveSyncMode = plan.effectiveMode;
        m_progress.syncFallback = plan.fallback;
        ++m_startPermits;
        m_cycleStarted = completion;
    }
    m_waiter.notify_all();
}

/// @brief FPS 期限は Controller が決め、Host は表示候補時刻と準備通知だけを返す
Result<FramePacingPlan> FrameController::cycle_plan(std::chrono::steady_clock::time_point a_completion)
{
    try
    {
        // 無制限 FPS は周期 0 とし、固定周期の待機を追加しない
        const auto period = m_desc.maxFps
                                ? std::chrono::nanoseconds((1'000'000'000ULL + m_desc.maxFps - 1) / m_desc.maxFps)
                                : std::chrono::nanoseconds::zero();
        // Display prediction を要求した場合だけ次の表示候補を取得する
        auto nextDisplay = m_desc.syncMode == FrameSyncMode::DisplayPrediction && m_pacingSource.nextDisplayTime
                               ? m_pacingSource.nextDisplayTime()
                               : std::nullopt;
        return Result<FramePacingPlan>::success(plan_frame_cycle(m_desc.syncMode, m_cycleStarted + period,
                                                                  a_completion, nextDisplay, m_desc.predictionSlack,
                                                                  m_pacingSource.waitForPresentation &&
                                                                  (!m_pacingSource.isPresentationAvailable ||
                                                                   m_pacingSource.isPresentationAvailable())));
    }
    catch (...)
    {
        return Result<FramePacingPlan>::failure({ErrorCategory::PlatformFailure, "FrameController.cycle_plan.exception"});
    }
}
/// @brief 固定更新と描画 Snapshot の Callback を開始前だけ登録する
Result<void> FrameController::register_simulation_callbacks(simulationCallback a_update,
                                                            frameSnapshotCallback a_snapshot)
{
    if (std::this_thread::get_id() != m_ownerId)
    {
        return Result<void>::failure({ErrorCategory::WrongThread, "FrameController.simulation_callbacks"});
    }
    if (m_isStarted || m_hasStarted)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "FrameController.simulation_callbacks"});
    }
    m_simulationUpdate = std::move(a_update);
    m_snapshotUpdate = std::move(a_snapshot);
    return Result<void>::success();
}

/// @brief Update スレッドが可変 World を進めた後にだけ描画用の不変値を抽出する
Result<void> FrameController::perform_update(std::uint64_t a_frame, std::stop_token a_token)
{
    std::chrono::steady_clock::time_point sample;
    {
        std::lock_guard lock(m_mutex);
        sample = m_frameInputTimes[a_frame % m_frameInputTimes.size()];
    }
    if (m_resetsSimulationSample.exchange(false))
    {
        m_simulation.reset_sample_time();
    }
    // 入力採取時刻を基準に Simulation の Tick と補間情報を進める
    auto simulation = m_simulation.advance(a_frame, sample, m_simulationUpdate, a_token);
    if (!simulation.has_value())
    {
        return Result<void>::failure(*simulation.try_error());
    }
    const auto info = simulation.take_value();
    if (info.wasCanceled || a_token.stop_requested())
    {
        return Result<void>::success();
    }
    // Simulation 更新後に Frame 単位の Update Callback を実行する
    auto updated = invoke_callback(m_update, a_frame, a_token, "FrameController.update.exception");
    if (!updated.has_value())
    {
        return updated;
    }
    if (m_snapshotUpdate)
    {
        try
        {
            // 確定済み Tick の組から Render に渡す Snapshot を作る
            auto prepared = m_snapshotUpdate(info, a_token);
            if (!prepared.has_value())
            {
                return prepared;
            }
        }
        catch (...)
        {
            return Result<void>::failure({ErrorCategory::InvalidState, "FrameController.snapshot.exception"});
        }
    }
    {
        std::lock_guard lock(m_mutex);
        m_simulationFrames[a_frame % m_simulationFrames.size()] = info;
        m_progress.simulationTicks = info.currentTick;
        m_progress.droppedSimulationTime += info.droppedElapsed;
    }
    return Result<void>::success();
}

/// @brief Frame ID が一致する確定情報だけ返し、未来 Frame の可変状態を参照しない
Result<FrameSimulationInfo> FrameController::simulation_info(std::uint64_t a_frame) const
{
    std::lock_guard lock(m_mutex);
    const auto &info = m_simulationFrames[a_frame % m_simulationFrames.size()];
    if (!info || info->frameIndex != a_frame)
    {
        return Result<FrameSimulationInfo>::failure({ErrorCategory::InvalidState, "FrameController.simulation_info"});
    }
    return Result<FrameSimulationInfo>::success(*info);
}

/// @brief 更新 Thread 以外から Stepper を直接変更せず、次の Frame に時刻再設定を予約する
void FrameController::reset_simulation_sample_time()
{
    if (std::this_thread::get_id() == m_ownerId)
    {
        m_resetsSimulationSample.store(true);
    }
}
} // namespace cue
