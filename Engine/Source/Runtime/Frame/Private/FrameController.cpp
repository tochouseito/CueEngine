#include <Cue/Runtime/FrameController.h>

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
    catch (const std::exception&)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, a_operation});
    }
    catch (...)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, a_operation});
    }
}
} // namespace

/// @brief 借用ServiceとFrame処理を保持する
FrameController::FrameController(FrameControllerDesc a_desc, Clock& a_clock, Waiter& a_waiter,
                                 ThreadFactory& a_threadFactory, FrameCallback a_update, FrameCallback a_render)
    : m_desc(a_desc), m_clock(a_clock), m_waiter(a_waiter), m_threadFactory(a_threadFactory),
      m_update(std::move(a_update)), m_render(std::move(a_render)), m_ownerId(std::this_thread::get_id())
{
}

/// @brief 停止とjoinを完了してCallbackの借用先を失効させる
FrameController::~FrameController()
{
    [[maybe_unused]] auto stopResult = stop();
}

/// @brief 設定検証後にUpdateとRenderのWorkerを開始する
Result<void> FrameController::start()
{
    if (std::this_thread::get_id() != m_ownerId)
    {
        return Result<void>::failure({ErrorCategory::WrongThread, "FrameController.start"});
    }
    if (m_isStarted || m_hasStarted)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "FrameController.start"});
    }
    if (!m_update || !m_render || m_desc.maxFramesInFlight == 0 || m_desc.maxFramesInFlight > 2)
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "FrameController.start"});
    }
    {
        std::lock_guard lock(m_mutex);
        m_stopRequested = false;
    }

    if (m_desc.useWorkerThreads)
    {
        auto updateResult = m_threadFactory.start([this](std::stop_token a_token) { return update_loop(a_token); });
        if (!updateResult.has_value())
        {
            return Result<void>::failure(*updateResult.try_error());
        }
        m_updateThread = updateResult.take_value();

        auto renderResult = m_threadFactory.start([this](std::stop_token a_token) { return render_loop(a_token); });
        if (!renderResult.has_value())
        {
            {
                std::lock_guard lock(m_mutex);
                m_stopRequested = true;
            }
            m_updateThread->request_stop();
            m_waiter.notify_all();
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

    m_isStarted = true;
    m_hasStarted = true;
    return Result<void>::success();
}

/// @brief MainThreadから容量を確認して次のFrameを投入する
Result<bool> FrameController::advance()
{
    if (std::this_thread::get_id() != m_ownerId)
    {
        return Result<bool>::failure({ErrorCategory::WrongThread, "FrameController.advance"});
    }
    std::uint64_t frame = 0;
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
        if (m_progress.submittedFrames - m_progress.renderedFrames >= m_desc.maxFramesInFlight)
        {
            return Result<bool>::success(false);
        }
        frame = m_progress.submittedFrames++;
    }

    if (m_desc.useWorkerThreads)
    {
        m_waiter.notify_all();
        return Result<bool>::success(true);
    }

    // Fallbackは同じFrameのUpdate完了後にRenderを実行する
    const auto updateStart = m_clock.now();
    auto updateResult = invoke_callback(m_update, frame, {}, "FrameController.update.exception");
    const auto updateDuration = std::chrono::duration_cast<std::chrono::nanoseconds>(m_clock.now() - updateStart);
    if (!updateResult.has_value())
    {
        Error error = std::move(*updateResult.try_error());
        record_failure(error);
        return Result<bool>::failure(std::move(error));
    }
    {
        std::lock_guard lock(m_mutex);
        ++m_progress.updatedFrames;
        m_progress.lastUpdateDuration = updateDuration;
    }

    const auto renderStart = m_clock.now();
    auto renderResult = invoke_callback(m_render, frame, {}, "FrameController.render.exception");
    const auto renderDuration = std::chrono::duration_cast<std::chrono::nanoseconds>(m_clock.now() - renderStart);
    if (!renderResult.has_value())
    {
        Error error = std::move(*renderResult.try_error());
        record_failure(error);
        return Result<bool>::failure(std::move(error));
    }
    {
        std::lock_guard lock(m_mutex);
        ++m_progress.renderedFrames;
        m_progress.lastRenderDuration = renderDuration;
    }
    return Result<bool>::success(true);
}

