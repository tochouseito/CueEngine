#include <Cue/Runtime/RuntimeApplicationSession.h>

#include <Cue/Foundation/Assert.h>
#include <Cue/GameCore/Clock.h>
#include <Cue/GameCore/CommandBuffer.h>
#include <Cue/GameCore/RuntimeSystemRegistry.h>
#include <Cue/GameCore/RuntimeWorld.h>
#include <Cue/Input/InputEvent.h>
#include <Cue/Input/InputEventQueue.h>
#include <Cue/Input/InputState.h>
#include <Cue/Runtime/Error.h>
#include <Cue/Runtime/RuntimeSceneSession.h>
#include <Cue/Schema/Types.h>

#include <exception>
#include <new>
#include <utility>

namespace cue::runtime
{
Result<std::unique_ptr<RuntimeApplicationSession>> RuntimeApplicationSession::create(
    std::uint64_t a_generation, game_core::MonotonicClock &a_clock, std::int64_t a_maxDeltaNanoseconds,
    const AssertContext &a_assertContext) noexcept
{
    if (a_generation == 0U)
    {
        return Result<std::unique_ptr<RuntimeApplicationSession>>::failure(
            make_runtime_error(a_assertContext, RuntimeError::InvalidApplicationConfiguration,
                               "Runtime application session requires a non-zero generation"));
    }

    Result<game_core::GameClock> clock = game_core::GameClock::create(a_clock, a_maxDeltaNanoseconds, a_assertContext);
    if (!clock)
    {
        return Result<std::unique_ptr<RuntimeApplicationSession>>::failure(std::move(*clock.try_error()));
    }

    try
    {
        std::unique_ptr<RuntimeApplicationSession> session =
            std::make_unique<RuntimeApplicationSession>(ConstructionKey{}, a_generation, a_assertContext);
        session->m_inputEvents = std::make_unique<InputEventQueue>();
        session->m_inputState = std::make_unique<InputState>();
        session->m_clock = std::make_unique<game_core::GameClock>(std::move(*clock.try_value()));
        session->m_systemRegistry = std::make_unique<game_core::RuntimeSystemRegistry>(a_assertContext);
        return Result<std::unique_ptr<RuntimeApplicationSession>>::success(std::move(session));
    }
    catch (const std::bad_alloc &)
    {
        a_assertContext.fatal_handler().terminate("Cue.Runtime application session allocation failed");
    }
    catch (...)
    {
        a_assertContext.fatal_handler().terminate("Cue.Runtime application session caught an unexpected exception");
    }

    std::terminate();
}

RuntimeApplicationSession::RuntimeApplicationSession(ConstructionKey, std::uint64_t a_generation,
                                                     const AssertContext &a_assertContext) noexcept
    : m_assertContext(&a_assertContext), m_ownerThread(std::this_thread::get_id()), m_generation(a_generation)
{
}

RuntimeApplicationSession::~RuntimeApplicationSession() noexcept
{
    assert_owner_thread();
    const bool hasLiveOwnership = m_state == RuntimeApplicationSessionState::Starting ||
                                  m_state == RuntimeApplicationSessionState::RollingBack ||
                                  m_state == RuntimeApplicationSessionState::Running ||
                                  m_state == RuntimeApplicationSessionState::StopRequested ||
                                  m_state == RuntimeApplicationSessionState::Stopping ||
                                  m_state == RuntimeApplicationSessionState::CleanupFailed || m_sceneSession != nullptr;
    CUE_ASSERT(*m_assertContext, !hasLiveOwnership,
               "Cue.Runtime application session destruction requires completed runtime cleanup");
    if (hasLiveOwnership)
    {
        m_assertContext->fatal_handler().terminate(
            "Cue.Runtime application session destruction requires completed runtime cleanup");
    }
}

Result<void> RuntimeApplicationSession::register_system(game_core::RuntimeSystemDescriptor a_descriptor,
                                                        std::unique_ptr<game_core::RuntimeSystem> a_system) noexcept
{
    assert_owner_thread();
    if (m_state != RuntimeApplicationSessionState::Constructed)
    {
        return Result<void>::failure(make_runtime_error(*m_assertContext, RuntimeError::InvalidApplicationSessionState,
                                                        "Runtime systems can register only before session start"));
    }
    return m_systemRegistry->register_system(std::move(a_descriptor), std::move(a_system));
}

Result<void> RuntimeApplicationSession::start(const scene::SceneSnapshot &a_snapshot,
                                              game_core::WorldIdentitySource &a_identitySource,
                                              const schema::SchemaRegistry &a_schemaRegistry,
                                              schema::TypeId a_transformTypeId,
                                              schema::TypeId a_sceneObjectStateTypeId) noexcept
{
    assert_owner_thread();
    if (m_state != RuntimeApplicationSessionState::Constructed)
    {
        return Result<void>::failure(make_runtime_error(*m_assertContext, RuntimeError::InvalidApplicationSessionState,
                                                        "Runtime application session can start only once"));
    }

    m_state = RuntimeApplicationSessionState::Starting;
    Result<void> sealed = m_systemRegistry->seal();
    if (!sealed)
    {
        retain_failure(std::move(*sealed.try_error()), "Runtime system registry seal failed during session start",
                       "Runtime system registry");
        m_state = RuntimeApplicationSessionState::Stopped;
        return make_operation_failure(RuntimeError::ApplicationSessionStartFailed,
                                      "Runtime application session start failed");
    }

    Result<std::unique_ptr<RuntimeSceneSession>> sceneSession =
        RuntimeSceneSession::start(a_snapshot, a_identitySource, a_schemaRegistry, std::move(a_transformTypeId),
                                   std::move(a_sceneObjectStateTypeId), *m_assertContext);
    if (!sceneSession)
    {
        retain_failure(std::move(*sceneSession.try_error()), "Runtime scene start failed", "Runtime scene session");
        m_state = RuntimeApplicationSessionState::Stopped;
        return make_operation_failure(RuntimeError::ApplicationSessionStartFailed,
                                      "Runtime application session start failed");
    }

    m_sceneSession = std::move(*sceneSession.try_value());
    m_worldId = m_sceneSession->world_id();
    Result<void> systemsStarted = m_systemRegistry->start(m_sceneSession->runtime_world());
    if (!systemsStarted)
    {
        retain_failure(std::move(*systemsStarted.try_error()), "Runtime system start failed",
                       "Runtime system registry");
        Result<void> rollback = rollback_start();
        if (!rollback)
        {
            return rollback;
        }
        return make_operation_failure(RuntimeError::ApplicationSessionStartFailed,
                                      "Runtime application session start failed");
    }

    Result<void> clockReset = m_clock->reset();
    if (!clockReset)
    {
        retain_failure(std::move(*clockReset.try_error()), "Runtime clock reset failed", "Runtime clock");
        Result<void> rollback = rollback_start();
        if (!rollback)
        {
            return rollback;
        }
        return make_operation_failure(RuntimeError::ApplicationSessionStartFailed,
                                      "Runtime application session start failed");
    }

    m_state = RuntimeApplicationSessionState::Running;
    return Result<void>::success();
}

Result<void> RuntimeApplicationSession::advance_frame(InputCapture a_capture) noexcept
{
    assert_owner_thread();
    if (m_state != RuntimeApplicationSessionState::Running)
    {
        return Result<void>::failure(make_runtime_error(*m_assertContext, RuntimeError::InvalidApplicationSessionState,
                                                        "Runtime frame requires a running application session"));
    }

    m_inputState->begin_frame(a_capture);
    InputEvent event;
    while (m_inputEvents->try_pop(event))
    {
        m_inputState->apply_event(event);
    }

    Result<game_core::UpdateContext> timing = m_clock->advance_frame();
    if (!timing)
    {
        retain_failure(std::move(*timing.try_error()), "Runtime clock frame failed", "Runtime clock");
        m_stopReason = RuntimeApplicationStopReason::RuntimeFailure;
        m_state = RuntimeApplicationSessionState::StopRequested;
        return make_operation_failure(RuntimeError::ApplicationSessionUpdateFailed, "Runtime application frame failed");
    }

    game_core::RuntimeWorld &runtimeWorld = m_sceneSession->runtime_world();
    Result<void> update = m_systemRegistry->update(runtimeWorld, *timing.try_value(), m_inputState->snapshot());
    if (!update)
    {
        retain_failure(std::move(*update.try_error()), "Runtime system update failed", "Runtime system registry");
        m_stopReason = RuntimeApplicationStopReason::RuntimeFailure;
        m_state = RuntimeApplicationSessionState::StopRequested;
        return make_operation_failure(RuntimeError::ApplicationSessionUpdateFailed, "Runtime application frame failed");
    }

    Result<game_core::StructuralCommandReport> flush = runtimeWorld.tick();
    if (!flush)
    {
        retain_failure(std::move(*flush.try_error()), "Runtime frame safe point failed", "Runtime world");
        m_stopReason = RuntimeApplicationStopReason::RuntimeFailure;
        m_state = RuntimeApplicationSessionState::StopRequested;
        return make_operation_failure(RuntimeError::ApplicationSessionUpdateFailed, "Runtime application frame failed");
    }

    bool hasCommandFailure = false;
    for (const game_core::StructuralCommandResult &result : flush.try_value()->results())
    {
        hasCommandFailure = hasCommandFailure || !result.succeeded();
    }
    if (hasCommandFailure)
    {
        retain_command_failures(*flush.try_value());
        m_stopReason = RuntimeApplicationStopReason::RuntimeFailure;
        m_state = RuntimeApplicationSessionState::StopRequested;
        return make_operation_failure(RuntimeError::ApplicationSessionUpdateFailed, "Runtime application frame failed");
    }

    return Result<void>::success();
}

Result<void> RuntimeApplicationSession::request_stop(RuntimeApplicationStopReason a_reason) noexcept
{
    assert_owner_thread();
    if (a_reason == RuntimeApplicationStopReason::None || a_reason == RuntimeApplicationStopReason::RuntimeFailure)
    {
        return Result<void>::failure(make_runtime_error(*m_assertContext, RuntimeError::InvalidApplicationConfiguration,
                                                        "Runtime host stop requires an explicit host reason"));
    }
    if (m_state == RuntimeApplicationSessionState::StopRequested ||
        m_state == RuntimeApplicationSessionState::CleanupFailed)
    {
        return Result<void>::success();
    }
    if (m_state != RuntimeApplicationSessionState::Running)
    {
        return Result<void>::failure(make_runtime_error(*m_assertContext, RuntimeError::InvalidApplicationSessionState,
                                                        "Runtime stop request requires a running session"));
    }

    m_stopReason = a_reason;
    m_state = RuntimeApplicationSessionState::StopRequested;
    return Result<void>::success();
}

Result<void> RuntimeApplicationSession::stop() noexcept
{
    assert_owner_thread();
    if (m_state == RuntimeApplicationSessionState::Stopped)
    {
        return Result<void>::success();
    }
    if (m_state == RuntimeApplicationSessionState::Constructed)
    {
        m_state = RuntimeApplicationSessionState::Stopped;
        return Result<void>::success();
    }
    if (m_state == RuntimeApplicationSessionState::Running)
    {
        m_stopReason = RuntimeApplicationStopReason::Requested;
        m_state = RuntimeApplicationSessionState::StopRequested;
    }
    if (m_state != RuntimeApplicationSessionState::StopRequested &&
        m_state != RuntimeApplicationSessionState::CleanupFailed)
    {
        return Result<void>::failure(make_runtime_error(*m_assertContext, RuntimeError::InvalidApplicationSessionState,
                                                        "Runtime application cleanup requires a stopped frame loop"));
    }

    m_state = RuntimeApplicationSessionState::Stopping;
    game_core::RuntimeSystemRegistryState registryState = m_systemRegistry->state();
    if (registryState == game_core::RuntimeSystemRegistryState::Started ||
        registryState == game_core::RuntimeSystemRegistryState::StopPending ||
        registryState == game_core::RuntimeSystemRegistryState::Sealed)
    {
        Result<void> systemsStopped = m_systemRegistry->stop(m_sceneSession->runtime_world());
        if (!systemsStopped)
        {
            retain_failure(std::move(*systemsStopped.try_error()), "Runtime system cleanup is incomplete",
                           "Runtime system registry");
            m_state = RuntimeApplicationSessionState::CleanupFailed;
            return make_operation_failure(RuntimeError::ApplicationSessionCleanupFailed,
                                          "Runtime application session cleanup is incomplete");
        }
    }

    Result<void> flushed = flush_system_commands();
    if (!flushed)
    {
        m_state = RuntimeApplicationSessionState::CleanupFailed;
        return make_operation_failure(RuntimeError::ApplicationSessionCleanupFailed,
                                      "Runtime application session cleanup is incomplete");
    }

    Result<void> sceneCleanup = finish_scene_cleanup();
    if (!sceneCleanup)
    {
        return sceneCleanup;
    }
    if (m_hasCleanupCommandFailure)
    {
        return make_operation_failure(RuntimeError::ApplicationSessionCleanupFailed,
                                      "Runtime application session cleanup contained structural command failures");
    }
    return Result<void>::success();
}

InputEventQueue &RuntimeApplicationSession::input_events() noexcept
{
    assert_owner_thread();
    return *m_inputEvents;
}

RuntimeApplicationSessionState RuntimeApplicationSession::state() const noexcept
{
    assert_owner_thread();
    return m_state;
}

std::uint64_t RuntimeApplicationSession::generation() const noexcept
{
    assert_owner_thread();
    return m_generation;
}

std::uint64_t RuntimeApplicationSession::world_id() const noexcept
{
    assert_owner_thread();
    return m_worldId;
}

std::uint64_t RuntimeApplicationSession::next_frame_index() const noexcept
{
    assert_owner_thread();
    return m_clock->next_frame_index();
}

std::size_t RuntimeApplicationSession::system_count() const noexcept
{
    assert_owner_thread();
    return m_systemRegistry->system_count();
}

RuntimeApplicationStopReason RuntimeApplicationSession::stop_reason() const noexcept
{
    assert_owner_thread();
    return m_stopReason;
}

const Error *RuntimeApplicationSession::try_failure() const noexcept
{
    assert_owner_thread();
    return m_failure.has_value() ? &m_failure.value() : nullptr;
}

Result<std::optional<Error>> RuntimeApplicationSession::take_failure() noexcept
{
    assert_owner_thread();
    if (m_state != RuntimeApplicationSessionState::Stopped)
    {
        return Result<std::optional<Error>>::failure(
            make_runtime_error(*m_assertContext, RuntimeError::InvalidApplicationSessionState,
                               "Runtime application failure can move only after all owned runtime state stopped"));
    }

    std::optional<Error> failure = std::move(m_failure);
    m_failure.reset();
    return Result<std::optional<Error>>::success(std::move(failure));
}

void RuntimeApplicationSession::assert_owner_thread() const noexcept
{
    const bool isOwner = std::this_thread::get_id() == m_ownerThread;
    CUE_ASSERT(*m_assertContext, isOwner, "Cue.Runtime application session API requires its owner thread");
    if (!isOwner)
    {
        m_assertContext->fatal_handler().terminate("Cue.Runtime application session API requires its owner thread");
    }
}

void RuntimeApplicationSession::retain_failure(Error &&a_error, const char *a_context, const char *a_label) noexcept
{
    if (!m_failure.has_value())
    {
        m_failure.emplace(std::move(a_error));
        return;
    }
    m_failure->append_secondary_diagnostics(*m_assertContext, a_error, a_context, a_label);
}

void RuntimeApplicationSession::retain_command_failures(const game_core::StructuralCommandReport &a_report) noexcept
{
    bool hasFailure = false;
    for (const game_core::StructuralCommandResult &result : a_report.results())
    {
        hasFailure = hasFailure || !result.succeeded();
    }
    if (!hasFailure)
    {
        return;
    }

    if (!m_failure.has_value())
    {
        m_failure.emplace(make_runtime_error(*m_assertContext, RuntimeError::StructuralCommandFailed,
                                             "Runtime structural command batch contained failures"));
    }
    for (const game_core::StructuralCommandResult &result : a_report.results())
    {
        if (!result.succeeded())
        {
            m_failure->append_secondary_diagnostics(*m_assertContext, *result.try_error(),
                                                    "Runtime structural command failed", "Structural command");
        }
    }
}

Result<void> RuntimeApplicationSession::flush_system_commands() noexcept
{
    if (m_hasFlushedSystemCommands || m_sceneSession == nullptr)
    {
        return Result<void>::success();
    }

    Result<game_core::StructuralCommandReport> flush = m_sceneSession->runtime_world().tick();
    if (!flush)
    {
        retain_failure(std::move(*flush.try_error()), "Runtime system cleanup safe point failed", "Runtime world");
        return make_operation_failure(RuntimeError::ApplicationSessionCleanupFailed,
                                      "Runtime system cleanup safe point failed");
    }

    retain_command_failures(*flush.try_value());
    for (const game_core::StructuralCommandResult &result : flush.try_value()->results())
    {
        m_hasCleanupCommandFailure = m_hasCleanupCommandFailure || !result.succeeded();
    }
    m_hasFlushedSystemCommands = true;
    return Result<void>::success();
}

Result<void> RuntimeApplicationSession::rollback_start() noexcept
{
    m_state = RuntimeApplicationSessionState::RollingBack;
    const game_core::RuntimeSystemRegistryState registryState = m_systemRegistry->state();
    if (registryState == game_core::RuntimeSystemRegistryState::Started ||
        registryState == game_core::RuntimeSystemRegistryState::Sealed)
    {
        Result<void> stopped = m_systemRegistry->stop(m_sceneSession->runtime_world());
        if (!stopped)
        {
            retain_failure(std::move(*stopped.try_error()), "Runtime system rollback is incomplete",
                           "Runtime system registry");
            m_state = RuntimeApplicationSessionState::CleanupFailed;
            return make_operation_failure(RuntimeError::ApplicationSessionCleanupFailed,
                                          "Runtime application start rollback is incomplete");
        }
    }
    else if (registryState == game_core::RuntimeSystemRegistryState::StopPending)
    {
        m_state = RuntimeApplicationSessionState::CleanupFailed;
        return make_operation_failure(RuntimeError::ApplicationSessionCleanupFailed,
                                      "Runtime application start rollback is incomplete");
    }

    Result<void> flushed = flush_system_commands();
    if (!flushed)
    {
        m_state = RuntimeApplicationSessionState::CleanupFailed;
        return flushed;
    }
    return finish_scene_cleanup();
}

Result<void> RuntimeApplicationSession::finish_scene_cleanup() noexcept
{
    if (m_sceneSession != nullptr)
    {
        Result<void> sceneEnded = m_sceneSession->end();
        if (!sceneEnded)
        {
            retain_failure(std::move(*sceneEnded.try_error()), "Runtime scene and world cleanup is incomplete",
                           "Runtime scene session");
            m_state = RuntimeApplicationSessionState::CleanupFailed;
            return make_operation_failure(RuntimeError::ApplicationSessionCleanupFailed,
                                          "Runtime application scene cleanup is incomplete");
        }
        m_sceneSession.reset();
    }

    m_state = RuntimeApplicationSessionState::Stopped;
    return Result<void>::success();
}

Result<void> RuntimeApplicationSession::make_operation_failure(RuntimeError a_code,
                                                               const char *a_summary) const noexcept
{
    return Result<void>::failure(make_runtime_error(*m_assertContext, a_code, a_summary));
}
} // namespace cue::runtime
