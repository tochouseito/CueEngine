#include <SceneRender/RenderScene.h>

#include <chrono>
#include <cstdio>
#include <thread>

#include <Runtime/Components/CameraComponent.h>
#include <Runtime/Components/MeshRendererComponent.h>
#include <Runtime/Components/TransformComponent.h>
#include <Runtime/World.h>
#include <SceneRuntime/SceneInstantiation.h>

namespace
{
#define CHECK(...)                                                                                                     \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(__VA_ARGS__))                                                                                            \
        {                                                                                                              \
            std::fprintf(stderr, "RenderScene test line %d\n", __LINE__);                                              \
            return false;                                                                                              \
        }                                                                                                              \
    } while (false)

/// @brief 旧形式の空の World は Camera がなくても Clear のみの Snapshot を作れる
bool verify_empty()
{
    auto created = cue::World::create();
    CHECK(created.has_value());
    auto world = created.take_value();
    CHECK(world->initialize().has_value());
    auto snapshot = cue::extract_render_scene(*world);
    CHECK(snapshot.has_value() && snapshot.try_value()->objects.empty() && !snapshot.try_value()->camera);
    CHECK(world->shutdown().has_value());
    return true;
}

/// @brief Scene 実体化・Camera 数の検証・Frame 所有値の独立性と Owner 制約を確認する
bool verify_extraction()
{
    cue::SceneDescriptor descriptor;
    descriptor.sceneId = "8d94c2b1-9b6a-435c-ade7-c01f473c3951";
    descriptor.name = "Render Scene";
    cue::SceneObjectDescriptor cube;
    cube.objectId = "9a00d731-7759-424c-9ade-fd086d624551";
    cube.name = "Cube";
    cube.transform = cue::math::floatTransform{};
    cube.mesh = cue::SceneMeshDescriptor{};
    cue::SceneObjectDescriptor camera;
    camera.objectId = "35f6ad62-88f0-49dc-b067-f318c116eb61";
    camera.name = "Camera";
    camera.transform = cue::math::floatTransform{};
    camera.transform->position.z = 3;
    camera.camera = cue::SceneCameraDescriptor{};
    descriptor.objects = {cube, camera};
    auto scene = cue::Scene::create(std::move(descriptor));
    auto created = cue::World::create();
    CHECK(scene.has_value() && created.has_value());
    auto world = created.take_value();
    auto instance = cue::instantiate_scene(*scene.try_value(), *world);
    CHECK(instance.has_value() && world->initialize().has_value());
    auto snapshot = cue::extract_render_scene(*world);
    CHECK(snapshot.has_value() && snapshot.try_value()->camera && snapshot.try_value()->objects.size() == 1);
    CHECK(snapshot.try_value()->objects[0].meshId == cue::k_cubeMeshId &&
          snapshot.try_value()->objects[0].world == cue::math::float4x4::identity());
    auto origin = cue::math::transform_point(cue::math::float3{}, snapshot.try_value()->camera->view);
    CHECK(origin.has_value() && origin.try_value()->z == -3);
    CHECK(snapshot.try_value()->camera->nearClip == 0.1f);
    auto independent = snapshot.take_value();

    auto cubeEntity = world->find_by_id(*instance.try_value(), cube.objectId);
    auto cameraEntity = world->find_by_id(*instance.try_value(), camera.objectId);
    CHECK(cubeEntity.has_value() && cameraEntity.has_value());
    auto transform = world->get_component<cue::TransformComponent>(*cubeEntity.try_value());
    CHECK(transform.has_value());
    auto moved = (*transform.try_value())->local();
    moved.position.x = 5;
    CHECK((*transform.try_value())->set_local(moved).has_value());
    CHECK(!cue::extract_render_scene(*world).has_value());
    CHECK(world->update(std::chrono::nanoseconds{1}).has_value());
    auto next = cue::extract_render_scene(*world);
    CHECK(next.has_value() && next.try_value()->objects[0].world.values[3][0] == 5 &&
          independent.objects[0].world.values[3][0] == 0);

    auto cameraTransform = world->get_component<cue::TransformComponent>(*cameraEntity.try_value());
    CHECK(cameraTransform.has_value());
    auto cameraMoved = (*cameraTransform.try_value())->local();
    cameraMoved.position.x = 1;
    CHECK((*cameraTransform.try_value())->set_local(cameraMoved).has_value());
    CHECK(!cue::extract_render_scene(*world).has_value());
    CHECK(world->update(std::chrono::nanoseconds{1}).has_value());
    auto nextCamera = cue::extract_render_scene(*world);
    CHECK(nextCamera.has_value() && nextCamera.try_value()->camera->view.values[3][0] == -1 &&
          independent.camera->view.values[3][0] == 0);

    CHECK(world->set_active(*cameraEntity.try_value(), false).has_value());
    auto missing = cue::extract_render_scene(*world);
    CHECK(!missing.has_value() && missing.try_error()->operation == "RenderScene.camera_count");
    CHECK(world->set_active(*cameraEntity.try_value(), true).has_value());
    auto extra = world->create_entity(*instance.try_value(), "extra-camera", "Extra Camera");
    CHECK(extra.has_value());
    CHECK(world->add_component<cue::TransformComponent>(*extra.try_value()).has_value());
    CHECK(world->add_component<cue::CameraComponent>(*extra.try_value()).has_value());
    CHECK(world->update(std::chrono::nanoseconds{1}).has_value());
    auto duplicated = cue::extract_render_scene(*world);
    CHECK(!duplicated.has_value() && duplicated.try_error()->operation == "RenderScene.camera_count");
    CHECK(world->destroy_entity(*extra.try_value()).has_value());
    CHECK(world->update(std::chrono::nanoseconds{1}).has_value());
    CHECK(cue::extract_render_scene(*world).has_value());
    bool rejected = false;
    std::thread other(
        [&]
        {
            auto result = cue::extract_render_scene(*world);
            rejected = !result.has_value() && result.try_error()->category == cue::ErrorCategory::WrongThread;
        });
    other.join();
    CHECK(rejected && world->update(std::chrono::nanoseconds{1}).has_value());
    CHECK(world->shutdown().has_value());
    // World の破棄後にも行列と Asset ID を所有し続ける
    world.reset();
    CHECK(independent.objects[0].meshId == cue::k_cubeMeshId && independent.camera->view.values[3][2] == -3);
    return true;
}
} // namespace

/// @brief World の抽出境界と Snapshot の値所有を GPU なしで検証する
int main()
{
    return verify_empty() && verify_extraction() ? 0 : 1;
}
