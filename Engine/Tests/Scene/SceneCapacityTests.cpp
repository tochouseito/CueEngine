#include <SceneRuntime/SceneInstantiation.h>

#include <cstddef>
#include <utility>

#include <Runtime/World.h>

#include "SceneFixtures.h"

namespace
{
template <std::size_t Index> struct CapacityTag final : cue::ecs::IComponentTag
{
};
/// @brief 型容量不足の拒否を別 Process で検証する
template <std::size_t... Index> void fill_types(std::index_sequence<Index...>)
{
    ((void)cue::ecs::ComponentID<CapacityTag<Index>>(), ...);
}
} // namespace

/// @brief 実体化前の型容量拒否で、既存の World / Entity を失敗状態へしない
int main()
{
    fill_types(std::make_index_sequence<260>{});
    auto scene = cue::Scene::parse(cue::tests::k_sceneText);
    auto created = cue::World::create();
    if (!scene.has_value() || !created.has_value())
        return 1;
    auto world = created.take_value();
    auto original = world->create_scene_instance("Existing");
    if (!original.has_value())
        return 2;
    auto entity = world->create_entity(*original.try_value(), "keep", "Keep");
    auto result = cue::instantiate_scene(*scene.try_value(), *world);
    if (!entity.has_value() || result.has_value() ||
        result.try_error()->operation != "SceneInstantiation.component_capacity")
        return 3;
    auto alive = world->is_alive(*entity.try_value());
    if (!alive.has_value() || !*alive.try_value() || !world->initialize().has_value())
        return 4;
    return world->shutdown().has_value() ? 0 : 5;
}
