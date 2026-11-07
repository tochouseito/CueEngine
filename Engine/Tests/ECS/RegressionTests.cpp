#include "TestComponents.h"

#include <array>
#include <cstdio>
#include <functional>
#include <unordered_map>

namespace cue::tests
{
namespace
{
using ecs::componentView;
using ecs::ECSManager;
using ecs::Entity;

/// @brief 全 Bucket を横断して Entity が一度だけ所属しているか確認する
bool has_one_bucket(ECSManager &a_ecs, Entity a_entity)
{
    std::size_t count = 0;
    for (const auto &[archetype, bucket] : a_ecs.get_arch_to_entities())
    {
        for (const auto entity : bucket.get_entities())
            count += entity == a_entity ? 1 : 0;
    }
    return count == 1;
}

/// @brief Entity 条件を持たず、任意の段階で Test の構造変更を実行する
class Driver final : public ECSManager::ISystem
{
  public:
    /// @brief Callback を所有し、捕捉先は Test の実行中だけ借用する
    Driver(std::function<void()> a_update, std::function<void()> a_initialize = {}, std::function<void()> a_awake = {})
        : m_update(std::move(a_update)), m_initialize(std::move(a_initialize)), m_awake(std::move(a_awake))
    {
    }
    /// @brief 構造変更用の更新段階を実行する
    void update() override
    {
        if (m_update)
            m_update();
    }
    /// @brief 構造変更用の初期化段階を実行する
    void initialize() override
    {
        if (m_initialize)
            m_initialize();
    }
    /// @brief 構造変更用の起動段階を実行する
    void awake() override
    {
        if (m_awake)
            m_awake();
    }

