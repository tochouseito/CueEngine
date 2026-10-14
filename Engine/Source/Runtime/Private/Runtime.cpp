#include <Runtime/Runtime.h>

#include <cstdio>
#include <utility>

#include <Foundation/ScopedFlag.h>

namespace cue
{
/// @brief UI 内の変更要求でも Callback を取消さず、周期待機だけ解除する
void Runtime::request_drain()
{
    if (std::this_thread::get_id() == m_ownerId && m_controller)
    {
        m_controller->request_drain();
    }
}

/// @brief GPU の再構成より前に Worker の借用を止める
Result<void> Runtime::pause()
{
    if (std::this_thread::get_id() != m_ownerId)
    {
        return Result<void>::failure({ErrorCategory::WrongThread, "Runtime.pause"});
    }
    if (m_isStepping || m_lifecycle != Lifecycle::Running)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "Runtime.pause"});
    }
    return m_controller->pause();
}

/// @brief 同じ Controller の Callback と Frame ID を継続する
Result<void> Runtime::resume(FrameControllerDesc a_desc, std::optional<SimulationConfig> a_simulation)
{
    if (std::this_thread::get_id() != m_ownerId)
    {
        return Result<void>::failure({ErrorCategory::WrongThread, "Runtime.resume"});
    }
    if (m_isStepping || m_lifecycle != Lifecycle::Running)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "Runtime.resume"});
    }
    auto result = m_controller->resume(a_desc, a_simulation);
    if (result.has_value())
    {
        m_desc = a_desc;
        if (a_simulation)
        {
            m_simulationConfig = *a_simulation;
        }
    }
    return result;
}

/// @brief 借用するPlatform Serviceと構築Threadを記録する
Runtime::Runtime(FrameControllerDesc a_desc, Clock &a_clock, Waiter &a_waiter, ThreadFactory &a_threadFactory,
                 SimulationConfig a_simulation)
    : m_desc(a_desc), m_simulationConfig(a_simulation), m_clock(a_clock), m_waiter(a_waiter),
      m_threadFactory(a_threadFactory), m_ownerId(std::this_thread::get_id())
{
}

/// @brief 明示停止がない場合もWorkerを回収する
Runtime::~Runtime()
{
    // 呼出側が明示停止を忘れても Worker の借用先を生かしたまま回収する
    auto result = shutdown();
    if (!result.has_value())
    {
        std::fprintf(stderr, "Runtime cleanup: %s (nativeCode=%lld)\n",
                     result.try_error()->operation.c_str(),
                     static_cast<long long>(result.try_error()->nativeCode));
    }
}

/// @brief Callback登録とWorker開始をまとめて成功・失敗へ確定する
Result<void> Runtime::initialize(FrameCallback a_update, FrameCallback a_render, FrameCallback a_main,
                                 FramePacingSource a_pacing, simulationCallback a_simulationUpdate,
                                 frameSnapshotCallback a_snapshot)
{
    // 借用 Service と Callback の捕捉先を安全に扱うため、構築 Thread に固定する
    if (std::this_thread::get_id() != m_ownerId)
    {
        return Result<void>::failure({ErrorCategory::WrongThread, "Runtime.initialize"});
    }
    if (m_lifecycle != Lifecycle::Uninitialized)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "Runtime.initialize"});
    }
    m_lifecycle = Lifecycle::Stopped;

    // 必須 Callback が欠けた状態では Controller を作らない
    if (!a_update || !a_render)
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "Runtime.callbacks"});
    }

    // Service は借用し、Controller だけを Runtime が所有する
    m_controller = std::make_unique<FrameController>(m_desc, m_clock, m_waiter, m_threadFactory, m_simulationConfig);
    auto simulation = m_controller->register_simulation_callbacks(std::move(a_simulationUpdate), std::move(a_snapshot));
    if (!simulation.has_value())
    {
        m_controller.reset();
        return simulation;
    }
    auto pacing = m_controller->register_pacing_source(std::move(a_pacing));
    if (!pacing.has_value())
    {
        m_controller.reset();
        return pacing;
    }

    // 登録失敗時は Controller を破棄して停止済みに戻す
    auto registerResult = m_controller->register_callbacks(std::move(a_update), std::move(a_render), std::move(a_main));
    if (!registerResult.has_value())
    {
        m_controller.reset();
        return registerResult;
    }

    // Worker 開始に失敗した場合も Controller の後片付けを先に完了する
    auto startResult = m_controller->start();
    if (!startResult.has_value())
    {
        m_controller.reset();
        return startResult;
    }

    // Callback と Worker の両方が揃ってから Step を許可する
    m_lifecycle = Lifecycle::Running;
    return Result<void>::success();
}