/// @brief 協調停止を通知してWorkerをjoinする
Result<void> FrameController::stop()
{
    if (std::this_thread::get_id() != m_ownerId)
    {
        return Result<void>::failure({ErrorCategory::WrongThread, "FrameController.stop"});
    }
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

    Result<void> updateResult = m_updateThread ? m_updateThread->join() : Result<void>::success();
    Result<void> renderResult = m_renderThread ? m_renderThread->join() : Result<void>::success();
    m_updateThread.reset();
    m_renderThread.reset();
    m_isStarted = false;

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

/// @brief 投入済みFrameをUpdate Workerが順番に処理する
Result<void> FrameController::update_loop(std::stop_token a_stopToken)
{
    while (!a_stopToken.stop_requested())
    {
        const auto generation = m_waiter.generation();
        std::uint64_t frame = 0;
        bool hasFrame = false;
        {
            std::lock_guard lock(m_mutex);
            if (m_stopRequested)
            {
                break;
            }
            if (m_nextUpdateFrame < m_progress.submittedFrames)
            {
                frame = m_nextUpdateFrame++;
                hasFrame = true;
            }
        }
        if (!hasFrame)
        {
            [[maybe_unused]] const auto waitStatus =
                m_waiter.wait_for_change(generation, std::chrono::hours(24), a_stopToken);
            continue;
        }

        const auto start = m_clock.now();
        auto result = invoke_callback(m_update, frame, a_stopToken, "FrameController.update.exception");
        const auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(m_clock.now() - start);
        if (!result.has_value())
        {
            record_failure(std::move(*result.try_error()));
            return Result<void>::failure({ErrorCategory::InvalidState, "FrameController.update"});
        }
        {
            std::lock_guard lock(m_mutex);
            if (m_stopRequested || a_stopToken.stop_requested())
            {
                break;
            }
            ++m_progress.updatedFrames;
            m_progress.lastUpdateDuration = duration;
        }
        m_waiter.notify_all();
    }
    return Result<void>::success();
}

/// @brief Update完了済みFrameをRender Workerが順番に処理する
Result<void> FrameController::render_loop(std::stop_token a_stopToken)
{
    while (!a_stopToken.stop_requested())
    {
        const auto generation = m_waiter.generation();
        std::uint64_t frame = 0;
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
                hasFrame = true;
            }
        }
        if (!hasFrame)
        {
            [[maybe_unused]] const auto waitStatus =
                m_waiter.wait_for_change(generation, std::chrono::hours(24), a_stopToken);
            continue;
        }

        const auto start = m_clock.now();
        auto result = invoke_callback(m_render, frame, a_stopToken, "FrameController.render.exception");
        const auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(m_clock.now() - start);
        if (!result.has_value())
        {
            record_failure(std::move(*result.try_error()));
            return Result<void>::failure({ErrorCategory::InvalidState, "FrameController.render"});
        }
        {
            std::lock_guard lock(m_mutex);
            if (m_stopRequested || a_stopToken.stop_requested())
            {
                break;
            }
            ++m_progress.renderedFrames;
            m_progress.lastRenderDuration = duration;
        }
        m_waiter.notify_all();
    }
    return Result<void>::success();
}

/// @brief 最初の失敗を保存して停止へ移る
void FrameController::record_failure(Error a_error)
{
    {
        std::lock_guard lock(m_mutex);
        if (!m_failure)
        {
            m_failure = std::move(a_error);
        }
        m_stopRequested = true;
    }
    m_waiter.notify_all();
}
} // namespace cue
