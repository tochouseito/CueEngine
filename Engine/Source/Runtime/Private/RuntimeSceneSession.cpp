#include <Cue/Runtime/RuntimeSceneSession.h>

#include "RuntimeSceneSessionInternals.h"

#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Fatal.h>
#include <Cue/GameCore/RuntimeWorld.h>
#include <Cue/Runtime/Error.h>
#include <Cue/Scene/Error.h>
#include <Cue/Schema/Types.h>

#include <exception>
#include <new>
#include <utility>
#include <vector>

namespace
{
class ProductionSceneEndOperation final : public cue::runtime::details::SceneEndOperation
{
  public:
    /// @brief ProductionではSceneInstanceの所有Entity終了を正確に一度実行する
    [[nodiscard]] cue::Result<cue::scene::SceneInstanceEndReport> end(
        cue::scene::SceneInstance &a_instance, cue::game_core::RuntimeWorld &a_runtimeWorld,
        const cue::AssertContext &a_assertContext) const noexcept override
    {
        return a_instance.end(a_runtimeWorld, a_assertContext);
    }
};

/// @brief Process全体で不変なProduction Scene終了Operationを返す
[[nodiscard]] const cue::runtime::details::SceneEndOperation &production_scene_end_operation() noexcept
{
    static const ProductionSceneEndOperation operation;
    return operation;
}

/// @brief 下位ErrorをRuntime境界へ再分類して元Causeを保持する
[[nodiscard]] cue::Error reclassify_runtime_error(const cue::AssertContext &a_assertContext,
                                                  cue::runtime::RuntimeError a_code, std::string_view a_summary,
                                                  cue::Error &&a_cause) noexcept
{
    cue::ErrorCode code =
        cue::ErrorCode::create(a_assertContext.fatal_handler(), "Cue.Runtime", static_cast<std::int64_t>(a_code));
    return cue::Error::reclassify(a_assertContext.fatal_handler(), std::move(code), a_summary, std::move(a_cause));
}
} // namespace

