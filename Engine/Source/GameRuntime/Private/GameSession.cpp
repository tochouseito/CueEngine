#include <GameRuntime/GameSession.h>

#include <utility>

#include <Foundation/ScopedFlag.h>
#include <Runtime/World.h>
#include <Scene/Scene.h>
#include <SceneRuntime/SceneInstantiation.h>

namespace cue
{
GameSession::GameSession(CreateToken, std::unique_ptr<World> a_world, SceneInstanceHandle a_instance,
                         ScriptSystem *a_scripts) noexcept
    : m_owner(std::this_thread::get_id()), m_world(std::move(a_world)), m_instance(a_instance), m_scripts(a_scripts)
{
}

GameSession::~GameSession()
{
    try
    {
        auto stopped = shutdown();
        (void)stopped;
    }
    catch (...)
    {
        // 明示 shutdown が診断を返す。破棄時の診断値確保失敗でも World の RAII は維持する
    }
}

Result<std::unique_ptr<GameSession>> GameSession::create(const Scene &a_scene,
                                                         std::unique_ptr<RuntimeGameModule> a_module,
                                                         scriptDiagnosticCallback a_report)
{
    using SessionResult = Result<std::unique_ptr<GameSession>>;
    try
    {
        auto created = World::create();
        if (!created.has_value())
            return SessionResult::failure(*created.try_error());
        auto world = created.take_value();
        auto instance = instantiate_scene(a_scene, *world);
        if (!instance.has_value())
            return SessionResult::failure(*instance.try_error());
        auto attached = attach_scene_scripts(a_scene, *world, *instance.try_value());
        if (!attached.has_value())
            return SessionResult::failure(*attached.try_error());
        auto createdSystem = ScriptSystem::create(std::move(a_module), std::move(a_report));
        if (!createdSystem.has_value())
            return SessionResult::failure(*createdSystem.try_error());
        auto *scripts = createdSystem.try_value()->get();
        auto added = world->add_system(createdSystem.take_value());
        if (!added.has_value())
            return SessionResult::failure(*added.try_error());
        auto initialized = world->initialize();
        if (!initialized.has_value())
            return SessionResult::failure(*initialized.try_error());
        return SessionResult::success(
            std::make_unique<GameSession>(CreateToken{}, std::move(world), *instance.try_value(), scripts));
    }
    catch (...)
    {
        return SessionResult::failure({ErrorCategory::Fatal, "GameSession.create.exception"});
    }
}

Result<void> GameSession::check_running() const
{
    if (std::this_thread::get_id() != m_owner)
        return Result<void>::failure({ErrorCategory::WrongThread, "GameSession.owner_thread"});
    if (m_isBusy || m_isStopped || !m_world)
        return Result<void>::failure({ErrorCategory::InvalidState, "GameSession.lifecycle"});
    if (m_failure)
        return Result<void>::failure(*m_failure);
    return Result<void>::success();
}

Result<void> GameSession::update(std::chrono::nanoseconds a_deltaTime)
{
    auto access = check_running();
    if (!access.has_value())
        return access;
    ScopedFlag busy(m_isBusy);
    auto updated = m_world->update(a_deltaTime);
    if (!updated.has_value())
    {
        m_failure = *updated.try_error();
        return updated;
    }
    ++m_completedTicks;
    return Result<void>::success();
}

Result<SceneRenderSnapshot> GameSession::snapshot()
{
    auto access = check_running();
    if (!access.has_value())
        return Result<SceneRenderSnapshot>::failure(*access.try_error());
    ScopedFlag busy(m_isBusy);
    auto snapshot = extract_render_scene(*m_world);
    if (!snapshot.has_value())
        m_failure = *snapshot.try_error();
    return snapshot;
}

Result<void> GameSession::set_property(std::string_view a_objectId, std::string_view a_instanceId,
                                       const ScriptPropertyValue &a_value)
{
    auto access = check_running();
    if (!access.has_value())
        return access;
    ScopedFlag busy(m_isBusy);
    auto entity = m_world->find_by_id(m_instance, a_objectId);
    if (!entity.has_value())
        return Result<void>::failure(*entity.try_error());
    auto changed = m_scripts->set_property(*m_world, *entity.try_value(), a_instanceId, a_value);
    if (!changed.has_value())
        m_failure = *changed.try_error();
    return changed;
}

Result<void> GameSession::shutdown()
{
    if (std::this_thread::get_id() != m_owner)
        return Result<void>::failure({ErrorCategory::WrongThread, "GameSession.owner_thread"});
    if (m_isBusy)
        return Result<void>::failure({ErrorCategory::InvalidState, "GameSession.shutdown.reentry"});
    if (m_isStopped)
        return m_failure ? Result<void>::failure(*m_failure) : Result<void>::success();
    ScopedFlag busy(m_isBusy);
    auto stopped = m_world->shutdown();
    if (!stopped.has_value() && !m_failure)
        m_failure = *stopped.try_error();
    m_isStopped = true;
    m_scripts = nullptr;
    m_world.reset();
    return m_failure ? Result<void>::failure(*m_failure) : Result<void>::success();
}

std::uint64_t GameSession::completed_ticks() const noexcept
{
    return m_completedTicks;
}
World *GameSession::world() noexcept
{
    return std::this_thread::get_id() == m_owner && !m_isStopped && !m_isBusy && !m_failure ? m_world.get() : nullptr;
}
SceneInstanceHandle GameSession::instance() const noexcept
{
    return m_instance;
}
} // namespace cue
