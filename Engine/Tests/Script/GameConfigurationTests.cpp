#include <ScriptPlay/PlayClient.h>

#include <chrono>
#include <cstdio>
#include <functional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <Scene/Scene.h>

namespace
{
constexpr const char *k_objectId = "9a00d731-7759-424c-9ade-fd086d624551";
constexpr const char *k_scriptId = "6c78eb99-aee8-4602-9fba-66fa96080f99";

#define CUE_CONFIGURATION_CHECK(...)                                                                                   \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(__VA_ARGS__))                                                                                            \
        {                                                                                                              \
            std::fprintf(stderr, "Game configuration check failed at %d: %s\n", __LINE__, #__VA_ARGS__);               \
            return __LINE__;                                                                                           \
        }                                                                                                              \
    } while (false)

/// @brief Frame / Runtime の参照を共有せず、選択構成の子だけを非待機で進める
bool wait_until(cue::PlayClient &a_client, const std::function<bool()> &a_ready,
                cue::PlayState a_terminal = cue::PlayState::Running)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
    while (std::chrono::steady_clock::now() < deadline)
    {
        auto polled = a_client.poll();
        if (!polled.has_value())
        {
            std::fprintf(stderr, "Play poll failed: %s\n", polled.try_error()->operation.c_str());
            return false;
        }
        if (a_ready())
            return true;
        if (a_client.state() == cue::PlayState::Failed && a_terminal != cue::PlayState::Failed)
        {
            std::fprintf(stderr, "Play failed: %.*s\n", int(a_client.error().size()), a_client.error().data());
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::fprintf(stderr, "Play timed out: state %d, %.*s\n", int(a_client.state()), int(a_client.error().size()),
                 a_client.error().data());
    return false;
}

/// @brief Script の Mode を指定した未保存 Cube / Camera Scene を所有値で作る
cue::Result<cue::Scene> make_scene(double a_mode)
{
    cue::SceneDescriptor descriptor;
    descriptor.sceneId = "8d94c2b1-9b6a-435c-ade7-c01f473c3951";
    descriptor.name = "Cross Configuration Play";
    cue::SceneObjectDescriptor cube;
    cube.objectId = k_objectId;
    cube.name = "Cube";
    cube.transform.emplace();
    cube.mesh.emplace();
    cue::ScriptDescriptor script;
    script.instanceId = k_scriptId;
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

/// @brief 正常終了まで Instance の停止・破棄を子に行わせ、View の失効を確認する
bool stop(cue::PlayClient &a_client)
{
    return a_client.stop().has_value() && !a_client.view_snapshot() &&
           wait_until(
               a_client, [&] { return a_client.state() == cue::PlayState::Stopped; }, cue::PlayState::Stopped) &&
           !a_client.process_id();
}

/// @brief 確定した次の Tick を待ち、描画値が取得できることを確認する
bool wait_tick(cue::PlayClient &a_client, std::uint64_t a_tick)
{
    return wait_until(a_client, [&] { return a_client.view_snapshot() && a_client.view_snapshot()->tick >= a_tick; });
}

/// @brief Metadata、回転、設定変更、停止・再開と例外の Status 変換を実 DLL で検証する
int verify_module(const cue::Path &a_host, const cue::Path &a_module, cue::ScriptConfiguration a_configuration)
{
    std::printf("Testing Game configuration %u: %s\n", unsigned(a_configuration), a_module.utf8().c_str());
    auto created = cue::PlayClient::create({a_host, a_module, true, 0, true, a_configuration});
    CUE_CONFIGURATION_CHECK(created.has_value());
    auto client = created.take_value();
    CUE_CONFIGURATION_CHECK(client->describe().has_value());
    CUE_CONFIGURATION_CHECK(
        wait_until(*client, [&] { return client->state() == cue::PlayState::Stopped; }, cue::PlayState::Stopped));
    CUE_CONFIGURATION_CHECK(client->types().size() == 1 && client->types()[0].typeId == "test.Failure");
    auto scene = make_scene(6);
    CUE_CONFIGURATION_CHECK(scene.has_value());
    auto original = scene.try_value()->serialize();
    CUE_CONFIGURATION_CHECK(original.has_value());
    CUE_CONFIGURATION_CHECK(client->start(*scene.try_value()).has_value() && wait_tick(*client, 2));
    CUE_CONFIGURATION_CHECK(client->view_snapshot()->render.objects.size() == 1);
    const auto firstSession = client->session_id();
    const auto firstTick = client->view_snapshot()->tick;
    const auto firstMatrix = client->view_snapshot()->render.objects[0].world;
    CUE_CONFIGURATION_CHECK(wait_tick(*client, firstTick + 4));
    CUE_CONFIGURATION_CHECK(client->view_snapshot()->render.objects[0].world != firstMatrix);
    // 設定値は POD / 借用文字列で DLL に渡し、Game 内の設定を更新する
    cue::ScriptPropertyValue stoppedMode{"mode", cue::ScriptPropertyKind::Number, 0, false};
    auto request = client->set_property(k_objectId, k_scriptId, stoppedMode);
    CUE_CONFIGURATION_CHECK(request.has_value());
    bool received = false;
    CUE_CONFIGURATION_CHECK(wait_until(*client,
                                       [&]
                                       {
                                           for (const auto &ack : client->take_acknowledgements())
                                               if (ack.requestId == *request.try_value() && ack.isAccepted &&
                                                   ack.objectId == k_objectId && ack.scriptInstanceId == k_scriptId &&
                                                   ack.value == stoppedMode)
                                                   received = true;
                                           return received;
                                       }));
    // 応答より前に採取済みの View を読み違えず、変更後の Tick の停止姿勢を比較する
    CUE_CONFIGURATION_CHECK(wait_tick(*client, client->view_snapshot()->tick + 2));
    const auto stoppedTick = client->view_snapshot()->tick;
    const auto stoppedMatrix = client->view_snapshot()->render.objects[0].world;
    CUE_CONFIGURATION_CHECK(wait_tick(*client, stoppedTick + 3));
    CUE_CONFIGURATION_CHECK(client->view_snapshot()->render.objects[0].world == stoppedMatrix);
    CUE_CONFIGURATION_CHECK(stop(*client));

    CUE_CONFIGURATION_CHECK(client->start(*scene.try_value()).has_value() && wait_tick(*client, 2));
    CUE_CONFIGURATION_CHECK(client->session_id() != firstSession);
    const auto restartedTick = client->view_snapshot()->tick;
    const auto restartedMatrix = client->view_snapshot()->render.objects[0].world;
    CUE_CONFIGURATION_CHECK(wait_tick(*client, restartedTick + 3));
    CUE_CONFIGURATION_CHECK(client->view_snapshot()->render.objects[0].world != restartedMatrix);
    CUE_CONFIGURATION_CHECK(stop(*client));

    // Result と C++ 例外を両方確認し、どちらも親へ C++ オブジェクトを投げず診断値で届くことを確かめる
    for (const auto mode : {3, 8})
    {
        auto failedScene = make_scene(mode);
        CUE_CONFIGURATION_CHECK(failedScene.has_value());
        CUE_CONFIGURATION_CHECK(client->start(*failedScene.try_value()).has_value());
        CUE_CONFIGURATION_CHECK(
            wait_until(*client, [&] { return client->state() == cue::PlayState::Failed; }, cue::PlayState::Failed));
        const auto expected = mode == 3 ? "Failure.update" : "Script.unhandled_exception";
        CUE_CONFIGURATION_CHECK(client->error().find(expected) != std::string_view::npos);
        CUE_CONFIGURATION_CHECK(!client->view_snapshot());
    }
    // 失敗した DLL の Instance / Process が次の正常 Session に持ち越されない
    CUE_CONFIGURATION_CHECK(client->start(*scene.try_value()).has_value() && wait_tick(*client, 2));
    CUE_CONFIGURATION_CHECK(stop(*client));
    auto after = scene.try_value()->serialize();
    CUE_CONFIGURATION_CHECK(after.has_value() && *after.try_value() == *original.try_value());
    return 0;
}

/// @brief DLL の真の構成と異なる期待値は Metadata と Play の両経路で拒否する
int verify_mismatch(const cue::Path &a_host, const cue::Path &a_module, cue::ScriptConfiguration a_actual)
{
    auto scene = make_scene(6);
    CUE_CONFIGURATION_CHECK(scene.has_value());
    for (const auto expected :
         {cue::ScriptConfiguration::Debug, cue::ScriptConfiguration::Development, cue::ScriptConfiguration::Release})
    {
        if (expected == a_actual)
            continue;
        for (const bool describe : {true, false})
        {
            auto created = cue::PlayClient::create({a_host, a_module, true, 0, true, expected});
            CUE_CONFIGURATION_CHECK(created.has_value());
            auto client = created.take_value();
            auto started = describe ? client->describe() : client->start(*scene.try_value());
            CUE_CONFIGURATION_CHECK(started.has_value());
            CUE_CONFIGURATION_CHECK(
                wait_until(*client, [&] { return client->state() == cue::PlayState::Failed; }, cue::PlayState::Failed));
            CUE_CONFIGURATION_CHECK(client->error().find("GameModule.abi_mismatch") != std::string_view::npos);
            CUE_CONFIGURATION_CHECK(!client->view_snapshot());
        }
    }
    return 0;
}
} // namespace

/// @brief 同一 PlayHost から真の Debug / Development / Release Game DLL を検証する
int main(int a_count, char **a_arguments)
{
    CUE_CONFIGURATION_CHECK(a_count == 5);
    auto host = cue::Path::create(a_arguments[1]);
    CUE_CONFIGURATION_CHECK(host.has_value() && host.try_value()->is_absolute());
    const cue::ScriptConfiguration configurations[]{
        cue::ScriptConfiguration::Debug, cue::ScriptConfiguration::Development, cue::ScriptConfiguration::Release};
    for (int index = 0; index < 3; ++index)
    {
        auto module = cue::Path::create(a_arguments[index + 2]);
        CUE_CONFIGURATION_CHECK(module.has_value() && module.try_value()->is_absolute());
        if (const auto failed = verify_module(*host.try_value(), *module.try_value(), configurations[index]))
            return failed;
        if (const auto failed = verify_mismatch(*host.try_value(), *module.try_value(), configurations[index]))
            return failed;
    }
    return 0;
}
