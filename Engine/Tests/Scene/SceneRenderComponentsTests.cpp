#include <SceneRuntime/SceneInstantiation.h>

#include <cstdio>
#include <string>
#include <utility>

#include <Runtime/Components/CameraComponent.h>
#include <Runtime/Components/MeshRendererComponent.h>
#include <Runtime/Components/TransformComponent.h>
#include <Runtime/World.h>

#include "SceneFixtures.h"

namespace
{
#define CHECK(...)                                                                                                     \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(__VA_ARGS__))                                                                                            \
        {                                                                                                              \
            std::fprintf(stderr, "Scene render components line %d\n", __LINE__);                                       \
            return false;                                                                                              \
        }                                                                                                              \
    } while (false)

/// @brief Camera / Mesh の Schema を復元し、未知・部分設定・Transform 欠落を拒否する
bool verify_components()
{
    const std::string text =
        std::string(cue::tests::k_sceneText) +
        "camera.projection=perspective\ncamera.fovYRadians=1.04719755\ncamera.near=0.1\ncamera.far=1000\n"
        "mesh.assetId=cue://engine/mesh/cube\n";
    auto scene = cue::Scene::parse(text);
    CHECK(scene.has_value());
    const auto &object = scene.try_value()->descriptor().objects[0];
    CHECK(object.camera && object.mesh && object.camera->nearClip == 0.1f && object.mesh->assetId == cue::k_cubeMeshId);
    auto serialized = scene.try_value()->serialize();
    CHECK(serialized.has_value());
    auto decoded = cue::Scene::parse(*serialized.try_value());
    CHECK(decoded.has_value());
    const auto &restored = decoded.try_value()->descriptor().objects[0];
    CHECK(restored.camera && restored.mesh &&
          restored.camera->verticalFovRadians == object.camera->verticalFovRadians &&
          restored.camera->nearClip == object.camera->nearClip && restored.camera->farClip == object.camera->farClip &&
          restored.mesh->assetId == object.mesh->assetId);
    for (const auto &[from, to] :
         {std::pair{"perspective", "orthographic"}, std::pair{"camera.near=0.1", "camera.near=0"},
          std::pair{"camera.far=1000", "camera.far=0.01"},
          std::pair{"camera.fovYRadians=1.04719755", "camera.fovYRadians=nan"}, std::pair{"camera.far=1000", ""},
          std::pair{"mesh/cube", "mesh/missing"}})
    {
        std::string invalid(text);
        const auto position = invalid.find(from);
        CHECK(position != std::string::npos);
        invalid.replace(position, std::char_traits<char>::length(from), to);
        CHECK(!cue::Scene::parse(invalid).has_value());
    }
    auto noTransform = scene.try_value()->descriptor();
    noTransform.objects[0].transform.reset();
    CHECK(!cue::Scene::create(std::move(noTransform)).has_value());
    auto duplicate = text + "camera.near=1\n";
    CHECK(!cue::Scene::parse(duplicate).has_value());

    auto created = cue::World::create();
    CHECK(created.has_value());
    auto world = created.take_value();
    auto instance = cue::instantiate_scene(*scene.try_value(), *world);
    CHECK(instance.has_value() && world->initialize().has_value());
    auto entity = world->find_by_id(*instance.try_value(), cue::tests::k_cubeId);
    CHECK(entity.has_value());
    auto camera = world->get_component<cue::CameraComponent>(*entity.try_value());
    auto mesh = world->get_component<cue::MeshRendererComponent>(*entity.try_value());
    CHECK(camera.has_value() && mesh.has_value());
    CHECK((*camera.try_value())->projection().nearClip == object.camera->nearClip &&
          (*mesh.try_value())->mesh_id() == cue::k_cubeMeshId);
    CHECK(world->shutdown().has_value());
    return true;
}
} // namespace

/// @brief Scene の描画 Component と World への実体化を Renderer なしで確認する
int main()
{
    return verify_components() ? 0 : 1;
}