Result<bool> Runtime::wait_for_frame()
{
    if (std::this_thread::get_id() != m_ownerId)
    {
        return Result<bool>::failure({ErrorCategory::WrongThread, "Runtime.wait_for_frame"});
    }
    if (m_lifecycle != Lifecycle::Running || m_isStepping)
    {
        return Result<bool>::failure({ErrorCategory::InvalidState, "Runtime.wait_for_frame"});
    }
    ScopedFlag stepping(m_isStepping);
    return m_controller->wait_for_frame();
}

/// @brief 構築 Thread から FrameController の開始待機と進行を委譲する
Result<bool> Runtime::step()
{
    // Frame 進行は Controller を作った Thread からだけ受け付ける
    if (std::this_thread::get_id() != m_ownerId)
    {
        return Result<bool>::failure({ErrorCategory::WrongThread, "Runtime.step"});
    }
    if (m_lifecycle != Lifecycle::Running || m_isStepping)
    {
        return Result<bool>::failure({ErrorCategory::InvalidState, "Runtime.step"});
    }
    ScopedFlag stepping(m_isStepping);
    return m_controller->step();
}

/// @brief Windowに依存しないFrame統計を返す
Result<FrameProgress> Runtime::progress() const
{
    // Controller 解放後の Snapshot 参照を避ける
    if (std::this_thread::get_id() != m_ownerId)
    {
        return Result<FrameProgress>::failure({ErrorCategory::WrongThread, "Runtime.progress"});
    }
    if (m_lifecycle != Lifecycle::Running)
    {
        return Result<FrameProgress>::failure({ErrorCategory::InvalidState, "Runtime.progress"});
    }
    return Result<FrameProgress>::success(m_controller->progress());
}

/// @brief Controller を破棄できる Owner の操作と統計参照を同じ Thread に固定する
Result<FrameTimingInfo> Runtime::timing_info() const
{
    if (std::this_thread::get_id() != m_ownerId)
    {
        return Result<FrameTimingInfo>::failure({ErrorCategory::WrongThread, "Runtime.timing_info"});
    }
    if (m_lifecycle != Lifecycle::Running)
    {
        return Result<FrameTimingInfo>::failure({ErrorCategory::InvalidState, "Runtime.timing_info"});
    }
    return Result<FrameTimingInfo>::success(m_controller->timing_info());
}

/// @brief Resize などの新規投入停止中も Worker の失敗を伝える
Result<bool> Runtime::is_idle() const
{
    if (std::this_thread::get_id() != m_ownerId)
    {
        return Result<bool>::failure({ErrorCategory::WrongThread, "Runtime.is_idle"});
    }
    if (m_lifecycle != Lifecycle::Running || m_isStepping)
    {
        return Result<bool>::failure({ErrorCategory::InvalidState, "Runtime.is_idle"});
    }
    return m_controller->is_idle();
}

/// @brief Worker停止後にControllerを解放しService借用を終える
Result<void> Runtime::shutdown()
{
    // 借用している Service より先に Controller を停止する
    if (std::this_thread::get_id() != m_ownerId)
    {
        return Result<void>::failure({ErrorCategory::WrongThread, "Runtime.shutdown"});
    }
    if (m_isStepping)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "Runtime.shutdown"});
    }
    m_lifecycle = Lifecycle::Stopped;
    if (!m_controller)
    {
        return Result<void>::success();
    }
    // Stop が失敗しても Controller は残さず、再度の Shutdown を安全にする
    auto stopResult = m_controller->stop();
    m_controller.reset();
    return stopResult;
}
/// @brief Callback 内は Controller の所有が join まで保証される。外部 Worker からは参照しない
Result<FrameSimulationInfo> Runtime::simulation_info(std::uint64_t a_frame) const
{
    return m_controller
               ? m_controller->simulation_info(a_frame)
               : Result<FrameSimulationInfo>::failure({ErrorCategory::InvalidState, "Runtime.simulation_info"});
}
/// @brief CPU 排出後の復帰で、時間の正本を更新 Thread に再設定させる
void Runtime::reset_simulation_sample_time()
{
    if (std::this_thread::get_id() == m_ownerId && m_controller)
    {
        m_controller->reset_simulation_sample_time();
    }
}
} // namespace cue