namespace cue::runtime
{
Result<std::unique_ptr<RuntimeSceneSession>> RuntimeSceneSession::start(
    const scene::SceneSnapshot &a_snapshot, game_core::WorldIdentitySource &a_identitySource,
    const schema::SchemaRegistry &a_schemaRegistry, schema::TypeId a_transformTypeId,
    schema::TypeId a_sceneObjectStateTypeId, const AssertContext &a_assertContext) noexcept
{
    return start_with_operation(a_snapshot, a_identitySource, a_schemaRegistry, std::move(a_transformTypeId),
                                std::move(a_sceneObjectStateTypeId), a_assertContext, production_scene_end_operation());
}

RuntimeSceneSession::RuntimeSceneSession(ConstructionKey, const AssertContext &a_assertContext) noexcept
    : m_assertContext(&a_assertContext), m_ownerThread(std::this_thread::get_id())
{
}

RuntimeSceneSession::~RuntimeSceneSession() noexcept
{
    assert_owner_thread();
    const bool hasLiveOwnership = m_state == RuntimeSceneSessionState::Running ||
                                  m_state == RuntimeSceneSessionState::CleanupFailed || m_runtimeWorld != nullptr ||
                                  m_sceneInstance.has_value();
    CUE_ASSERT(*m_assertContext, !hasLiveOwnership,
               "Cue.Runtime scene session destruction requires completed scene and world cleanup");
    if (hasLiveOwnership)
    {
        m_assertContext->fatal_handler().terminate(
            "Cue.Runtime scene session destruction requires completed scene and world cleanup");
    }
}

Result<void> RuntimeSceneSession::end() noexcept
{
    assert_owner_thread();
    if (m_state == RuntimeSceneSessionState::Stopped)
    {
        return Result<void>::success();
    }
    if (m_state != RuntimeSceneSessionState::Running && m_state != RuntimeSceneSessionState::CleanupFailed)
    {
        return Result<void>::failure(make_runtime_error(*m_assertContext, RuntimeError::InvalidSceneSessionState,
                                                        "Runtime scene session end requires a running session"));
    }

    if (m_sceneInstance.has_value())
    {
        Result<scene::SceneInstanceEndReport> result =
            m_endOperation->end(*m_sceneInstance, *m_runtimeWorld, *m_assertContext);
        if (!result || !result.try_value()->succeeded())
        {
            terminate_scene_cleanup(std::move(result));
        }
        m_sceneInstance.reset();
    }

    const game_core::RuntimeWorldState worldState = m_runtimeWorld->state();
    if (worldState == game_core::RuntimeWorldState::Running)
    {
        Result<void> stop = m_runtimeWorld->request_stop();
        if (!stop)
        {
            m_state = RuntimeSceneSessionState::CleanupFailed;
            return Result<void>::failure(reclassify_runtime_error(
                *m_assertContext, RuntimeError::RuntimeWorldShutdownFailed,
                "Runtime scene session could not request world shutdown", std::move(*stop.try_error())));
        }
    }

    if (m_runtimeWorld->state() == game_core::RuntimeWorldState::Stopping)
    {
        Result<game_core::StructuralCommandReport> tick = m_runtimeWorld->tick();
        if (!tick)
        {
            m_state = RuntimeSceneSessionState::CleanupFailed;
            return Result<void>::failure(reclassify_runtime_error(
                *m_assertContext, RuntimeError::RuntimeWorldShutdownFailed,
                "Runtime scene session final safe point failed", std::move(*tick.try_error())));
        }
    }

    if (m_runtimeWorld->state() != game_core::RuntimeWorldState::Shutdown)
    {
        m_state = RuntimeSceneSessionState::CleanupFailed;
        return Result<void>::failure(make_runtime_error(*m_assertContext, RuntimeError::RuntimeWorldShutdownFailed,
                                                        "Runtime world did not reach shutdown after final safe point"));
    }

    m_runtimeWorld.reset();
    m_state = RuntimeSceneSessionState::Stopped;
    return Result<void>::success();
}

RuntimeSceneSessionState RuntimeSceneSession::state() const noexcept
{
    assert_owner_thread();
    return m_state;
}

std::uint64_t RuntimeSceneSession::world_id() const noexcept
{
    assert_owner_thread();
    return m_worldId;
}

std::size_t RuntimeSceneSession::entity_count() const noexcept
{
    assert_owner_thread();
    return m_sceneInstance.has_value() ? m_sceneInstance->entity_count() : 0U;
}

Result<std::unique_ptr<RuntimeSceneSession>> RuntimeSceneSession::start_with_operation(
    const scene::SceneSnapshot &a_snapshot, game_core::WorldIdentitySource &a_identitySource,
    const schema::SchemaRegistry &a_schemaRegistry, schema::TypeId a_transformTypeId,
    schema::TypeId a_sceneObjectStateTypeId, const AssertContext &a_assertContext,
    const details::SceneEndOperation &a_endOperation) noexcept
{
    try
    {
        std::unique_ptr<RuntimeSceneSession> session =
            std::make_unique<RuntimeSceneSession>(ConstructionKey{}, a_assertContext);
        session->m_endOperation = &a_endOperation;
        session->m_runtimeWorld = game_core::RuntimeWorld::create(a_identitySource, a_schemaRegistry,
                                                                  std::move(a_transformTypeId), a_assertContext);

        Result<void> initialized = session->m_runtimeWorld->initialize();
        if (!initialized)
        {
            return Result<std::unique_ptr<RuntimeSceneSession>>::failure(
                session->finish_failed_start(std::move(*initialized.try_error())));
        }

        game_core::World *world = session->m_runtimeWorld->try_world();
        CUE_ASSERT(a_assertContext, world != nullptr,
                   "Cue.Runtime initialized world must expose its operational ECS world");
        if (world == nullptr)
        {
            a_assertContext.fatal_handler().terminate(
                "Cue.Runtime initialized world must expose its operational ECS world");
        }

        Result<game_core::ComponentType<scene::SceneObjectState>> stateType =
            world->register_component<scene::SceneObjectState>(std::move(a_sceneObjectStateTypeId));
        if (!stateType)
        {
            return Result<std::unique_ptr<RuntimeSceneSession>>::failure(
                session->finish_failed_start(std::move(*stateType.try_error())));
        }

        std::vector<scene::RuntimeComponentBuilder *> builders;
        Result<scene::SceneInstance> instance = scene::SceneInstantiator::instantiate(
            a_snapshot, *session->m_runtimeWorld, *stateType.try_value(), builders, a_assertContext);
        if (!instance)
        {
            return Result<std::unique_ptr<RuntimeSceneSession>>::failure(
                session->finish_failed_start(std::move(*instance.try_error())));
        }

        session->m_worldId = world->id();
        session->m_sceneInstance.emplace(std::move(*instance.try_value()));
        session->m_state = RuntimeSceneSessionState::Running;
        return Result<std::unique_ptr<RuntimeSceneSession>>::success(std::move(session));
    }
    catch (const std::bad_alloc &)
    {
        a_assertContext.fatal_handler().terminate("Cue.Runtime scene session allocation failed");
    }
    catch (...)
    {
        a_assertContext.fatal_handler().terminate("Cue.Runtime scene session caught an unexpected exception");
    }

    std::terminate();
}

game_core::RuntimeWorld &RuntimeSceneSession::runtime_world() noexcept
{
    assert_owner_thread();
    const bool isOperational = m_runtimeWorld != nullptr && (m_state == RuntimeSceneSessionState::Running ||
                                                             m_state == RuntimeSceneSessionState::CleanupFailed);
    CUE_ASSERT(*m_assertContext, isOperational, "Cue.Runtime scene session does not own an operational world");
    if (!isOperational)
    {
        m_assertContext->fatal_handler().terminate("Cue.Runtime scene session does not own an operational world");
    }
    return *m_runtimeWorld;
}

void RuntimeSceneSession::assert_owner_thread() const noexcept
{
    const bool isOwner = std::this_thread::get_id() == m_ownerThread;
    CUE_ASSERT(*m_assertContext, isOwner, "Cue.Runtime scene session API requires its owner thread");
    if (!isOwner)
    {
        m_assertContext->fatal_handler().terminate("Cue.Runtime scene session API requires its owner thread");
    }
}

Error RuntimeSceneSession::finish_failed_start(Error &&a_cause) noexcept
{
    Error primary = reclassify_runtime_error(*m_assertContext, RuntimeError::SceneSessionStartFailed,
                                             "Runtime scene session start failed", std::move(a_cause));
    if (m_runtimeWorld != nullptr)
    {
        Result<void> shutdown = m_runtimeWorld->shutdown();
        if (!shutdown)
        {
            primary.append_secondary_diagnostics(*m_assertContext, *shutdown.try_error(),
                                                 "Runtime world cleanup after scene start failure was incomplete",
                                                 "Runtime world cleanup");
        }
        m_runtimeWorld.reset();
    }
    m_state = RuntimeSceneSessionState::Stopped;
    return primary;
}

[[noreturn]] void RuntimeSceneSession::terminate_scene_cleanup(
    Result<scene::SceneInstanceEndReport> &&a_result) noexcept
{
    Error fatal = !a_result ? reclassify_runtime_error(*m_assertContext, RuntimeError::SceneCleanupFailed,
                                                       "Runtime scene cleanup failed", std::move(*a_result.try_error()))
                            : make_runtime_error(*m_assertContext, RuntimeError::SceneCleanupFailed,
                                                 "Runtime scene cleanup retained live entities");

    if (a_result)
    {
        for (const scene::SceneInstanceEndFailure &failure : a_result.try_value()->failures())
        {
            fatal.append_secondary_diagnostics(*m_assertContext, failure.error(), "Runtime scene entity cleanup failed",
                                               "Scene entity cleanup");
        }
    }

    report_fatal(m_assertContext->logger(), m_assertContext->fatal_handler(),
                 "Runtime Sceneの所有Entityを安全に終了できないためProcessを停止します", std::move(fatal));
}

} // namespace cue::runtime
