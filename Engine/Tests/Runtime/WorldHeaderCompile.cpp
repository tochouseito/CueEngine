#include <Runtime/Components/CameraComponent.h>
#include <Runtime/Components/MeshRendererComponent.h>
#include <Runtime/Components/TransformComponent.h>
#include <Runtime/World.h>
#include <Runtime/WorldHandle.h>
#include <Runtime/WorldSystem.h>

#include <type_traits>

static_assert(!std::is_copy_constructible_v<cue::World> && !std::is_move_constructible_v<cue::World>);
static_assert(std::is_trivially_copyable_v<cue::EntityHandle>);
static_assert(std::is_trivially_copyable_v<cue::SceneInstanceHandle>);
static_assert(std::is_base_of_v<cue::ecs::IComponentTag, cue::TransformComponent>);
static_assert(std::is_base_of_v<cue::ecs::IComponentTag, cue::CameraComponent>);
static_assert(std::is_base_of_v<cue::ecs::IComponentTag, cue::MeshRendererComponent>);

/// @brief World の公開契約を Window / DX12 / Editor なしで Compile する
int main()
{
    auto created = cue::World::create();
    return created.has_value() ? 0 : 1;
}
