#include <Runtime/World.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <functional>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>

#include <Runtime/Components/TransformComponent.h>

namespace cue::tests
{
int g_initializations = 0;

/// @brief World の生成・削除時に Component 初期化が重複しないか観測する
struct WorldNumber final : ecs::IComponentTag
{
    int value = 0;
    /// @brief 追加時の初期化だけを数える
    void initialize() override
    {
        ++g_initializations;
    }
};

/// @brief World の Multi Component の借用と削除を確認する
struct WorldTag final : ecs::IComponentTag
{
    int value = 0;
};
} // namespace cue::tests

namespace cue::ecs
{
template <> struct IsMultiComponent<tests::WorldTag> : std::true_type
{
};
} // namespace cue::ecs

namespace
{
using cue::EntityHandle;
using cue::Result;
using cue::SceneInstanceHandle;
using cue::TransformComponent;
using cue::World;
using cue::WorldUpdateContext;
using cue::tests::WorldNumber;
using cue::tests::WorldTag;
using namespace std::chrono_literals;

#define CUE_WORLD_CHECK(...)                                                                                           \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(__VA_ARGS__))                                                                                            \
        {                                                                                                              \
            std::fprintf(stderr, "World test failed at line %d: %s\n", __LINE__, #__VA_ARGS__);                        \
            return __LINE__;                                                                                           \
        }                                                                                                              \
    } while (false)

/// @brief Lifecycle の順序と失敗を外部の観測値へ記録する
class CallbackSystem final : public cue::WorldSystem
{
  public:
    /// @brief Callback を所有する。捕捉した Test 値は World の停止まで生存させる
    CallbackSystem(std::function<Result<void>(World &, const WorldUpdateContext &)> a_update = {},
                   std::function<Result<void>(World &)> a_initialize = {},
                   std::function<Result<void>(World &)> a_shutdown = {})
        : m_update(std::move(a_update)), m_initialize(std::move(a_initialize)), m_shutdown(std::move(a_shutdown))
    {
    }
    /// @brief 初期化 Callback がなければ成功する
    Result<void> initialize(World &a_world) override
    {
        return m_initialize ? m_initialize(a_world) : Result<void>::success();
    }
    /// @brief 更新 Callback がなければ成功する
    Result<void> update(World &a_world, const WorldUpdateContext &a_context) override
    {
        return m_update ? m_update(a_world, a_context) : Result<void>::success();
    }
    /// @brief 部分初期化の失敗時も停止 Callback を実行する
    Result<void> shutdown(World &a_world) override
    {
        return m_shutdown ? m_shutdown(a_world) : Result<void>::success();
    }

