#include <Cue/GameCore/RuntimeWorld.h>

#include <Cue/Foundation/Fatal.h>
#include <Cue/GameCore/Error.h>

#include <exception>
#include <new>
#include <utility>

namespace cue::game_core
{
std::unique_ptr<RuntimeWorld> RuntimeWorld::create(WorldIdentitySource &a_identitySource,
                                                   const schema::SchemaRegistry &a_schemaRegistry,
                                                   schema::TypeId a_transformTypeId,
                                                   const AssertContext &a_assertContext) noexcept
{
    try
    {
        return std::make_unique<RuntimeWorld>(ConstructionKey{}, a_identitySource, a_schemaRegistry,
                                              std::move(a_transformTypeId), a_assertContext);
    }
    catch (const std::bad_alloc &)
    {
        a_assertContext.fatal_handler().terminate("Cue.GameCore runtime world allocation failed");
    }
    catch (...)
    {
        a_assertContext.fatal_handler().terminate("Cue.GameCore runtime world construction failed");
    }

    std::terminate();
}

RuntimeWorld::RuntimeWorld(ConstructionKey, WorldIdentitySource &a_identitySource,
                           const schema::SchemaRegistry &a_schemaRegistry, schema::TypeId a_transformTypeId,
                           const AssertContext &a_assertContext) noexcept
    : m_identitySource(&a_identitySource), m_schemaRegistry(&a_schemaRegistry),
      m_transformTypeId(std::move(a_transformTypeId)), m_assertContext(&a_assertContext),
      m_ownerThread(std::this_thread::get_id())
{
}

RuntimeWorld::~RuntimeWorld() noexcept
{
    assert_owner_thread();
    CUE_ASSERT(*m_assertContext, !m_isSystemCallbackActive,
               "Cue.GameCore runtime world destruction is forbidden during a system callback");
    if (m_isSystemCallbackActive)
    {
        m_assertContext->fatal_handler().terminate(
            "Cue.GameCore runtime world destruction is forbidden during a system callback");
    }
    release_owned_state();
    m_state = RuntimeWorldState::Shutdown;
}

Result<void> RuntimeWorld::initialize() noexcept
{
    assert_owner_thread();
    Result<void> lifecycleValidation = validate_lifecycle_mutation();
    if (!lifecycleValidation)
    {
        return lifecycleValidation;
    }

    if (m_state != RuntimeWorldState::Initializing)
    {
        return Result<void>::failure(make_state_error("Runtime world can only initialize once"));
    }

    auto worldResult = World::create(*m_identitySource, *m_schemaRegistry, *m_assertContext);

    if (!worldResult)
    {
        m_state = RuntimeWorldState::Failed;
        return Result<void>::failure(std::move(*worldResult.try_error()));
    }

    m_world = std::move(*worldResult.try_value());
    auto transformResult = m_world->register_component<math::Transform>(m_transformTypeId);

    if (!transformResult)
    {
        auto error = std::move(*transformResult.try_error());
        release_owned_state();
        m_state = RuntimeWorldState::Failed;
        return Result<void>::failure(std::move(error));
    }

    m_transformType.emplace(std::move(*transformResult.try_value()));

    try
    {
        m_commandBuffer = std::make_unique<StructuralCommandBuffer>(*m_world);
    }
    catch (const std::bad_alloc &)
    {
        terminate_allocation();
    }
    catch (...)
    {
        terminate_exception();
    }

    m_state = RuntimeWorldState::Running;
    return Result<void>::success();
}

Result<StructuralCommandReport> RuntimeWorld::tick() noexcept
{
    assert_owner_thread();
    Result<void> lifecycleValidation = validate_lifecycle_mutation();
    if (!lifecycleValidation)
    {
        return Result<StructuralCommandReport>::failure(std::move(*lifecycleValidation.try_error()));
    }

    if (!is_operational())
    {
        return Result<StructuralCommandReport>::failure(
            make_state_error("Runtime world tick requires Running or Stopping"));
    }

    auto report = m_world->flush_commands(*m_commandBuffer);

    if (m_state == RuntimeWorldState::Stopping)
    {
        auto shutdownResult = shutdown();

        if (!shutdownResult)
        {
            return Result<StructuralCommandReport>::failure(std::move(*shutdownResult.try_error()));
        }
    }

    return Result<StructuralCommandReport>::success(std::move(report));
}

Result<void> RuntimeWorld::request_stop() noexcept
{
    assert_owner_thread();
    Result<void> lifecycleValidation = validate_lifecycle_mutation();
    if (!lifecycleValidation)
    {
        return lifecycleValidation;
    }

    if (m_state == RuntimeWorldState::Stopping || m_state == RuntimeWorldState::Shutdown)
    {
        return Result<void>::success();
    }

    if (m_state != RuntimeWorldState::Running)
    {
        return Result<void>::failure(make_state_error("Runtime world stop requires Running"));
    }

    m_state = RuntimeWorldState::Stopping;
    return Result<void>::success();
}

Result<void> RuntimeWorld::shutdown() noexcept
{
    assert_owner_thread();
    Result<void> lifecycleValidation = validate_lifecycle_mutation();
    if (!lifecycleValidation)
    {
        return lifecycleValidation;
    }

    if (m_state == RuntimeWorldState::Shutdown)
    {
        return Result<void>::success();
    }

    release_owned_state();
    m_state = RuntimeWorldState::Shutdown;
    return Result<void>::success();
}

RuntimeWorldState RuntimeWorld::state() const noexcept
{
    assert_owner_thread();
    return m_state;
}

World *RuntimeWorld::try_world() noexcept
{
    assert_owner_thread();
    return is_operational() ? m_world.get() : nullptr;
}

const World *RuntimeWorld::try_world() const noexcept
{
    assert_owner_thread();
    return is_operational() ? m_world.get() : nullptr;
}

StructuralCommandBuffer *RuntimeWorld::try_command_buffer() noexcept
{
    assert_owner_thread();
    return is_operational() ? m_commandBuffer.get() : nullptr;
}

const ComponentType<math::Transform> *RuntimeWorld::try_transform_type() const noexcept
{
    assert_owner_thread();
    return is_operational() && m_transformType.has_value() ? &m_transformType.value() : nullptr;
}

bool RuntimeWorld::is_operational() const noexcept
{
    return m_state == RuntimeWorldState::Running || m_state == RuntimeWorldState::Stopping;
}

void RuntimeWorld::begin_system_callback_lease() noexcept
{
    assert_owner_thread();
    CUE_ASSERT(*m_assertContext, !m_isSystemCallbackActive,
               "Cue.GameCore runtime world system callback lease must not be nested");
    if (m_isSystemCallbackActive)
    {
        m_assertContext->fatal_handler().terminate(
            "Cue.GameCore runtime world system callback lease must not be nested");
    }
    m_isSystemCallbackActive = true;
}

void RuntimeWorld::end_system_callback_lease() noexcept
{
    assert_owner_thread();
    CUE_ASSERT(*m_assertContext, m_isSystemCallbackActive,
               "Cue.GameCore runtime world system callback lease is not active");
    if (!m_isSystemCallbackActive)
    {
        m_assertContext->fatal_handler().terminate("Cue.GameCore runtime world system callback lease is not active");
    }
    m_isSystemCallbackActive = false;
}

Result<void> RuntimeWorld::validate_lifecycle_mutation() const noexcept
{
    if (m_isSystemCallbackActive)
    {
        return Result<void>::failure(
            make_state_error("Runtime world lifecycle cannot change during a system callback"));
    }
    return Result<void>::success();
}

void RuntimeWorld::assert_owner_thread() const noexcept
{
    const bool isOwner = std::this_thread::get_id() == m_ownerThread;
    CUE_ASSERT(*m_assertContext, isOwner, "Cue.GameCore runtime world API requires its owner thread");

    if (!isOwner)
    {
        m_assertContext->fatal_handler().terminate("Cue.GameCore runtime world API requires its owner thread");
    }
}

void RuntimeWorld::release_owned_state() noexcept
{
    m_commandBuffer.reset();
    m_transformType.reset();

    if (m_world != nullptr)
    {
        m_world->shutdown();
        m_world.reset();
    }
}

Error RuntimeWorld::make_state_error(std::string_view a_summary) const noexcept
{
    return make_game_core_error(*m_assertContext, GameCoreError::InvalidRuntimeState, a_summary);
}

[[noreturn]] void RuntimeWorld::terminate_allocation() noexcept
{
    release_owned_state();
    m_assertContext->fatal_handler().terminate("Cue.GameCore runtime world allocation failed");
}

[[noreturn]] void RuntimeWorld::terminate_exception() noexcept
{
    release_owned_state();
    m_assertContext->fatal_handler().terminate("Cue.GameCore runtime world caught an unexpected exception");
}
} // namespace cue::game_core
