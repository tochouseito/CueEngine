#include <EditorHost/EditorHost.h>

#include <chrono>
#include <cstdio>
#include <functional>
#include <string>
#include <thread>
#include <utility>

#include <DX12/DX12Backend.h>
#include <DX12/DX12RenderDevice.h>
#include <Runtime/Components/TransformComponent.h>
#include <Runtime/World.h>

namespace
{
constexpr const char *k_cubeId = "9a00d731-7759-424c-9ade-fd086d624551";

#define CUE_EMBEDDED_CHECK(...)                                                                                        \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(__VA_ARGS__))                                                                                            \
        {                                                                                                              \
            std::fprintf(stderr, "Embedded Play failed at %d: %s\n", __LINE__, #__VA_ARGS__);                          \
            return __LINE__;                                                                                           \
        }                                                                                                              \
    } while (false)

/// @brief 未保存の Cube / Camera / 障害注入 Script を持つ Scene を作る
cue::Result<cue::Scene> make_scene(double a_mode)
{
    cue::SceneDescriptor descriptor;
    descriptor.sceneId = "8d94c2b1-9b6a-435c-ade7-c01f473c3951";
    descriptor.name = "Embedded Play Test";
    cue::SceneObjectDescriptor cube;
    cube.objectId = k_cubeId;
    cube.name = "Cube";
    cube.transform.emplace();
    cube.mesh.emplace();
    cue::ScriptDescriptor script;
    script.instanceId = "6c78eb99-aee8-4602-9fba-66fa96080f99";
    script.typeId = "test.Failure";
    script.properties.push_back({"mode", cue::ScriptPropertyKind::Number, a_mode, false});
    cube.scripts.push_back(std::move(script));
    descriptor.objects.push_back(std::move(cube));
    cue::SceneObjectDescriptor camera;
    camera.objectId = "35f6ad62-88f0-49dc-b067-f318c116eb61";
    camera.name = "Camera";
    camera.transform.emplace();
    camera.transform->position = {0, 0, 3};
    camera.camera.emplace();
    descriptor.objects.push_back(std::move(camera));
    return cue::Scene::create(std::move(descriptor));
}

/// @brief Editor の描画と Pipe を進め、子の停止・障害でも Editor step 自体が成功することを確認する
bool wait_editor(cue::EditorHost &a_host, const std::function<bool()> &a_ready)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
    while (std::chrono::steady_clock::now() < deadline)
    {
        auto stepped = a_host.step();
        if (!stepped.has_value() || !*stepped.try_value())
        {
            std::fprintf(stderr, "Editor step: %s\n",
                         stepped.has_value() ? "closed" : stepped.try_error()->operation.c_str());
            return false;
        }
        if (a_ready())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::fprintf(stderr, "Editor wait timeout, Play state %d\n", int(a_host.play_state()));
    return false;
}

/// @brief Editor 表示・実行姿勢・編集正本の分離と、子の Crash / Hang / 描画不正後の継続を確認する
int test_editor(const cue::Path &a_executable, const cue::Path &a_module, double a_mode)
{
    auto scene = make_scene(a_mode);
    CUE_EMBEDDED_CHECK(scene.has_value());
    auto original = scene.try_value()->serialize();
    CUE_EMBEDDED_CHECK(original.has_value());
    cue::EditorHostConfig config;
    config.window = {"CueEngine Embedded Play Test", {640, 480}};
    config.frame = {1, false, 60};
    config.initialScene = scene.take_value();
    config.playExecutable = a_executable;
    config.scriptModule = a_module;
    config.gameConfiguration = cue::current_game_build_configuration();
    config.imgui.settingsFile.clear();
    config.logging.isFileEnabled = false;
    config.buildUi = [] { return cue::Result<void>::success(); };
    config.createBackend = [](cue::IFileSystem *a_files, cue::Clock &a_clock)
    {
        auto backend = cue::dx12::DX12Backend::create(cue::dx12::AdapterSelection::Warp, {}, a_files, &a_clock);
        return backend.has_value() ? cue::Result<std::unique_ptr<cue::IBackend>>::success(backend.take_value())
                                   : cue::Result<std::unique_ptr<cue::IBackend>>::failure(*backend.try_error());
    };
    cue::EditorHost host(std::move(config));
    CUE_EMBEDDED_CHECK(host.initialize().has_value());
    CUE_EMBEDDED_CHECK(wait_editor(host, [&] { return host.play_state() == cue::PlayState::Stopped; }));
    CUE_EMBEDDED_CHECK(host.start_play().has_value());
    CUE_EMBEDDED_CHECK(wait_editor(host, [&] { return host.play_view_snapshot() != nullptr; }));
    CUE_EMBEDDED_CHECK(host.play_state() == cue::PlayState::Running);
    CUE_EMBEDDED_CHECK(host.play_view_snapshot()->render.objects.size() == 1);

    if (a_mode == 6)
    {
        const auto initialTick = host.play_view_snapshot()->tick;
        const auto initialMatrix = host.play_view_snapshot()->render.objects[0].world;
        const auto sampleStart = std::chrono::steady_clock::now();
        CUE_EMBEDDED_CHECK(wait_editor(
            host, [&] { return host.play_view_snapshot() && host.play_view_snapshot()->tick >= initialTick + 8; }));
        const auto elapsed = std::chrono::steady_clock::now() - sampleStart;
        CUE_EMBEDDED_CHECK(elapsed >= std::chrono::milliseconds(70));
        CUE_EMBEDDED_CHECK(host.play_view_snapshot()->render.objects[0].world != initialMatrix);
        cue::math::floatTransform changed;
        changed.position.x = 2;
        CUE_EMBEDDED_CHECK(host.request_play_transform(k_cubeId, changed).has_value());
        CUE_EMBEDDED_CHECK(wait_editor(host,
                                       [&]
                                       {
                                           return host.play_view_snapshot() &&
                                                  host.play_view_snapshot()->render.objects[0].world.values[3][0] == 2;
                                       }));
        auto entity = host.preview_world()->find_by_id(*host.preview_instance(), k_cubeId);
        CUE_EMBEDDED_CHECK(entity.has_value());
        auto transform = host.preview_world()->get_component<cue::TransformComponent>(*entity.try_value());
        CUE_EMBEDDED_CHECK(transform.has_value() && (*transform.try_value())->local().position.x == 0);
        CUE_EMBEDDED_CHECK(host.stop_play().has_value());
        CUE_EMBEDDED_CHECK(!host.play_view_snapshot());
        CUE_EMBEDDED_CHECK(wait_editor(host, [&] { return host.play_state() == cue::PlayState::Stopped; }));
        CUE_EMBEDDED_CHECK(host.start_play().has_value());
        CUE_EMBEDDED_CHECK(wait_editor(host, [&] { return host.play_view_snapshot() != nullptr; }));
        CUE_EMBEDDED_CHECK(host.play_view_snapshot()->render.objects[0].world.values[3][0] == 0);
        CUE_EMBEDDED_CHECK(host.stop_play().has_value());
        CUE_EMBEDDED_CHECK(wait_editor(host, [&] { return host.play_state() == cue::PlayState::Stopped; }));
    }
    else
    {
        if (a_mode == 5)
        {
            // 子の Update が停止するまで Editor 自身は描画と操作を継続する
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
            CUE_EMBEDDED_CHECK(wait_editor(host, [&] { return std::chrono::steady_clock::now() >= deadline; }));
            CUE_EMBEDDED_CHECK(host.stop_play().has_value());
        }
        const auto terminal = a_mode == 7 ? cue::PlayState::Stopped : cue::PlayState::Failed;
        CUE_EMBEDDED_CHECK(wait_editor(host, [&] { return host.play_state() == terminal; }));
        CUE_EMBEDDED_CHECK(!host.play_view_snapshot());
        const auto before = host.frame_progress();
        CUE_EMBEDDED_CHECK(before.has_value());
        CUE_EMBEDDED_CHECK(wait_editor(host,
                                       [&]
                                       {
                                           auto progress = host.frame_progress();
                                           return progress.has_value() && progress.try_value()->renderedFrames >
                                                                              before.try_value()->renderedFrames + 2;
                                       }));
    }
    auto after = host.scene()->serialize();
    CUE_EMBEDDED_CHECK(after.has_value() && *after.try_value() == *original.try_value());
    CUE_EMBEDDED_CHECK(host.shutdown().has_value());
    return 0;
}

/// @brief 不正 Snapshot / 未要求応答が親の状態へ公開されないことを検証する
int test_protocol(const cue::Path &a_fixture)
{
    for (const auto &[name, expected] :
         {std::pair{"bad-view-nan.dll", "Play.view_protocol"},
          std::pair{"bad-view-count.dll", "Play.view_protocol"},
          std::pair{"bad-view-request.dll", "Play.message_protocol"},
          std::pair{"bad-transform-ack.dll", "Play.transform_ack_protocol"}})
    {
        auto module = a_fixture.parent().join(name);
        auto scene = make_scene(0);
        CUE_EMBEDDED_CHECK(module.has_value() && scene.has_value());
        auto client = cue::PlayClient::create({a_fixture, module.take_value(), true, 0, true});
        CUE_EMBEDDED_CHECK(client.has_value());
        CUE_EMBEDDED_CHECK((*client.try_value())->start(*scene.try_value()).has_value());
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while ((*client.try_value())->state() != cue::PlayState::Failed && std::chrono::steady_clock::now() < deadline)
        {
            CUE_EMBEDDED_CHECK((*client.try_value())->poll().has_value());
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        CUE_EMBEDDED_CHECK((*client.try_value())->state() == cue::PlayState::Failed);
        CUE_EMBEDDED_CHECK((*client.try_value())->error() == expected);
        CUE_EMBEDDED_CHECK(!(*client.try_value())->view_snapshot());
    }
    return 0;
}
} // namespace

/// @brief 実 PlayHost / Game DLL / WARP Editor で埋込み表示と障害分離を検証する
int main(int a_count, char **a_arguments)
{
    CUE_EMBEDDED_CHECK(a_count == 4);
    auto executable = cue::Path::create(a_arguments[1]);
    auto module = cue::Path::create(a_arguments[2]);
    auto protocol = cue::Path::create(a_arguments[3]);
    CUE_EMBEDDED_CHECK(executable.has_value() && module.has_value() && protocol.has_value());
    if (const auto failed = test_protocol(*protocol.try_value()))
        return failed;
    for (const double mode : {6, 4, 5, 7})
        if (const auto failed = test_editor(*executable.try_value(), *module.try_value(), mode))
            return failed;
    return 0;
}
