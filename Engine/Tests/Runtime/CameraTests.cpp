#include <Runtime/Components/CameraComponent.h>
#include <Runtime/Components/MeshRendererComponent.h>
#include <Runtime/Components/TransformComponent.h>
#include <Runtime/World.h>

#include <cstdio>

/// @brief Camera / Mesh の設定検証、World 上の所有と Query、Transform との分離を確認する
int main()
{
    auto created = cue::World::create();
    if (!created.has_value())
        return 1;
    auto world = created.take_value();
    auto instance = world->create_scene_instance("CameraTest");
    if (!instance.has_value())
        return 2;
    auto entity = world->create_entity(*instance.try_value(), "camera", "Camera");
    if (!entity.has_value())
        return 3;
    auto transform = world->add_component<cue::TransformComponent>(*entity.try_value());
    auto camera = world->add_component<cue::CameraComponent>(*entity.try_value());
    auto mesh = world->add_component<cue::MeshRendererComponent>(*entity.try_value());
    if (!transform.has_value() || !camera.has_value() || !mesh.has_value())
        return 4;
    const auto original = (*camera.try_value())->projection();
    auto invalid = original;
    invalid.nearClip = invalid.farClip;
    if ((*camera.try_value())->set_projection(invalid).has_value() ||
        (*camera.try_value())->projection().nearClip != original.nearClip ||
        (*camera.try_value())->projection_matrix(0).has_value() ||
        !(*camera.try_value())->projection_matrix(16.0f / 9.0f).has_value())
        return 5;
    if ((*mesh.try_value())->set_mesh_id("cue://engine/mesh/unknown").has_value() ||
        (*mesh.try_value())->mesh_id() != cue::k_cubeMeshId ||
        !(*mesh.try_value())->set_mesh_id(cue::k_cubeMeshId).has_value())
        return 6;
    auto local = (*transform.try_value())->local();
    local.position.z = 3;
    if (!(*transform.try_value())->set_local(local).has_value() || !world->initialize().has_value())
        return 7;
    unsigned count = 0;
    auto query = world->for_each<cue::TransformComponent, cue::CameraComponent>(
        [&count](cue::EntityHandle, cue::TransformComponent &a_transform, cue::CameraComponent &a_camera)
        {
            ++count;
            auto view = cue::math::view_from_transform(a_transform.local());
            auto projection = a_camera.projection_matrix(1.0f);
            return view.has_value() && projection.has_value()
                       ? cue::Result<void>::success()
                       : cue::Result<void>::failure({cue::ErrorCategory::InvalidState, "CameraTests.query"});
        });
    if (!query.has_value() || count != 1)
        return 8;
    return world->shutdown().has_value() ? 0 : 9;
}