  private:
    std::function<void()> m_update;
    std::function<void()> m_initialize;
    std::function<void()> m_awake;
};

/// @brief 既存構成・値・Active を保ち、旧 System の再初期化と二重走査を防ぐ
int test_existing_entity()
{
    ECSManager ecs;
    const auto target = ecs.generate_entity();
    const auto inactive = ecs.generate_entity();
    const auto neighbor = ecs.generate_entity();
    ecs.add_component<Counter>(target)->value = 42;
    ecs.add_component<Counter>(inactive)->value = 7;
    ecs.add_component<Counter>(neighbor)->value = 9;
    ecs.set_entity_active(inactive, false);
    bool added = false;
    std::unordered_map<Entity, int> initA, initAB, updateA, updateAB;
    ecs.add_system<ECSManager::System<Counter>>(
        [&](Entity a_entity, Counter &)
        {
            ++updateA[a_entity];
            if (a_entity != target || added)
                return;
            added = true;
            ecs.add_component<Step>(target)->amount = 3;
            ecs.add_component<Step>(target);
            ecs.add_component<Tag>(target)->value = 11;
            ecs.add_component<Step>(inactive);
            ecs.set_entity_active(inactive, true);
            ecs.set_entity_active(inactive, false);
        },
        [&](Entity a_entity, Counter &) { ++initA[a_entity]; });
    ecs.add_system<ECSManager::System<Counter, Step>>([&](Entity a_entity, Counter &, Step &) { ++updateAB[a_entity]; },
                                                      [&](Entity a_entity, Counter &, Step &) { ++initAB[a_entity]; });
    ecs.initialize_all_systems();
    ecs.update_all_systems();
    if (!ecs.get_component<Counter>(target) || ecs.get_component<Counter>(target)->value != 42 ||
        ecs.get_component<Step>(target)->amount != 3 || ecs.is_entity_active(inactive) || initAB[target] != 0)
        return __LINE__;
    ecs.update_all_systems();
    ecs.update_all_systems();
    if (initA[target] != 1 || initAB[target] != 1 || updateA[target] != 3 || updateA[neighbor] != 3 ||
        updateAB[target] != 2 || initAB[inactive] != 0 || !has_one_bucket(ecs, target) ||
        !has_one_bucket(ecs, neighbor))
        return __LINE__;
    ecs.set_entity_active(inactive, true);
    ecs.update_all_systems();
    if (initAB[inactive] != 1 || updateAB[inactive] != 1)
        return __LINE__;
    return 0;
}

/// @brief 同じ更新中に作ったコピー元と空 Entity、既存コピー先の型保持を確認する
int test_staging_copies()
{
    ECSManager ecs;
    const auto seed = ecs.generate_entity();
    const auto destination = ecs.generate_entity();
    const auto otherDestination = ecs.generate_entity();
    const auto protectedDestination = ecs.generate_entity();
    ecs.add_component<Counter>(seed);
    ecs.add_component<Step>(destination)->amount = 3;
    ecs.add_component<Step>(otherDestination)->amount = 4;
    ecs.add_component<Counter>(protectedDestination)->value = 12;
    ecs.add_component<Step>(protectedDestination)->amount = 5;
    Entity source{}, clone{}, empty{};
    bool copied = false;
    ecs.add_system<Driver>(
        [&]()
        {
            if (copied)
                return;
            copied = true;
            empty = ecs.generate_entity();
            source = ecs.generate_entity();
            ecs.add_component<Counter>(source)->value = 71;
            ecs.add_component<Tag>(source)->value = 3;
            ecs.add_component<Tag>(source)->value = 4;
            clone = ecs.copy_entity(source);
            ecs.copy_components(source, destination);
            ecs.copy_entity(source, otherDestination);
            ecs.copy_components(source, protectedDestination, false);
        });
    ecs.update_all_systems();
    for (const auto entity : {source, clone, destination, otherDestination})
    {
        auto *counter = ecs.get_component<Counter>(entity);
        auto *tags = ecs.get_all_components<Tag>(entity);
        if (!counter || counter->value != 71 || !tags || tags->size() != 2 || tags->front().value != 3 ||
            tags->back().value != 4 || !has_one_bucket(ecs, entity))
            return __LINE__;
    }
    if (!ecs.get_component<Step>(destination) || ecs.get_component<Step>(destination)->amount != 3 ||
        !ecs.get_component<Step>(otherDestination) || ecs.get_component<Counter>(protectedDestination)->value != 12 ||
        !ecs.get_component<Step>(protectedDestination) || !has_one_bucket(ecs, empty))
        return __LINE__;
    return 0;
}

/// @brief Awake で作った Entity を明示 Initialize した後、Update で二度初期化しない
int test_awake_initialize()
{
    ECSManager ecs;
    int initialized = 0;
    int updated = 0;
    ecs.add_system<Driver>(std::function<void()>{}, std::function<void()>{},
                           [&]()
                           {
                               const auto entity = ecs.generate_entity();
                               ecs.add_component<Counter>(entity);
                           });
    ecs.add_system<ECSManager::System<Counter>>([&](Entity, Counter &) { ++updated; },
                                                [&](Entity, Counter &) { ++initialized; });
    ecs.awake_all_systems();
    ecs.initialize_all_systems();
    ecs.update_all_systems();
    ecs.update_all_systems();
    return initialized == 1 && updated == 2 ? 0 : __LINE__;
}

/// @brief Initialize 中の生成は System 順序に依存せず初回 Update 前に一度だけ初期化する
int test_initialize_order()
{
    for (const int priority : {5, 20})
    {
        ECSManager ecs;
        int initialized = 0;
        int updated = 0;
        auto &driver = ecs.add_system<Driver>(std::function<void()>{},
                                              [&]()
                                              {
                                                  const auto entity = ecs.generate_entity();
                                                  ecs.add_component<Counter>(entity);
                                              });
        driver.set_priority(10);
        auto &query = ecs.add_system<ECSManager::System<Counter>>([&](Entity, Counter &) { ++updated; },
                                                                  [&](Entity, Counter &) { ++initialized; });
        query.set_priority(priority);
        ecs.initialize_all_systems();
        ecs.update_all_systems();
        ecs.update_all_systems();
        if (initialized != 1 || updated != 2)
            return __LINE__;
    }
    return 0;
}

/// @brief 無効な System の通知を別 Pipeline が消さず、初回実行時だけ消費する
int test_pending_pipeline()
{
    ECSManager ecs;
    const auto entity = ecs.generate_entity();
    ecs.add_component<Counter>(entity);
    int initialized = 0;
    bool added = false;
    auto &driver = ecs.add_system<Driver>(
        [&]()
        {
            if (!added)
            {
                added = true;
                ecs.add_component<Step>(entity);
            }
        });
    auto &query = ecs.add_system<ECSManager::System<Counter, Step>>([](Entity, Counter &, Step &) {},
                                                                    [&](Entity, Counter &, Step &) { ++initialized; });
    query.set_enabled(false);
    ecs.update_all_systems();
    ECSManager::SystemPipeline onlyDriver;
    onlyDriver.add_system(&driver);
    onlyDriver.update(ecs);
    query.set_enabled(true);
    ECSManager::SystemPipeline onlyQuery;
    onlyQuery.add_system(&query);
    onlyQuery.update(ecs);
    onlyQuery.update(ecs);
    return initialized == 1 ? 0 : __LINE__;
}

/// @brief 中断後の確定、通知待ち削除、同じ番号の再利用を確認する
int test_cancel_and_recycle()
{
    ECSManager ecs;
    Entity removed{};
    bool ran = false;
    int initialized = 0;
    auto &driver = ecs.add_system<Driver>(
        [&]()
        {
            if (ran)
                return;
            ran = true;
            removed = ecs.generate_entity();
            ecs.add_component<Counter>(removed);
            ecs.remove_entity(removed);
            ecs.cancel_update_loop();
        });
    auto &query = ecs.add_system<ECSManager::System<Counter>>([](Entity, Counter &) {},
                                                              [&](Entity, Counter &) { ++initialized; });
    driver.set_priority(0);
    query.set_priority(1);
    ecs.update_all_systems();
    if (ecs.is_entity_active(removed) || ecs.get_component<Counter>(removed))
        return __LINE__;
    const auto recycled = ecs.generate_entity();
    ecs.add_component<Counter>(recycled);
    driver.set_enabled(false);
    ecs.update_all_systems();
    return recycled == removed && initialized == 0 && has_one_bucket(ecs, recycled) ? 0 : __LINE__;
}

/// @brief 全段階と Entity ごとの段階で同じ本体へ書き、Inactive Component を除外する
int test_multi_phases()
{
    for (const bool context : {false, true})
    {
        ECSManager ecs;
        const auto entity = ecs.generate_entity();
        ecs.add_component<Tag>(entity)->value = 0;
        ecs.add_component<Tag>(entity)->value = 100;
        ecs.add_component<Tag>(entity)->value = 1;
        ecs.get_all_components<Tag>(entity)->at(1).set_active(false);
        bool isSameStorage = true;
        auto mutate = [&](Entity a_entity, componentView<Tag> a_view)
        {
            auto &storage = *ecs.get_all_components<Tag>(a_entity);
            isSameStorage = isSameStorage && a_view.size() == 2 && a_view[0] == &storage[0] && a_view[1] == &storage[2];
            for (auto *component : a_view)
                ++component->value;
        };
        ECSManager::MultiComponentSystem<Tag> *system = nullptr;
        if (context)
        {
            system = &ecs.add_system<ECSManager::MultiComponentSystem<Tag>>(
                [&](Entity e, componentView<Tag> v, const ecs::UpdateContext &) { mutate(e, v); },
                [&](Entity e, componentView<Tag> v, const ecs::InitializeContext &) { mutate(e, v); },
                [&](Entity e, componentView<Tag> v, const ecs::FinalizeContext &) { mutate(e, v); },
                [&](Entity e, componentView<Tag> v, const ecs::AwakeContext &) { mutate(e, v); });
        }
        else
            system = &ecs.add_system<ECSManager::MultiComponentSystem<Tag>>(mutate, mutate, mutate, mutate);
        ecs.awake_all_systems();
        ecs.initialize_all_systems();
        ecs.update_all_systems();
        ecs.finalize_all_systems();
        system->awake_entity(entity);
        system->initialize_entity(entity);
        system->finalize_entity(entity);
        const auto &values = *ecs.get_all_components<Tag>(entity);
        if (!isSameStorage || values[0].value != 7 || values[1].value != 100 || values[2].value != 8)
            return __LINE__;
    }
    return 0;
}

/// @brief Staging 本体を含む View の再配置を拒否し、defer 経由の追加を Callback 終了後に行う
int test_multi_staging_view()
{
    ECSManager ecs;
    const auto entity = ecs.generate_entity();
    ecs.add_component<Tag>(entity)->value = 1;
    bool denied = false;
    bool ran = false;
    bool queued = false;
    auto mutate = [&](Entity e, componentView<Tag> v)
    {
        try
        {
            ecs.add_component<Tag>(e);
        }
        catch (const std::logic_error &)
        {
            denied = true;
        }
        for (auto *component : v)
            ++component->value;
        if (!queued)
        {
            queued = true;
            ecs.defer([&, e]() { ecs.add_component<Tag>(e)->value = 7; });
        }
    };
    auto &multi = ecs.add_system<ECSManager::MultiComponentSystem<Tag>>([](Entity, componentView<Tag>) {}, mutate,
                                                                        mutate, mutate);
    auto &driver = ecs.add_system<Driver>(
        [&]()
        {
            if (ran)
                return;
            ran = true;
            ecs.add_component<Tag>(entity)->value = 3;
            multi.awake_entity(entity);
            multi.initialize_entity(entity);
            multi.finalize_entity(entity);
        });
    driver.set_priority(0);
    multi.set_priority(10);
    ecs.update_all_systems();
    const auto &values = *ecs.get_all_components<Tag>(entity);
    if (!denied || values.size() != 3 || values[0].value != 4 || values[1].value != 6 || values[2].value != 7)
        return __LINE__;
    return 0;
}

/// @brief View 中に要求した全削除で参照が失効せず、旧 Bucket と Pending が残らない
int test_multi_deferred_removal()
{
    ECSManager ecs;
    const auto entity = ecs.generate_entity();
    ecs.add_component<Counter>(entity);
    ecs.add_component<Tag>(entity)->value = 10;
    ecs.add_component<Tag>(entity)->value = 20;
    int finalizedSum = 0;
    int counterUpdates = 0;
    ecs.add_system<ECSManager::MultiComponentSystem<Tag>>(
        [&](Entity e, componentView<Tag> v)
        {
            for (auto *component : v)
                component->value += 3;
            ecs.remove_all_components<Tag>(e);
            if (v.front()->value != 13 || v.back()->value != 23)
                finalizedSum = -1000;
        },
        std::function<void(Entity, componentView<Tag>)>{},
        [&](Entity, componentView<Tag> v)
        {
            for (auto *component : v)
                finalizedSum += component->value;
        });
    ecs.add_system<ECSManager::System<Counter>>([&](Entity, Counter &) { ++counterUpdates; });
    ecs.update_all_systems();
    ecs.update_all_systems();
    if (finalizedSum != 36 || ecs.get_all_components<Tag>(entity) || counterUpdates != 2 ||
        !has_one_bucket(ecs, entity))
        return __LINE__;
    ECSManager::EntityContainer bucket;
    bucket.add(0);
    bucket.add(2);
    bucket.add(2);
    bucket.remove(1);
    bucket.remove(2);
    bucket.remove(2);
    return bucket.get_entities() == std::vector<Entity>{0} ? 0 : __LINE__;
}
} // namespace

/// @brief View から遅延した各追加・コピー経路でも、初回 Update より先に初期化する
int test_multi_deferred_initialization()
{
    ECSManager ecs;
    const auto owner = ecs.generate_entity();
    ecs.add_component<Tag>(owner);
    const auto source = ecs.generate_entity();
    ecs.add_component<Counter>(source)->value = 42;
    std::array<Entity, 5> targets{};
    for (std::size_t index = 2; index < targets.size(); ++index)
    {
        targets[index] = ecs.generate_entity();
        ecs.add_component<Step>(targets[index]);
    }
    bool scheduled = false;
    bool updatedBeforeInitialize = false;
    std::unordered_map<Entity, int> initializations;
    std::unordered_map<Entity, int> combinedInitializations;
    ecs.add_system<ECSManager::MultiComponentSystem<Tag>>(
        [&](Entity, componentView<Tag>)
        {
            if (scheduled)
                return;
            scheduled = true;
            ecs.defer(
                [&]()
                {
                    targets[0] = ecs.generate_entity();
                    ecs.add_component<Counter>(targets[0])->value = 42;
                    targets[1] = ecs.copy_entity(source);
                    ecs.copy_components(source, targets[2]);
                    ecs.copy_entity(source, targets[3]);
                    Counter restored;
                    restored.value = 42;
                    ecs.prefab_add_component(targets[4], restored);
                    ecs.add_component<Step>(source);
                });
        });
    ecs.add_system<ECSManager::System<Counter>>([&](Entity a_entity, Counter &)
                                                { updatedBeforeInitialize |= initializations[a_entity] != 1; },
                                                [&](Entity a_entity, Counter &) { ++initializations[a_entity]; });
    ecs.add_system<ECSManager::System<Counter, Step>>(
        [&](Entity a_entity, Counter &, Step &) { updatedBeforeInitialize |= combinedInitializations[a_entity] != 1; },
        [&](Entity a_entity, Counter &, Step &) { ++combinedInitializations[a_entity]; });
    ecs.initialize_all_systems();
    ecs.update_all_systems();
    ecs.update_all_systems();
    ecs.update_all_systems();
    for (const auto entity : targets)
    {
        if (initializations[entity] != 1 || ecs.get_component<Counter>(entity)->value != 42 ||
            !has_one_bucket(ecs, entity))
            return __LINE__;
    }
    for (std::size_t index = 2; index < targets.size(); ++index)
        if (combinedInitializations[targets[index]] != 1)
            return __LINE__;
    return !updatedBeforeInitialize && initializations[source] == 1 && combinedInitializations[source] == 1 ? 0
                                                                                                            : __LINE__;
}

/// @brief 明示 Initialize と Update が非 Active Entity / Component の通知を消費しない
int test_inactive_pending_initialization()
{
    for (const bool inactiveEntity : {true, false})
    {
        ECSManager ecs;
        Entity entity{};
        int initialized = 0;
        bool generated = false;
        ecs.add_system<Driver>(
            [&]()
            {
                if (generated)
                    return;
                generated = true;
                entity = ecs.generate_entity();
                auto *component = ecs.add_component<Counter>(entity);
                if (inactiveEntity)
                    ecs.set_entity_active(entity, false);
                else
                    component->set_active(false);
            });
        ecs.add_system<ECSManager::System<Counter>>([](Entity, Counter &) {},
                                                    [&](Entity, Counter &) { ++initialized; });
        ecs.update_all_systems();
        ecs.initialize_all_systems();
        ecs.update_all_systems();
        if (initialized != 0)
            return __LINE__;
        ecs.set_entity_active(entity, true);
        ecs.get_component<Counter>(entity)->set_active(true);
        ecs.update_all_systems();
        ecs.update_all_systems();
        if (initialized != 1)
            return __LINE__;
    }
    ECSManager ecs;
    Entity entity{};
    int initialized = 0;
    bool generated = false;
    ecs.add_system<Driver>(
        [&]()
        {
            if (!generated)
            {
                generated = true;
                entity = ecs.generate_entity();
                ecs.add_component<Tag>(entity)->set_active(false);
            }
        });
    ecs.add_system<ECSManager::MultiComponentSystem<Tag>>([](Entity, componentView<Tag>) {},
                                                          [&](Entity, componentView<Tag>) { ++initialized; });
    ecs.update_all_systems();
    ecs.initialize_all_systems();
    ecs.update_all_systems();
    if (initialized != 0)
        return __LINE__;
    ecs.get_all_components<Tag>(entity)->front().set_active(true);
    ecs.update_all_systems();
    ecs.update_all_systems();
    return initialized == 1 ? 0 : __LINE__;
}

/// @brief 初期化 Callback が次の Entity の Active を変更しても、実際の処理分だけ通知を消費する
int test_initialize_activation_changes()
{
    for (const bool useMulti : {false, true})
    {
        for (const bool startsActive : {false, true})
        {
            ECSManager ecs;
            std::array<Entity, 2> entities{};
            Entity trigger{};
            Entity target{};
            bool generated = false;
            std::unordered_map<Entity, int> initialized;
            ecs.add_system<Driver>(
                [&]()
                {
                    if (generated)
                        return;
                    generated = true;
                    for (auto &entity : entities)
                    {
                        entity = ecs.generate_entity();
                        ecs.add_component<Counter>(entity);
                        ecs.add_component<Tag>(entity);
                    }
                });
            const auto onInitialize = [&](Entity a_entity)
            {
                ++initialized[a_entity];
                if (a_entity == trigger)
                    ecs.set_entity_active(target, !startsActive);
            };
            if (useMulti)
                ecs.add_system<ECSManager::MultiComponentSystem<Tag>>([](Entity, componentView<Tag>) {},
                                                                      [&](Entity a_entity, componentView<Tag>)
                                                                      { onInitialize(a_entity); });
            else
                ecs.add_system<ECSManager::System<Counter>>([](Entity, Counter &) {}, [&](Entity a_entity, Counter &)
                                                            { onInitialize(a_entity); });
            ecs.update_all_systems();
            // Multi Pool は順不同の Map。実際の走査順の先頭が後続を切り替えるよう指定する
            trigger = useMulti ? ecs.get_component_pool<Tag>()->map().begin()->first : entities[0];
            target = trigger == entities[0] ? entities[1] : entities[0];
            ecs.set_entity_active(target, startsActive);
            ecs.initialize_all_systems();
            ecs.update_all_systems();
            if (initialized[trigger] != 1 || initialized[target] != (startsActive ? 0 : 1))
                return __LINE__;
            ecs.set_entity_active(target, true);
            ecs.update_all_systems();
            ecs.update_all_systems();
            if (initialized[trigger] != 1 || initialized[target] != 1)
                return __LINE__;
        }
    }
    return 0;
}

/// @brief ECS 修正の複合回帰ケースを実行し、失敗位置を報告する
int run_ecs_regressions()
{
    const std::array tests{test_existing_entity,
                           test_staging_copies,
                           test_awake_initialize,
                           test_initialize_order,
                           test_pending_pipeline,
                           test_cancel_and_recycle,
                           test_multi_phases,
                           test_multi_staging_view,
                           test_multi_deferred_removal,
                           test_multi_deferred_initialization,
                           test_inactive_pending_initialization,
                           test_initialize_activation_changes};
    for (std::size_t index = 0; index < tests.size(); ++index)
    {
        const auto line = tests[index]();
        if (line != 0)
        {
            std::fprintf(stderr, "ECS regression %zu failed at line %d\n", index, line);
            return line;
        }
    }
    return 0;
}
} // namespace cue::tests
