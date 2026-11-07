#include <Runtime/World.h>

#include <algorithm>
#include <atomic>
#include <limits>
#include <utility>

#include <Foundation/Utf8.h>
#include <Runtime/Components/TransformComponent.h>

namespace cue
{
namespace
{
// Handle の識別子だけを発行し、World 本体や実行状態は Global に保持しない
std::atomic<std::uint64_t> g_nextWorldId{1};

/// @brief Scene / Object ID と名前に使う UTF-8 の小さい所有文字列を検証する
bool valid_text(std::string_view a_text) noexcept
{
    if (a_text.empty() || a_text.size() > 1024 || !is_valid_utf8_text(a_text))
        return false;
    return std::none_of(a_text.begin(), a_text.end(),
                        [](unsigned char a_value) { return a_value < 0x20 || a_value == 0x7f; });
}
} // namespace

/// @brief ECS の実行順序を再利用し、外部へ生の ECS ID / Manager を公開せず WorldSystem を所有する
/// Entity ごとの Finalize は基底の no-op とし、Global Shutdown と混同しない
class World::SystemAdapter final : public ecs::ECSManager::ISystem
{
  public:
    /// @brief System と要求優先度を所有し、World は全 Callback の間借用する
    SystemAdapter(World &a_world, std::unique_ptr<WorldSystem> a_system, std::int32_t a_priority)
        : m_requestedPriority(a_priority), m_world(a_world), m_system(std::move(a_system))
    {
    }
    /// @brief Result の失敗を保持して後続を抑制し、ECS の Pipeline は正常復帰させる
    void initialize() override
    {
        if (m_world.m_failure)
            return;
        m_started = true;
        auto result = m_system->initialize(m_world);
        if (!result.has_value())
            m_world.record_failure(*result.try_error());
    }
    /// @brief World が検証した時間を使って System を一度実行する
    void update() override
    {
        if (!m_started || m_world.m_failure)
            return;
        auto result = m_system->update(m_world, m_world.m_context);
        if (!result.has_value())
            m_world.record_failure(*result.try_error());
    }
    /// @brief 初期化を開始した System を一度だけ停止する。例外後も再実行しない
    void stop()
    {
        if (!m_started || m_stopped)
            return;
        m_stopped = true;
        auto result = m_system->shutdown(m_world);
        if (!result.has_value())
            m_world.record_failure(*result.try_error());
    }

    /// @brief 登録順と組み合わせて実行順位を決める要求優先度を返す
    [[nodiscard]] std::int32_t requested_priority() const noexcept
    {
        return m_requestedPriority;
    }

