#include <Runtime/World.h>

#include <cstddef>
#include <utility>

namespace
{
/// @brief Global 型登録の上限外を、別 Process の Test で他の Test 型と独立して作る
template <std::size_t Index> struct CapacityComponent final : cue::ecs::IComponentTag
{
};

/// @brief Storage を生成せずに異なる型 ID だけを登録する
template <std::size_t... Index> void register_types(std::index_sequence<Index...>)
{
    ((void)cue::ecs::ComponentID<CapacityComponent<Index>>(), ...);
}
} // namespace

/// @brief Transform 未使用の World は、Global 型 ID 上限に到達しても更新できる
int main()
{
    register_types(std::make_index_sequence<260>{});
    auto created = cue::World::create();
    if (!created.has_value())
        return 1;
    auto world = created.take_value();
    auto instance = world->create_scene_instance("Scene");
    if (!instance.has_value())
        return 2;
    auto entity = world->create_entity(*instance.try_value(), "id", "Object");
    if (!entity.has_value())
        return 3;
    auto component = world->add_component<CapacityComponent<259>>(*entity.try_value());
    if (component.has_value() || component.try_error()->operation != "World.component_type_capacity")
        return 4;
    if (!world->initialize().has_value() || !world->update(std::chrono::nanoseconds{1}).has_value())
        return 5;
    return world->shutdown().has_value() ? 0 : 6;
}
