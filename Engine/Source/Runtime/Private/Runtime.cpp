#include <Cue/Runtime/Runtime.h>

#include <cstdio>
#include <utility>

namespace cue
{
/// @brief 借用するPlatform Serviceと構築Threadを記録する
Runtime::Runtime(FrameControllerDesc a_desc, Clock& a_clock, Waiter& a_waiter, ThreadFactory& a_threadFactory)
    : m_desc(a_desc), m_clock(a_clock), m_waiter(a_waiter), m_threadFactory(a_threadFactory),
      m_ownerId(std::this_thread::get_id())
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
Result<void> Runtime::initialize(FrameCallback a_update, FrameCallback a_render)
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
    m_controller = std::make_unique<FrameController>(m_desc, m_clock, m_waiter, m_threadFactory);

    // 登録失敗時は Controller を破棄して停止済みに戻す
    auto registerResult = m_controller->register_callbacks(std::move(a_update), std::move(a_render));
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

/// @brief 構築ThreadからFrameControllerの進行を委譲する
Result<bool> Runtime::step()
{
    // Frame 進行は Controller を作った Thread からだけ受け付ける
    if (std::this_thread::get_id() != m_ownerId)
    {
        return Result<bool>::failure({ErrorCategory::WrongThread, "Runtime.step"});
    }
    if (m_lifecycle != Lifecycle::Running)
    {
        return Result<bool>::failure({ErrorCategory::InvalidState, "Runtime.step"});
    }
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

/// @brief Worker停止後にControllerを解放しService借用を終える
Result<void> Runtime::shutdown()
{
    // 借用している Service より先に Controller を停止する
    if (std::this_thread::get_id() != m_ownerId)
    {
        return Result<void>::failure({ErrorCategory::WrongThread, "Runtime.shutdown"});
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
} // namespace cue
