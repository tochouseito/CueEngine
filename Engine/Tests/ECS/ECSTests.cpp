#include "TestComponents.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <limits>
#include <string_view>
#include <thread>

namespace
{
using cue::ecs::ECSManager;
using cue::ecs::Entity;
using cue::tests::Counter;
using cue::tests::SharedCounter;
using cue::tests::Step;
using cue::tests::Tag;

/// @brief Entity と Component の通知回数を所有する
struct EntityEvents final : cue::ecs::IEntityEventListener
{
    int created = 0;
    int destroyed = 0;

    /// @brief 生成イベントを記録する
    void on_entity_created(Entity) override
    {
        ++created;
    }
    /// @brief 破棄イベントを記録する
    void on_entity_destroyed(Entity) override
    {
        ++destroyed;
    }
};

/// @brief 型 ID の一貫性と採番の並行呼出しを検証する
int test_registry()
{
    const auto counter = cue::ecs::ComponentID<Counter>();
    if (counter != cue::tests::component_id_from_peer() || counter == cue::ecs::ComponentID<Step>())
    {
        return __LINE__;
    }
    std::array<cue::ecs::CompID, 8> ids{};
    std::vector<std::thread> threads;
    for (std::size_t index = 0; index < ids.size(); ++index)
    {
        threads.emplace_back([&, index]()
                             { ids[index] = cue::ecs::ECS_RegisterComponentID("Cue.ECS.Tests.Concurrent"); });
    }
    for (auto &thread : threads)
    {
        thread.join();
    }
    for (const auto id : ids)
    {
        if (id != ids.front())
        {
            return __LINE__;
        }
    }
    return 0;
}

/// @brief Entity 再利用、単一 Component、イベントと密な Pool の末尾移動を検証する
int test_entities_and_storage()
{
    ECSManager ecs;
    auto entities = std::make_shared<EntityEvents>();
    auto listener = std::make_shared<cue::ecs::IComponentEventListener>();
    listener->set_ecs_manager(&ecs);
    int added = 0;
    int copied = 0;
    int removed = 0;
    listener->register_on_add<Counter>([&](Entity, Counter *a_component)
                                       { added += a_component->initializations == 1 ? 1 : 100; });
    listener->register_on_copy<Counter>([&](Entity, Entity, Counter *a_component)
                                        { copied += a_component->value == 42 ? 1 : 100; });
    listener->register_on_remove<Counter>([&](Entity, Counter *) { ++removed; });
    ecs.add_entity_listener(entities);
    ecs.add_component_listener(listener);
    const auto first = ecs.generate_entity();
    const auto second = ecs.generate_entity();
    if (first == second || !ecs.is_entity_active(first) || ecs.get_component<Counter>(first))
    {
        return __LINE__;
    }
    ecs.add_component<Counter>(first)->value = 42;
    ecs.add_component<Counter>(second)->value = 7;
    const auto clone = ecs.copy_entity(first);
    if (added != 2 || copied != 1 || entities->created != 3 || ecs.get_component<Counter>(clone)->value != 42)
    {
        return __LINE__;
    }
    ecs.get_component<Counter>(clone)->value = 100;
    if (ecs.get_component<Counter>(first)->value != 42)
    {
        return __LINE__;
    }
    ecs.remove_component<Counter>(second);
    if (ecs.get_component<Counter>(second) || ecs.get_component<Counter>(clone)->value != 100)
    {
        return __LINE__;
    }
    ecs.set_entity_active(first, false);
    if (ecs.is_entity_active(first))
    {
        return __LINE__;
    }
    ecs.set_entity_active(first, true);
    ecs.remove_entity(first);
    if (ecs.is_entity_active(first) || entities->destroyed != 1 || removed != 2 || ecs.get_component<Counter>(first))
    {
        return __LINE__;
    }
    const auto recycled = ecs.generate_entity();
    if (recycled != first || !ecs.is_entity_active(recycled) || ecs.get_component<Counter>(recycled))
    {
        return __LINE__;
    }
    // 旧実装と同じ少ない予約容量から増設し、借用 Pointer を保持せず対応表を確認する
    ECSManager::ComponentPool<Counter> pool(2);
    for (Entity entity = 0; entity < 32; ++entity)
    {
        pool.add_component(entity)->value = static_cast<int>(entity * 3);
    }
    pool.remove_component(5);
    if (pool.get_component(5) || pool.get_component(31)->value != 93)
    {
        return __LINE__;
    }
    // Listener は weak_ptr。所有者が離れた後は呼び出さない
    listener.reset();
    ecs.add_component<Counter>(recycled);
    return added == 2 ? 0 : __LINE__;
}

/// @brief Context と優先度、各実行段階を検証する単一 Component System
class CounterSystem final : public ECSManager::System<Counter, Step>
{
  public:
    /// @brief Test の記録先を Manager の実行期間だけ借用する
    CounterSystem(int a_marker, std::vector<int> &a_order, std::array<int, 3> &a_lifecycle)
        : System(
              [&, a_marker](Entity, Counter &a_counter, Step &a_step, const cue::ecs::UpdateContext &a_context)
              {
                  a_order.push_back(a_marker);
                  a_counter.value += a_step.amount * static_cast<int>(a_context.bufferIndex);
              },
              [&](Entity, Counter &, Step &, const cue::ecs::InitializeContext &) { ++a_lifecycle[0]; },
              [&](Entity, Counter &, Step &, const cue::ecs::FinalizeContext &) { ++a_lifecycle[1]; },
              [&](Entity, Counter &, Step &, const cue::ecs::AwakeContext &) { ++a_lifecycle[2]; })
    {
    }
};

/// @brief System の段階・Query・Active・独立 Pipeline・計測 Callback を検証する
int test_systems_and_clock()
{
    std::chrono::nanoseconds now{};
    ECSManager ecs(
        [&]() noexcept
        {
            now += std::chrono::milliseconds(1);
            return now;
        });
    const auto entity = ecs.generate_entity();
    ecs.add_component<Counter>(entity);
    ecs.add_component<Step>(entity)->amount = 2;
    const auto counterOnly = ecs.generate_entity();
    ecs.add_component<Counter>(counterOnly);
    std::vector<int> order;
    std::array<int, 3> lifecycle{};
    auto &late = ecs.add_system<CounterSystem>(2, order, lifecycle);
    auto &early = ecs.add_system<CounterSystem>(1, order, lifecycle);
    late.set_priority(20);
    early.set_priority(10);
    ecs.awake_all_systems();
    ecs.initialize_all_systems();
    ecs.update_all_systems({3, 1.0f / 60.0f});
    if (order != std::vector<int>({1, 2}) || lifecycle[0] != 2 || lifecycle[2] != 2 ||
        ecs.get_component<Counter>(entity)->value != 12 || ecs.get_last_total_update_time_ms() != 5.0 ||
        ecs.get_last_system_update_time_ms<CounterSystem>() != 1.0)
    {
        return __LINE__;
    }
    order.clear();
    ecs.get_component<Step>(entity)->set_active(false);
    ecs.update_all_systems({3, 0.0f});
    if (!order.empty())
    {
        return __LINE__;
    }
    ecs.get_component<Step>(entity)->set_active(true);
    early.set_enabled(false);
    ECSManager::SystemPipeline pipeline;
    pipeline.add_system(&late);
    pipeline.add_system(&late);
    pipeline.update(ecs, {1, 0.01f});
    if (order != std::vector<int>({2}) || pipeline.empty() || !pipeline.contains_system(&late))
    {
        return __LINE__;
    }
    pipeline.remove_system(&late);
    if (!pipeline.empty())
    {
        return __LINE__;
    }
    early.set_enabled(true);
    ecs.finalize_all_systems();
    if (lifecycle[1] != 2 || ecs.get_last_total_initialize_time_ms() != 5.0 ||
        ecs.get_last_total_finalize_time_ms() != 5.0)
    {
        return __LINE__;
    }
    ecs.set_clock({});
    pipeline.update(ecs);
    return ecs.get_last_total_update_time_ms() == 0.0 ? 0 : __LINE__;
}

/// @brief 更新中の新規 Entity / Component と Deferred 削除の公開タイミングを検証する
int test_staging()
{
    ECSManager ecs;
    const auto seed = ecs.generate_entity();
    ecs.add_component<Counter>(seed);
    Entity created = (std::numeric_limits<Entity>::max)();
    bool hasRun = false;
    bool wasVisibleDuringUpdate = false;
    ecs.add_system<ECSManager::System<Counter>>(
        [&](Entity a_entity, Counter &)
        {
            if (hasRun)
            {
                return;
            }
            hasRun = true;
            created = ecs.generate_entity();
            ecs.add_component<Counter>(created)->value = 99;
            wasVisibleDuringUpdate = ecs.get_component<Counter>(created)->value == 99;
            ecs.remove_entity(a_entity);
        });
    ecs.update_all_systems();
    if (!wasVisibleDuringUpdate || ecs.is_entity_active(seed) || !ecs.is_entity_active(created) ||
        !ecs.get_component<Counter>(created) || ecs.get_component<Counter>(created)->value != 99)
    {
        return __LINE__;
    }
    ecs.update_all_systems();
    return ecs.get_component<Counter>(created)->value == 99 ? 0 : __LINE__;
}

/// @brief 複数 Component、Active View、Prototype の複製と子 Prefab を検証する
int test_multi_and_prototype()
{
    ECSManager ecs;
    const auto entity = ecs.generate_entity();
    ecs.add_component<Counter>(entity)->value = 17;
    ecs.add_component<Tag>(entity)->value = 1;
    ecs.add_component<Tag>(entity)->value = 2;
    ecs.add_component<SharedCounter>(entity);
    ecs.add_component<SharedCounter>(entity);
    ecs.get_all_components<SharedCounter>(entity)->back().set_active(false);
    ecs.add_system<ECSManager::MultiComponentSystem<SharedCounter>>(
        [](Entity, cue::ecs::componentView<SharedCounter> a_components)
        {
            for (auto *component : a_components)
            {
                ++*component->value;
            }
        });
    ecs.update_all_systems();
    auto *shared = ecs.get_all_components<SharedCounter>(entity);
    if (*shared->front().value != 1 || *shared->back().value != 0)
    {
        return __LINE__;
    }
    cue::ecs::Prototype::register_copy_func<Counter>();
    cue::ecs::Prototype::register_copy_func<Tag>();
    cue::ecs::Prototype::register_prefab_restore<Counter>();
    cue::ecs::Prototype::register_prefab_restore<Tag>();
    auto prototype = cue::ecs::Prototype::from_entity(ecs, entity);
    prototype.get_component<Counter>().value = 123;
    prototype.get_all_components<Tag>().front().value = 10;
    auto child = std::make_shared<cue::ecs::Prototype>();
    Counter childCounter;
    childCounter.value = 321;
    child->add_component(childCounter);
    prototype.add_sub_prefab(child);
    auto entities = std::make_shared<EntityEvents>();
    ecs.add_entity_listener(entities);
    const auto clone = prototype.instantiate(ecs);
    if (entities->created != 2 || ecs.get_component<Counter>(clone)->value != 123 ||
        ecs.get_component<Counter>(entity)->value != 17 || ecs.get_all_components<Tag>(clone)->size() != 2 ||
        ecs.get_all_components<Tag>(clone)->front().value != 10 ||
        ecs.get_all_components<Tag>(entity)->front().value != 1)
    {
        return __LINE__;
    }
    ecs.remove_component_instance<Tag>(clone, 0);
    if (ecs.get_all_components<Tag>(clone)->size() != 1 || ecs.get_all_components<Tag>(clone)->front().value != 2)
    {
        return __LINE__;
    }
    ecs.remove_all_components<Tag>(clone);
    if (ecs.get_all_components<Tag>(clone))
    {
        return __LINE__;
    }
    bool threwMissing = false;
    try
    {
        (void)prototype.get_component<Step>();
    }
    catch (const std::runtime_error &)
    {
        threwMissing = true;
    }
    return threwMissing ? 0 : __LINE__;
}
/// @brief 既存 Component の保持と Multi の値反映を回帰検証する
int test_fixed_behaviors()
{
    ECSManager ecs;
    const auto entity = ecs.generate_entity();
    ecs.add_component<Counter>(entity)->value = 42;
    ecs.add_system<ECSManager::System<Counter>>([&](Entity a_entity, Counter &) { ecs.add_component<Step>(a_entity); });
    ecs.update_all_systems();
    if (!ecs.get_component<Counter>(entity) || ecs.get_component<Counter>(entity)->value != 42 ||
        !ecs.get_component<Step>(entity))
    {
        return __LINE__;
    }
    ECSManager multi;
    const auto owner = multi.generate_entity();
    multi.add_component<Tag>(owner)->value = 5;
    multi.add_system<ECSManager::MultiComponentSystem<Tag>>([](Entity, cue::ecs::componentView<Tag> a_components)
                                                            { a_components.front()->value = 9; });
    multi.update_all_systems();
    return multi.get_all_components<Tag>(owner)->front().value == 9 ? 0 : __LINE__;
}
} // namespace

/// @brief 旧 ECS の格納・実行・通知・複製機能の移植を検証する
int main()
{
    const std::array tests{test_registry,
                           test_entities_and_storage,
                           test_systems_and_clock,
                           test_staging,
                           test_multi_and_prototype,
                           test_fixed_behaviors,
                           cue::tests::run_ecs_regressions};
    for (std::size_t index = 0; index < tests.size(); ++index)
    {
        try
        {
            const auto line = tests[index]();
            if (line != 0)
            {
                std::fprintf(stderr, "ECS test %zu failed at line %d\n", index, line);
                return line;
            }
        }
        catch (const std::exception &error)
        {
            std::fprintf(stderr, "ECS test %zu threw: %s\n", index, error.what());
            return 1;
        }
    }
    return 0;
}