  private:
    std::function<Result<void>(World &, const WorldUpdateContext &)> m_update;
    std::function<Result<void>(World &)> m_initialize;
    std::function<Result<void>(World &)> m_shutdown;
};

/// @brief 同じ Scene の ID / 名前を Instance 内へ閉じ、曖昧な名前・重複 ID を拒否する
int test_instances_and_handles()
{
    auto created = World::create();
    CUE_WORLD_CHECK(created.has_value());
    auto world = created.take_value();
    auto first = world->create_scene_instance("RotateCube.scene");
    auto second = world->create_scene_instance("RotateCube.scene");
    CUE_WORLD_CHECK(first.has_value() && second.has_value());
    const auto a = *first.try_value();
    const auto b = *second.try_value();
    auto cubeA = world->create_entity(a, "cube-id", "Cube");
    auto cubeB = world->create_entity(b, "cube-id", "Cube");
    CUE_WORLD_CHECK(cubeA.has_value() && cubeB.has_value() && *cubeA.try_value() != *cubeB.try_value());
    auto scoped = world->find_by_name(b, "Cube");
    CUE_WORLD_CHECK(scoped.has_value() && *scoped.try_value() == *cubeB.try_value());
    auto duplicate = world->create_entity(b, "cube-id", "Other");
    CUE_WORLD_CHECK(!duplicate.has_value());
    auto another = world->create_entity(b, "other-id", "Cube");
    CUE_WORLD_CHECK(another.has_value());
    auto ambiguous = world->find_by_name(b, "Cube");
    auto byId = world->find_by_id(b, "cube-id");
    CUE_WORLD_CHECK(!ambiguous.has_value() && ambiguous.try_error()->category == cue::ErrorCategory::InvalidState);
    CUE_WORLD_CHECK(byId.has_value() && *byId.try_value() == *cubeB.try_value());
    const auto stale = *cubeA.try_value();
    CUE_WORLD_CHECK(world->destroy_entity(stale).has_value());
    auto replacement = world->create_entity(a, "cube-id", "Cube");
    CUE_WORLD_CHECK(replacement.has_value() && replacement.try_value()->index == stale.index &&
                    replacement.try_value()->generation != stale.generation);
    auto staleAlive = world->is_alive(stale);
    CUE_WORLD_CHECK(staleAlive.has_value() && !*staleAlive.try_value());
    CUE_WORLD_CHECK(!world->add_component<WorldNumber>(stale).has_value());
    auto foreignCreated = World::create();
    CUE_WORLD_CHECK(foreignCreated.has_value());
    auto foreign = foreignCreated.take_value();
    CUE_WORLD_CHECK(!foreign->get_component<WorldNumber>(*replacement.try_value()).has_value());
    auto forged = *replacement.try_value();
    ++forged.generation;
    CUE_WORLD_CHECK(!world->destroy_entity(forged).has_value());
    CUE_WORLD_CHECK(world->destroy_scene_instance(b).has_value());
    auto aliveA = world->is_alive(*replacement.try_value());
    auto aliveB = world->is_alive(*cubeB.try_value());
    CUE_WORLD_CHECK(aliveA.has_value() && *aliveA.try_value() && aliveB.has_value() && !*aliveB.try_value());
    auto replacementScene = world->create_scene_instance("RotateCube.scene");
    CUE_WORLD_CHECK(replacementScene.has_value() && replacementScene.try_value()->index == b.index &&
                    replacementScene.try_value()->generation != b.generation);
    CUE_WORLD_CHECK(!world->find_by_id(b, "cube-id").has_value());
    CUE_WORLD_CHECK(!world->create_scene_instance(std::string_view("bad\0id", 6)).has_value());
    CUE_WORLD_CHECK(!world->create_entity(a, "", "Name").has_value());
    CUE_WORLD_CHECK(world->shutdown().has_value() && world->shutdown().has_value());
    auto stopped = world->is_alive(*replacement.try_value());
    CUE_WORLD_CHECK(stopped.has_value() && !*stopped.try_value());
    return 0;
}

/// @brief Instance ごとの Transform 更新・行列同期・無効値の非破壊性を確認する
int test_transform_and_systems()
{
    auto created = World::create();
    CUE_WORLD_CHECK(created.has_value());
    auto world = created.take_value();
    auto a = world->create_scene_instance("Sample");
    auto b = world->create_scene_instance("Sample");
    CUE_WORLD_CHECK(a.has_value() && b.has_value());
    auto first = world->create_entity(*a.try_value(), "cube", "Cube");
    auto second = world->create_entity(*b.try_value(), "cube", "Cube");
    CUE_WORLD_CHECK(first.has_value() && second.has_value());
    CUE_WORLD_CHECK(world->add_component<TransformComponent>(*first.try_value()).has_value());
    CUE_WORLD_CHECK(world->add_component<TransformComponent>(*second.try_value()).has_value());
    bool timeCorrect = false;
    auto system = std::make_unique<CallbackSystem>(
        [&](World &a_world, const WorldUpdateContext &a_context)
        {
            timeCorrect = a_context.deltaTime == 500ms && a_context.tick == 1;
            auto target = a_world.find_by_id(*b.try_value(), "cube");
            if (!target.has_value())
                return Result<void>::failure(*target.try_error());
            auto component = a_world.get_component<TransformComponent>(*target.try_value());
            if (!component.has_value())
                return Result<void>::failure(*component.try_error());
            auto value = (*component.try_value())->local();
            value.position = {10, 20, 30};
            auto rotation = cue::math::rotation_axis_angle(cue::math::float3{0, 0, 1}, cue::math::k_pi<float> / 2.0f);
            if (!rotation.has_value())
                return Result<void>::failure(*rotation.try_error());
            value.rotation = *rotation.try_value();
            return (*component.try_value())->set_local(value);
        });
    CUE_WORLD_CHECK(world->add_system(std::move(system)).has_value());
    CUE_WORLD_CHECK(world->initialize().has_value());
    CUE_WORLD_CHECK(world->update(500ms).has_value() && timeCorrect);
    auto transformA = world->get_component<TransformComponent>(*first.try_value());
    auto transformB = world->get_component<TransformComponent>(*second.try_value());
    CUE_WORLD_CHECK(transformA.has_value() && transformB.has_value());
    CUE_WORLD_CHECK((*transformA.try_value())->world_matrix() == cue::math::float4x4{});
    CUE_WORLD_CHECK(!(*transformB.try_value())->is_dirty());
    auto point = cue::math::transform_point(cue::math::float3{1, 0, 0}, (*transformB.try_value())->world_matrix());
    CUE_WORLD_CHECK(point.has_value() &&
                    cue::math::nearly_equal(*point.try_value(), cue::math::float3{10, 21, 30}, 1e-5f));
    auto invalid = (*transformB.try_value())->local();
    invalid.position.x = std::numeric_limits<float>::quiet_NaN();
    CUE_WORLD_CHECK(!(*transformB.try_value())->set_local(invalid).has_value());
    CUE_WORLD_CHECK((*transformB.try_value())->local().position.x == 10 && !(*transformB.try_value())->is_dirty());
    CUE_WORLD_CHECK(!world->update(-1ns).has_value());
    CUE_WORLD_CHECK(!world->initialize().has_value());
    CUE_WORLD_CHECK(world->shutdown().has_value());
    return 0;
}

/// @brief 同順位の登録順・停止の逆順・初期化を開始していない System の扱いを確認する
int test_system_order()
{
    std::vector<int> order;
    auto created = World::create();
    CUE_WORLD_CHECK(created.has_value());
    auto world = created.take_value();
    for (const auto [id, priority] : {std::pair{1, 20}, std::pair{2, 5}, std::pair{3, 20}})
    {
        auto system = std::make_unique<CallbackSystem>(
            [&, id](World &, const WorldUpdateContext &)
            {
                order.push_back(id + 10);
                return Result<void>::success();
            },
            [&, id](World &)
            {
                order.push_back(id);
                return Result<void>::success();
            },
            [&, id](World &)
            {
                order.push_back(id + 20);
                return Result<void>::success();
            });
        CUE_WORLD_CHECK(world->add_system(std::move(system), priority).has_value());
    }
    CUE_WORLD_CHECK(!world->update(1ms).has_value());
    CUE_WORLD_CHECK(world->initialize().has_value() && world->update(1ms).has_value());
    CUE_WORLD_CHECK(!world->add_system(std::make_unique<CallbackSystem>()).has_value());
    CUE_WORLD_CHECK(world->shutdown().has_value() && world->shutdown().has_value());
    CUE_WORLD_CHECK(order == std::vector<int>{2, 1, 3, 12, 11, 13, 23, 21, 22});
    return 0;
}

/// @brief Query の借用中は物理削除せず、予約追加を次の確定後から公開する
int test_deferred_mutations()
{
    auto created = World::create();
    CUE_WORLD_CHECK(created.has_value());
    auto world = created.take_value();
    auto instance = world->create_scene_instance("Scene");
    CUE_WORLD_CHECK(instance.has_value());
    auto victim = world->create_entity(*instance.try_value(), "victim", "Victim");
    auto keeper = world->create_entity(*instance.try_value(), "keeper", "Keeper");
    CUE_WORLD_CHECK(victim.has_value() && keeper.has_value());
    CUE_WORLD_CHECK(world->add_component<WorldNumber>(*victim.try_value()).has_value());
    CUE_WORLD_CHECK(world->add_component<WorldNumber>(*keeper.try_value()).has_value());
    const auto initializations = cue::tests::g_initializations;
    bool sawSafeReference = false;
    bool directRejected = false;
    int nested = 0;
    EntityHandle spawned{};
    bool ran = false;
    auto system = std::make_unique<CallbackSystem>(
        [&](World &a_world, const WorldUpdateContext &)
        {
            if (ran)
                return Result<void>::success();
            ran = true;
            directRejected = !a_world.create_scene_instance("Direct").has_value() && !a_world.shutdown().has_value() &&
                             !a_world.update(1ms).has_value();
            return a_world.for_each<WorldNumber>(
                [&](EntityHandle a_entity, WorldNumber &a_number)
                {
                    ++a_number.value;
                    if (a_entity != *victim.try_value())
                        return Result<void>::success();
                    directRejected &= !a_world.add_component<WorldTag>(a_entity).has_value();
                    auto removed = a_world.remove_component<WorldNumber>(a_entity);
                    if (!removed.has_value())
                        return removed;
                    auto destroyed = a_world.destroy_entity(a_entity);
                    if (!destroyed.has_value())
                        return destroyed;
                    auto alive = a_world.is_alive(a_entity);
                    sawSafeReference = alive.has_value() && !*alive.try_value() && a_number.value == 1;
                    return a_world.defer(
                        [&](World &a_deferred)
                        {
                            auto object = a_deferred.create_entity(*instance.try_value(), "spawned", "Spawned");
                            if (!object.has_value())
                                return Result<void>::failure(*object.try_error());
                            spawned = *object.try_value();
                            auto number = a_deferred.add_component<WorldNumber>(spawned);
                            if (!number.has_value())
                                return Result<void>::failure(*number.try_error());
                            (*number.try_value())->value = 77;
                            return a_deferred.defer(
                                [&](World &)
                                {
                                    ++nested;
                                    return Result<void>::success();
                                });
                        });
                });
        });
    CUE_WORLD_CHECK(world->add_system(std::move(system)).has_value());
    CUE_WORLD_CHECK(world->initialize().has_value() && world->update(16ms).has_value());
    CUE_WORLD_CHECK(sawSafeReference && directRejected && nested == 1);
    auto newNumber = world->get_component<WorldNumber>(spawned);
    auto keptNumber = world->get_component<WorldNumber>(*keeper.try_value());
    CUE_WORLD_CHECK(newNumber.has_value() && (*newNumber.try_value())->value == 77);
    CUE_WORLD_CHECK(keptNumber.has_value() && (*keptNumber.try_value())->value == 1);
    CUE_WORLD_CHECK(spawned.index == victim.try_value()->index && spawned.generation != victim.try_value()->generation);
    CUE_WORLD_CHECK(cue::tests::g_initializations == initializations + 1);
    CUE_WORLD_CHECK(world->shutdown().has_value());
    return 0;
}

/// @brief Multi の借用、Inactive Query、Instance 単位の遅延破棄を確認する
int test_multi_and_instance_retirement()
{
    auto created = World::create();
    CUE_WORLD_CHECK(created.has_value());
    auto world = created.take_value();
    auto instance = world->create_scene_instance("Scene");
    CUE_WORLD_CHECK(instance.has_value());
    auto first = world->create_entity(*instance.try_value(), "a", "A");
    auto second = world->create_entity(*instance.try_value(), "b", "B");
    CUE_WORLD_CHECK(first.has_value() && second.has_value());
    CUE_WORLD_CHECK(world->add_component<WorldTag>(*first.try_value()).has_value());
    CUE_WORLD_CHECK(world->add_component<WorldTag>(*first.try_value()).has_value());
    CUE_WORLD_CHECK(world->add_component<WorldNumber>(*first.try_value()).has_value());
    CUE_WORLD_CHECK(world->add_component<WorldNumber>(*second.try_value()).has_value());
    auto tags = world->get_components<WorldTag>(*first.try_value());
    CUE_WORLD_CHECK(tags.has_value() && tags.try_value()->size() == 2);
    (*tags.try_value())[0].value = 12;
    (*tags.try_value())[1].value = 34;
    CUE_WORLD_CHECK(world->set_active(*second.try_value(), false).has_value());
    int updates = 0;
    CUE_WORLD_CHECK(world
                        ->for_each<WorldNumber>(
                            [&](EntityHandle, WorldNumber &)
                            {
                                ++updates;
                                return Result<void>::success();
                            })
                        .has_value());
    CUE_WORLD_CHECK(updates == 1);
    CUE_WORLD_CHECK(world->initialize().has_value());
    CUE_WORLD_CHECK(world
                        ->for_each<WorldNumber>(
                            [&](EntityHandle, WorldNumber &)
                            {
                                auto result = world->destroy_scene_instance(*instance.try_value());
                                auto alive = world->is_alive(*instance.try_value());
                                if (!result.has_value() || !alive.has_value() || *alive.try_value())
                                    return Result<void>::failure(
                                        {cue::ErrorCategory::InvalidState, "test.instance_retirement"});
                                return Result<void>::success();
                            })
                        .has_value());
    auto beforeCommit = world->is_alive(*first.try_value());
    CUE_WORLD_CHECK(beforeCommit.has_value() && !*beforeCommit.try_value());
    CUE_WORLD_CHECK(world->update(1ms).has_value());
    auto another = world->create_scene_instance("Scene");
    CUE_WORLD_CHECK(another.has_value() && another.try_value()->generation != instance.try_value()->generation);
    CUE_WORLD_CHECK(world->shutdown().has_value());
    return 0;
}

/// @brief 部分初期化・Callback 例外・停止失敗でも回収し、最初の原因を維持する
int test_failure_cleanup()
{
    for (const bool throwDuringUpdate : {false, true})
    {
        std::vector<int> stopped;
        bool cleanupRead = false;
        bool laterUpdated = false;
        auto created = World::create();
        CUE_WORLD_CHECK(created.has_value());
        auto world = created.take_value();
        auto instance = world->create_scene_instance("Scene");
        CUE_WORLD_CHECK(instance.has_value());
        auto entity = world->create_entity(*instance.try_value(), "id", "Name");
        CUE_WORLD_CHECK(entity.has_value() && world->add_component<WorldNumber>(*entity.try_value()).has_value());
        auto first = std::make_unique<CallbackSystem>(
            std::function<Result<void>(World &, const WorldUpdateContext &)>{}, std::function<Result<void>(World &)>{},
            [&](World &a_world)
            {
                stopped.push_back(1);
                cleanupRead = a_world.get_component<WorldNumber>(*entity.try_value()).has_value();
                return Result<void>::success();
            });
        auto failing = std::make_unique<CallbackSystem>(
            [&](World &, const WorldUpdateContext &) -> Result<void> { throw std::runtime_error("test failure"); },
            [&](World &)
            {
                return throwDuringUpdate
                           ? Result<void>::success()
                           : Result<void>::failure({cue::ErrorCategory::InvalidState, "test.initialize.failure"});
            },
            [&](World &)
            {
                stopped.push_back(2);
                return Result<void>::failure({cue::ErrorCategory::InvalidState, "test.shutdown.failure"});
            });
        auto later = std::make_unique<CallbackSystem>(
            [&](World &, const WorldUpdateContext &)
            {
                laterUpdated = true;
                return Result<void>::success();
            },
            std::function<Result<void>(World &)>{},
            [&](World &)
            {
                stopped.push_back(3);
                return Result<void>::success();
            });
        CUE_WORLD_CHECK(world->add_system(std::move(first)).has_value());
        CUE_WORLD_CHECK(world->add_system(std::move(failing)).has_value());
        CUE_WORLD_CHECK(world->add_system(std::move(later)).has_value());
        auto initialized = world->initialize();
        if (throwDuringUpdate)
        {
            CUE_WORLD_CHECK(initialized.has_value());
            CUE_WORLD_CHECK(!world->update(1ms).has_value());
        }
        else
            CUE_WORLD_CHECK(!initialized.has_value());
        CUE_WORLD_CHECK(!world->update(1ms).has_value() && !laterUpdated);
        auto shutdown = world->shutdown();
        CUE_WORLD_CHECK(!shutdown.has_value() && cleanupRead);
        CUE_WORLD_CHECK(shutdown.try_error()->operation ==
                        (throwDuringUpdate ? "World.update.exception" : "test.initialize.failure"));
        CUE_WORLD_CHECK(stopped == (throwDuringUpdate ? std::vector<int>{3, 2, 1} : std::vector<int>{2, 1}));
        auto again = world->shutdown();
        CUE_WORLD_CHECK(!again.has_value() && stopped.size() == (throwDuringUpdate ? 3u : 2u));
    }
    return 0;
}

/// @brief 別 Thread の操作を拒否し、無限に増える予約を上限で停止する
int test_thread_and_deferred_limit()
{
    auto created = World::create();
    CUE_WORLD_CHECK(created.has_value());
    auto world = created.take_value();
    bool rejected = false;
    std::thread other(
        [&]
        {
            auto result = world->create_scene_instance("WrongThread");
            rejected = !result.has_value() && result.try_error()->category == cue::ErrorCategory::WrongThread;
        });
    other.join();
    CUE_WORLD_CHECK(rejected);
    int count = 0;
    std::function<Result<void>(World &)> repeat;
    repeat = [&](World &a_world)
    {
        ++count;
        return a_world.defer(repeat);
    };
    CUE_WORLD_CHECK(world->defer(repeat).has_value());
    auto initialized = world->initialize();
    CUE_WORLD_CHECK(!initialized.has_value() && initialized.try_error()->operation == "World.deferred_limit" &&
                    count == 4096);
    auto stopped = world->shutdown();
    CUE_WORLD_CHECK(!stopped.has_value());
    return 0;
}
/// @brief 更新失敗後の停止処理でも、全 Entity の回収走査を途中で打ち切らない
int test_failed_world_shutdown_query()
{
    int cleaned = 0;
    auto created = World::create();
    CUE_WORLD_CHECK(created.has_value());
    auto world = created.take_value();
    auto instance = world->create_scene_instance("Scene");
    CUE_WORLD_CHECK(instance.has_value());
    for (const auto *name : {"a", "b", "c"})
    {
        auto entity = world->create_entity(*instance.try_value(), name, name);
        CUE_WORLD_CHECK(entity.has_value() && world->add_component<WorldNumber>(*entity.try_value()).has_value());
    }
    auto system = std::make_unique<CallbackSystem>(
        [](World &, const WorldUpdateContext &)
        { return Result<void>::failure({cue::ErrorCategory::InvalidState, "test.update.failure"}); },
        std::function<Result<void>(World &)>{},
        [&](World &a_world)
        {
            return a_world.for_each<WorldNumber>(
                [&](EntityHandle, WorldNumber &)
                {
                    ++cleaned;
                    return Result<void>::success();
                });
        });
    CUE_WORLD_CHECK(world->add_system(std::move(system)).has_value());
    CUE_WORLD_CHECK(world->initialize().has_value() && !world->update(1ms).has_value());
    auto stopped = world->shutdown();
    CUE_WORLD_CHECK(!stopped.has_value() && stopped.try_error()->operation == "test.update.failure" && cleaned == 3);
    return 0;
}
#undef CUE_WORLD_CHECK
} // namespace

/// @brief World の所有・Instance・世代・遅延更新・失敗回収を検証する
int main()
{
    const std::array tests{test_instances_and_handles,     test_transform_and_systems,         test_system_order,
                           test_deferred_mutations,        test_multi_and_instance_retirement, test_failure_cleanup,
                           test_thread_and_deferred_limit, test_failed_world_shutdown_query};
    for (const auto test : tests)
    {
        try
        {
            if (test() != 0)
                return 1;
        }
        catch (const std::exception &error)
        {
            std::fprintf(stderr, "World test exception: %s\n", error.what());
            return 1;
        }
    }
    return 0;
}
