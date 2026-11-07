#include <SceneRuntime/SceneInstantiation.h>

#include <cstdio>
#include <thread>

#include <Runtime/Components/TransformComponent.h>
#include <Runtime/World.h>

#include "SceneFixtures.h"

/// @brief 同じ定義の二回実体化・独立 Transform・失敗時の既存 World 保全を確認する
int main()
{
    auto parsed = cue::Scene::parse(cue::tests::k_sceneText);
    auto created = cue::World::create();
    if (!parsed.has_value() || !created.has_value())
        return 1;
    auto world = created.take_value();
    auto first = cue::instantiate_scene(*parsed.try_value(), *world);
    auto second = cue::instantiate_scene(*parsed.try_value(), *world);
    if (!first.has_value() || !second.has_value() || *first.try_value() == *second.try_value())
        return 2;
    auto a = world->find_by_id(*first.try_value(), cue::tests::k_cubeId);
    auto b = world->find_by_name(*second.try_value(), "Cube");
    if (!a.has_value() || !b.has_value() || *a.try_value() == *b.try_value() || !world->initialize().has_value())
        return 3;
    auto tcA = world->get_component<cue::TransformComponent>(*a.try_value());
    auto tcB = world->get_component<cue::TransformComponent>(*b.try_value());
    if (!tcA.has_value() || !tcB.has_value())
        return 4;
    auto changed = (*tcB.try_value())->local();
    changed.position.x = 99;
    if (!(*tcB.try_value())->set_local(changed).has_value() ||
        !world->update(std::chrono::nanoseconds{1}).has_value() || (*tcA.try_value())->local().position.x != 1 ||
        (*tcB.try_value())->world_matrix().values[3][0] != 99)
        return 5;
    // Owner 以外の実体化は公開前に拒否し、既存 Instance を利用し続けられる
    bool rejected = false;
    std::thread other(
        [&]
        {
            auto result = cue::instantiate_scene(*parsed.try_value(), *world);
            rejected = !result.has_value() && result.try_error()->category == cue::ErrorCategory::WrongThread;
        });
    other.join();
    if (!rejected || !world->destroy_scene_instance(*first.try_value()).has_value())
        return 6;
    auto alive = world->is_alive(*b.try_value());
    if (!alive.has_value() || !*alive.try_value())
        return 7;
    // 実体化後は Scene 定義の寿命に依存しない。Running World への追加も独立する
    auto third = cue::instantiate_scene(*parsed.try_value(), *world);
    if (!third.has_value() || !world->update(std::chrono::nanoseconds{1}).has_value())
        return 8;
    return world->shutdown().has_value() ? 0 : 9;
}