  private:
    const std::int32_t m_requestedPriority;
    World &m_world;
    std::unique_ptr<WorldSystem> m_system;
    bool m_started = false;
    bool m_stopped = false;
};

Result<std::unique_ptr<World>> World::create()
{
    // 識別子が一周して過去の Handle と衝突することを許可しない
    auto id = g_nextWorldId.load(std::memory_order_relaxed);
    while (id != (std::numeric_limits<std::uint64_t>::max)())
    {
        if (g_nextWorldId.compare_exchange_weak(id, id + 1, std::memory_order_relaxed))
        {
            try
            {
                return Result<std::unique_ptr<World>>::success(std::make_unique<World>(ConstructionKey{}, id));
            }
            catch (...)
            {
                return Result<std::unique_ptr<World>>::failure({ErrorCategory::Fatal, "World.create.exception"});
            }
        }
    }
    return Result<std::unique_ptr<World>>::failure({ErrorCategory::InvalidState, "World.id_exhausted"});
}

World::World(ConstructionKey, std::uint64_t a_id)
    : m_id(a_id), m_owner(std::this_thread::get_id()), m_ecs(std::make_unique<ecs::ECSManager>())
{
}

World::~World()
{
    // 破棄も Owner Thread が前提。Destructor から例外を外へ出さない
    if (std::this_thread::get_id() == m_owner)
    {
        try
        {
            auto stopped = shutdown();
            (void)stopped;
        }
        catch (...)
        {
        }
    }
}

Result<void> World::check_access() const
{
    if (std::this_thread::get_id() != m_owner)
        return Result<void>::failure({ErrorCategory::WrongThread, "World.owner_thread"});
    if (m_isMutating)
        return Result<void>::failure({ErrorCategory::InvalidState, "World.storage_reentry"});
    if (m_lifecycle == Lifecycle::Stopped || (m_lifecycle == Lifecycle::Failed && m_phase != Phase::Stopping))
        return Result<void>::failure({ErrorCategory::InvalidState, "World.not_available"});
    return Result<void>::success();
}

Result<void> World::check_structure() const
{
    auto access = check_access();
    if (!access.has_value())
        return access;
    if (m_isIterating || m_isMutating || (m_phase != Phase::Idle && m_phase != Phase::Committing))
        return Result<void>::failure({ErrorCategory::InvalidState, "World.structure_requires_defer"});
    return Result<void>::success();
}

bool World::must_defer() const noexcept
{
    return m_isIterating || m_phase == Phase::Initializing || m_phase == Phase::Updating;
}

bool World::live(SceneInstanceHandle a_instance) const noexcept
{
    return m_lifecycle != Lifecycle::Stopped && a_instance.world == m_id && a_instance.index < m_instances.size() &&
           m_instances[a_instance.index].state == SlotState::Live &&
           m_instances[a_instance.index].generation == a_instance.generation;
}

bool World::live(EntityHandle a_entity) const noexcept
{
    return m_lifecycle != Lifecycle::Stopped && a_entity.world == m_id && a_entity.index < m_entities.size() &&
           m_entities[a_entity.index].state == SlotState::Live &&
           m_entities[a_entity.index].generation == a_entity.generation && live(m_entities[a_entity.index].instance);
}

EntityHandle World::entity_handle(std::size_t a_index) const noexcept
{
    return {m_id, static_cast<std::uint32_t>(a_index), m_entities[a_index].generation};
}

Error World::invalid_handle(const char *a_operation)
{
    return {ErrorCategory::InvalidArgument, a_operation};
}

void World::record_failure(Error a_error)
{
    if (!m_failure)
        m_failure = std::move(a_error);
    if (m_phase != Phase::Stopping)
        m_lifecycle = Lifecycle::Failed;
}

Result<SceneInstanceHandle> World::create_scene_instance(std::string_view a_sceneId)
{
    auto access = check_structure();
    if (!access.has_value())
        return Result<SceneInstanceHandle>::failure(*access.try_error());
    if (!valid_text(a_sceneId))
        return Result<SceneInstanceHandle>::failure({ErrorCategory::InvalidArgument, "World.scene_id"});
    try
    {
        ScopedFlag mutation(m_isMutating);
        InstanceSlot instance;
        instance.sceneId = a_sceneId;
        instance.state = SlotState::Live;
        for (std::size_t index = 0; index < m_instances.size(); ++index)
        {
            if (m_instances[index].state != SlotState::Free)
                continue;
            instance.generation = m_instances[index].generation;
            m_instances[index] = std::move(instance);
            return Result<SceneInstanceHandle>::success(
                {m_id, static_cast<std::uint32_t>(index), m_instances[index].generation});
        }
        if (m_instances.size() > (std::numeric_limits<std::uint32_t>::max)())
            return Result<SceneInstanceHandle>::failure({ErrorCategory::InvalidState, "World.instance_capacity"});
        m_instances.push_back(std::move(instance));
        return Result<SceneInstanceHandle>::success({m_id, static_cast<std::uint32_t>(m_instances.size() - 1), 1});
    }
    catch (...)
    {
        return fail_exception<SceneInstanceHandle>("World.create_scene_instance.exception");
    }
}

Result<EntityHandle> World::create_entity(SceneInstanceHandle a_instance, std::string_view a_objectId,
                                          std::string_view a_name)
{
    auto access = check_structure();
    if (!access.has_value())
        return Result<EntityHandle>::failure(*access.try_error());
    if (!live(a_instance))
        return Result<EntityHandle>::failure(invalid_handle("World.create_entity.instance"));
    if (!valid_text(a_objectId) || !valid_text(a_name))
        return Result<EntityHandle>::failure({ErrorCategory::InvalidArgument, "World.object_text"});
    for (const auto &entry : m_entities)
        if (entry.state == SlotState::Live && entry.instance == a_instance && entry.objectId == a_objectId)
            return Result<EntityHandle>::failure({ErrorCategory::InvalidArgument, "World.duplicate_object_id"});
    try
    {
        ScopedFlag mutation(m_isMutating);
        EntitySlot entity;
        entity.instance = a_instance;
        entity.objectId = a_objectId;
        entity.name = a_name;
        // Metadata の確保を終えてから ECS を生成し、途中例外では World を終端失敗にする
        std::size_t index = 0;
        while (index < m_entities.size() && m_entities[index].state != SlotState::Free)
            ++index;
        if (index == m_entities.size())
        {
            if (index > (std::numeric_limits<std::uint32_t>::max)())
                return Result<EntityHandle>::failure({ErrorCategory::InvalidState, "World.entity_capacity"});
            m_entities.emplace_back();
        }
        entity.generation = m_entities[index].generation;
        entity.entity = m_ecs->generate_entity();
        entity.state = SlotState::Live;
        m_entities[index] = std::move(entity);
        return Result<EntityHandle>::success(entity_handle(index));
    }
    catch (...)
    {
        return fail_exception<EntityHandle>("World.create_entity.exception");
    }
}

void World::erase_entity(EntityHandle a_entity)
{
    if (a_entity.world != m_id || a_entity.index >= m_entities.size())
        return;
    auto &entry = m_entities[a_entity.index];
    if (entry.generation != a_entity.generation ||
        (entry.state != SlotState::Live && entry.state != SlotState::Retiring))
        return;
    ScopedFlag mutation(m_isMutating);
    // 実体の破棄中の再入からも非公開とする
    entry.state = SlotState::Retiring;
    m_ecs->remove_entity(entry.entity);
    entry.objectId.clear();
    entry.name.clear();
    entry.instance = {};
    if (entry.generation == (std::numeric_limits<std::uint64_t>::max)())
        entry.state = SlotState::Exhausted;
    else
    {
        ++entry.generation;
        entry.state = SlotState::Free;
    }
}

void World::erase_instance(SceneInstanceHandle a_instance)
{
    if (a_instance.world != m_id || a_instance.index >= m_instances.size())
        return;
    auto &instance = m_instances[a_instance.index];
    if (instance.generation != a_instance.generation ||
        (instance.state != SlotState::Live && instance.state != SlotState::Retiring))
        return;
    ScopedFlag mutation(m_isMutating);
    instance.state = SlotState::Retiring;
    for (std::size_t index = 0; index < m_entities.size(); ++index)
        if (m_entities[index].instance == a_instance)
            erase_entity(entity_handle(index));
    instance.sceneId.clear();
    if (instance.generation == (std::numeric_limits<std::uint64_t>::max)())
        instance.state = SlotState::Exhausted;
    else
    {
        ++instance.generation;
        instance.state = SlotState::Free;
    }
}

Result<void> World::destroy_entity(EntityHandle a_entity)
{
    auto access = check_access();
    if (!access.has_value())
        return access;
    if (!live(a_entity))
        return Result<void>::failure(invalid_handle("World.destroy_entity"));
    if (must_defer())
    {
        auto queued = defer(
            [a_entity](World &a_world)
            {
                a_world.erase_entity(a_entity);
                return Result<void>::success();
            });
        if (queued.has_value())
            m_entities[a_entity.index].state = SlotState::Retiring;
        return queued;
    }
    auto structure = check_structure();
    if (!structure.has_value())
        return structure;
    try
    {
        erase_entity(a_entity);
        return Result<void>::success();
    }
    catch (...)
    {
        return fail_exception<void>("World.destroy_entity.exception");
    }
}

Result<void> World::destroy_scene_instance(SceneInstanceHandle a_instance)
{
    auto access = check_access();
    if (!access.has_value())
        return access;
    if (!live(a_instance))
        return Result<void>::failure(invalid_handle("World.destroy_scene_instance"));
    if (must_defer())
    {
        auto queued = defer(
            [a_instance](World &a_world)
            {
                a_world.erase_instance(a_instance);
                return Result<void>::success();
            });
        if (queued.has_value())
        {
            m_instances[a_instance.index].state = SlotState::Retiring;
            for (auto &entry : m_entities)
                if (entry.state == SlotState::Live && entry.instance == a_instance)
                    entry.state = SlotState::Retiring;
        }
        return queued;
    }
    auto structure = check_structure();
    if (!structure.has_value())
        return structure;
    try
    {
        erase_instance(a_instance);
        return Result<void>::success();
    }
    catch (...)
    {
        return fail_exception<void>("World.destroy_scene_instance.exception");
    }
}

Result<EntityHandle> World::find(SceneInstanceHandle a_instance, std::string_view a_key, bool a_byName) const
{
    auto access = check_access();
    if (!access.has_value())
        return Result<EntityHandle>::failure(*access.try_error());
    if (!live(a_instance))
        return Result<EntityHandle>::failure(invalid_handle("World.find.instance"));
    if (!valid_text(a_key))
        return Result<EntityHandle>::failure({ErrorCategory::InvalidArgument, "World.find.key"});
    std::optional<EntityHandle> found;
    for (std::size_t index = 0; index < m_entities.size(); ++index)
    {
        const auto &entry = m_entities[index];
        if (entry.state != SlotState::Live || entry.instance != a_instance ||
            (a_byName ? entry.name : entry.objectId) != a_key)
            continue;
        if (found)
            return Result<EntityHandle>::failure({ErrorCategory::InvalidState, "World.find.ambiguous_name"});
        found = entity_handle(index);
    }
    if (!found)
        return Result<EntityHandle>::failure({ErrorCategory::InvalidArgument, "World.find.not_found"});
    return Result<EntityHandle>::success(*found);
}

Result<EntityHandle> World::find_by_id(SceneInstanceHandle a_instance, std::string_view a_objectId) const
{
    return find(a_instance, a_objectId, false);
}

Result<EntityHandle> World::find_by_name(SceneInstanceHandle a_instance, std::string_view a_name) const
{
    return find(a_instance, a_name, true);
}

Result<std::vector<EntityHandle>> World::entities(SceneInstanceHandle a_instance) const
{
    auto access = check_access();
    if (!access.has_value())
        return Result<std::vector<EntityHandle>>::failure(*access.try_error());
    if (!live(a_instance))
        return Result<std::vector<EntityHandle>>::failure(invalid_handle("World.entities.instance"));
    try
    {
        std::vector<EntityHandle> result;
        for (std::size_t index = 0; index < m_entities.size(); ++index)
            if (m_entities[index].state == SlotState::Live && m_entities[index].instance == a_instance)
                result.push_back(entity_handle(index));
        return Result<std::vector<EntityHandle>>::success(std::move(result));
    }
    catch (...)
    {
        return Result<std::vector<EntityHandle>>::failure({ErrorCategory::Fatal, "World.entities.exception"});
    }
}

Result<bool> World::is_alive(EntityHandle a_entity) const
{
    if (std::this_thread::get_id() != m_owner)
        return Result<bool>::failure({ErrorCategory::WrongThread, "World.owner_thread"});
    return Result<bool>::success(m_lifecycle != Lifecycle::Failed && live(a_entity));
}

Result<std::vector<EntityHandle>> World::entities() const
{
    auto access = check_access();
    if (!access.has_value())
        return Result<std::vector<EntityHandle>>::failure(*access.try_error());
    try
    {
        std::vector<EntityHandle> result;
        result.reserve(m_entities.size());
        for (std::size_t index = 0; index < m_entities.size(); ++index)
            if (m_entities[index].state == SlotState::Live)
                result.push_back(entity_handle(index));
        return Result<std::vector<EntityHandle>>::success(std::move(result));
    }
    catch (...)
    {
        return Result<std::vector<EntityHandle>>::failure({ErrorCategory::Fatal, "World.entities.exception"});
    }
}

Result<bool> World::is_active(EntityHandle a_entity) const
{
    auto access = check_access();
    if (!access.has_value())
        return Result<bool>::failure(*access.try_error());
    if (!live(a_entity))
        return Result<bool>::failure(invalid_handle("World.is_active"));
    return Result<bool>::success(m_ecs->is_entity_active(m_entities[a_entity.index].entity));
}

Result<bool> World::is_alive(SceneInstanceHandle a_instance) const
{
    if (std::this_thread::get_id() != m_owner)
        return Result<bool>::failure({ErrorCategory::WrongThread, "World.owner_thread"});
    return Result<bool>::success(m_lifecycle != Lifecycle::Failed && live(a_instance));
}

Result<void> World::set_active(EntityHandle a_entity, bool a_active)
{
    auto access = check_access();
    if (!access.has_value())
        return access;
    if (!live(a_entity))
        return Result<void>::failure(invalid_handle("World.set_active"));
    m_ecs->set_entity_active(m_entities[a_entity.index].entity, a_active);
    return Result<void>::success();
}

Result<void> World::add_system(std::unique_ptr<WorldSystem> a_system, std::int32_t a_priority)
{
    auto access = check_structure();
    if (!access.has_value())
        return access;
    if (m_lifecycle != Lifecycle::Created || m_phase != Phase::Idle || !a_system || a_priority < 0 ||
        m_systems.size() >= static_cast<std::size_t>((std::numeric_limits<int>::max)()))
        return Result<void>::failure({ErrorCategory::InvalidArgument, "World.add_system.arguments"});
    try
    {
        ScopedFlag mutation(m_isMutating);
        // ECS に所有を渡した後の World 配列の追加で確保しない
        m_systems.reserve(m_systems.size() + 1);
        auto &adapter = m_ecs->add_system<SystemAdapter>(*this, std::move(a_system), a_priority);
        m_systems.push_back(&adapter);
        return Result<void>::success();
    }
    catch (...)
    {
        return fail_exception<void>("World.add_system.exception");
    }
}

Result<void> World::defer(std::function<Result<void>(World &)> a_command)
{
    auto access = check_access();
    if (!access.has_value())
        return access;
    if (!a_command || m_phase == Phase::Stopping)
        return Result<void>::failure({ErrorCategory::InvalidArgument, "World.defer.arguments"});
    try
    {
        m_commands.push_back(std::move(a_command));
        return Result<void>::success();
    }
    catch (...)
    {
        return fail_exception<void>("World.defer.exception");
    }
}

Result<void> World::synchronize_transforms()
{
    // 未使用の型を Archetype の Bit で照会しない。型 ID が上限外でも Pool がなければ同期不要
    auto *pool = m_ecs->get_component_pool<TransformComponent>();
    if (!pool)
        return Result<void>::success();
    for (const auto &entry : m_entities)
    {
        if (entry.state != SlotState::Live)
            continue;
        auto *transform = pool->get_component(entry.entity);
        if (!transform)
            continue;
        auto result = transform->synchronize();
        if (!result.has_value())
            return result;
    }
    return Result<void>::success();
}

Result<void> World::commit()
{
    const auto previousPhase = m_phase;
    m_phase = Phase::Committing;
    std::size_t count = 0;
    try
    {
        // Command が次の Command を予約しても、走査中の配列を失効させない
        while (!m_commands.empty() && !m_failure)
        {
            std::vector<std::function<Result<void>(World &)>> commands;
            commands.swap(m_commands);
            for (auto &command : commands)
            {
                if (++count > 4096)
                {
                    record_failure({ErrorCategory::InvalidState, "World.deferred_limit"});
                    break;
                }
                auto result = command(*this);
                if (!result.has_value())
                    record_failure(*result.try_error());
                if (m_failure)
                    break;
            }
        }
        if (!m_failure)
        {
            auto transforms = synchronize_transforms();
            if (!transforms.has_value())
                record_failure(*transforms.try_error());
        }
    }
    catch (...)
    {
        record_failure({ErrorCategory::Fatal, "World.commit.exception"});
    }
    m_phase = previousPhase;
    if (m_failure)
        return Result<void>::failure(*m_failure);
    return Result<void>::success();
}

Result<void> World::initialize()
{
    auto access = check_structure();
    if (!access.has_value())
        return access;
    if (m_phase != Phase::Idle || m_lifecycle != Lifecycle::Created)
        return Result<void>::failure({ErrorCategory::InvalidState, "World.initialize.lifecycle"});
    try
    {
        auto prepared = commit();
        if (!prepared.has_value())
            return prepared;
        // ECS の同順位 Sort に依存せず、要求優先度と登録順から一意の実行順位を作る
        std::stable_sort(m_systems.begin(), m_systems.end(), [](const auto *a_left, const auto *a_right)
                         { return a_left->requested_priority() < a_right->requested_priority(); });
        for (std::size_t index = 0; index < m_systems.size(); ++index)
            m_systems[index]->set_priority(static_cast<int>(index));
        m_phase = Phase::Initializing;
        m_ecs->initialize_all_systems();
        auto committed = commit();
        m_phase = Phase::Idle;
        if (!committed.has_value())
            return committed;
        m_lifecycle = Lifecycle::Running;
        return Result<void>::success();
    }
    catch (...)
    {
        m_phase = Phase::Idle;
        return fail_exception<void>("World.initialize.exception");
    }
}

Result<void> World::update(std::chrono::nanoseconds a_deltaTime)
{
    auto access = check_structure();
    if (!access.has_value())
        return access;
    if (m_phase != Phase::Idle || m_lifecycle != Lifecycle::Running || a_deltaTime.count() < 0 ||
        m_context.tick == (std::numeric_limits<std::uint64_t>::max)())
        return Result<void>::failure({ErrorCategory::InvalidState, "World.update.arguments"});
    try
    {
        auto prepared = commit();
        if (!prepared.has_value())
            return prepared;
        m_context.deltaTime = a_deltaTime;
        ++m_context.tick;
        m_phase = Phase::Updating;
        m_ecs->update_all_systems({0, std::chrono::duration<float>(a_deltaTime).count()});
        auto committed = commit();
        m_phase = Phase::Idle;
        return committed;
    }
    catch (...)
    {
        m_phase = Phase::Idle;
        return fail_exception<void>("World.update.exception");
    }
}

Result<void> World::shutdown()
{
    if (std::this_thread::get_id() != m_owner)
        return Result<void>::failure({ErrorCategory::WrongThread, "World.owner_thread"});
    if (m_phase != Phase::Idle || m_isIterating || m_isMutating)
        return Result<void>::failure({ErrorCategory::InvalidState, "World.shutdown.reentry"});
    if (m_lifecycle == Lifecycle::Stopped)
        return m_failure ? Result<void>::failure(*m_failure) : Result<void>::success();
    m_phase = Phase::Stopping;
    // 失敗した System の後も回収を継続する。Component を読む必要があるため ECS は最後に破棄する
    for (auto it = m_systems.rbegin(); it != m_systems.rend(); ++it)
    {
        try
        {
            (*it)->stop();
        }
        catch (...)
        {
            record_failure({ErrorCategory::Fatal, "World.shutdown.exception"});
        }
    }
    // 回収中の Component / System の Destructor から半破棄の ECS を借用させない
    m_lifecycle = Lifecycle::Stopped;
    m_commands.clear();
    m_systems.clear();
    m_ecs.reset();
    m_entities.clear();
    m_instances.clear();
    m_phase = Phase::Idle;
    return m_failure ? Result<void>::failure(*m_failure) : Result<void>::success();
}
} // namespace cue
