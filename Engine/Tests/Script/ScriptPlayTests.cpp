#include <ScriptPlay/PlayClient.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include <Scene/Scene.h>

namespace
{
constexpr const char *k_objectId = "9a00d731-7759-424c-9ade-fd086d624551";
constexpr const char *k_scriptId = "6c78eb99-aee8-4602-9fba-66fa96080f99";

/// @brief 非待機 Client の状態を期限まで進め、固まった場合も Test を終了する
bool wait_state(cue::PlayClient &a_client, cue::PlayState a_state, std::uint32_t a_timeout = 8000)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(a_timeout);
    do
    {
        auto polled = a_client.poll();
        if (!polled.has_value())
            return false;
        if (a_client.state() == a_state)
            return true;
        if (a_client.state() == cue::PlayState::Failed && a_state != cue::PlayState::Failed)
        {
            std::fprintf(stderr, "Play failed: %.*s\n", static_cast<int>(a_client.error().size()),
                         a_client.error().data());
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

/// @brief File 書込をせず、未保存の Script 割当を持つ Scene を作る
cue::Result<cue::Scene> make_scene(std::string a_type = "sample.RotateCube", double a_mode = 0)
{
    cue::SceneDescriptor descriptor;
    descriptor.sceneId = "8d94c2b1-9b6a-435c-ade7-c01f473c3951";
    descriptor.name = "Unsaved Play Snapshot";
    cue::SceneObjectDescriptor object;
    object.objectId = k_objectId;
    object.name = "Cube";
    object.transform = cue::math::floatTransform{};
    cue::ScriptDescriptor script;
    script.instanceId = k_scriptId;
    script.typeId = a_type;
    if (a_type == "test.Failure")
        script.properties.push_back({"mode", cue::ScriptPropertyKind::Number, a_mode, false});
    object.scripts.push_back(std::move(script));
    descriptor.objects.push_back(std::move(object));
    return cue::Scene::create(std::move(descriptor));
}

/// @brief Metadata 子 Process、未保存 Scene、型付き変更応答、停止・再実行を検証する
int test_play(const cue::Path &a_executable, const cue::Path &a_module)
{
    auto created = cue::PlayClient::create({a_executable, a_module, true, 0});
    if (!created.has_value())
        return __LINE__;
    auto client = created.take_value();
    auto described = client->describe();
    if (!described.has_value() || !wait_state(*client, cue::PlayState::Stopped) || client->types().size() != 1 ||
        client->types()[0].typeId != "sample.RotateCube")
        return __LINE__;
    const auto describeSession = client->session_id();
    auto scene = make_scene();
    if (!scene.has_value())
        return __LINE__;
    // Pipe の容量より大きい Snapshot を分割転送し、部分 Write / Read を検証する
    auto large = scene.try_value()->descriptor();
    for (unsigned index = 1; index <= 512; ++index)
    {
        char id[40]{};
        std::snprintf(id, sizeof(id), "a0000000-0000-4000-8000-%012x", index);
        cue::SceneObjectDescriptor object;
        object.objectId = id;
        object.name = std::string(200, 'A');
        large.objects.push_back(std::move(object));
    }
    scene = cue::Scene::create(std::move(large));
    if (!scene.has_value())
        return __LINE__;
    auto started = client->start(*scene.try_value());
    if (!started.has_value() || !wait_state(*client, cue::PlayState::Running) ||
        client->session_id() == describeSession)
        return __LINE__;
    const auto playSession = client->session_id();
    auto busy = client->start(*scene.try_value());
    if (busy.has_value() || client->state() != cue::PlayState::Running)
        return __LINE__;
    cue::ScriptPropertyValue value{"speed", cue::ScriptPropertyKind::Number, 180, false};
    auto requested = client->set_property(k_objectId, k_scriptId, value);
    if (!requested.has_value())
        return __LINE__;
    bool hasAck = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!hasAck && std::chrono::steady_clock::now() < deadline)
    {
        auto polled = client->poll();
        if (!polled.has_value() || client->state() != cue::PlayState::Running)
            return __LINE__;
        for (const auto &ack : client->take_acknowledgements())
        {
            if (ack.requestId != *requested.try_value() || !ack.isAccepted || ack.objectId != k_objectId ||
                ack.scriptInstanceId != k_scriptId || ack.value != value)
                return __LINE__;
            hasAck = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    auto stopped = client->stop();
    if (!hasAck || !stopped.has_value() || !wait_state(*client, cue::PlayState::Stopped))
        return __LINE__;
    // 同じ編集 Scene から新 Session を作り、前回の実行値や DLL Instance を再利用しない
    auto restarted = client->start(*scene.try_value());
    if (!restarted.has_value() || !wait_state(*client, cue::PlayState::Running) || client->session_id() == playSession)
        return __LINE__;
    stopped = client->stop();
    if (!stopped.has_value() || !wait_state(*client, cue::PlayState::Stopped))
        return __LINE__;
    // Ready より前の取消も正常な停止として扱い、次の Session を開始できる
    auto cancelledStart = client->start(*scene.try_value());
    if (!cancelledStart.has_value())
        return __LINE__;
    auto cancelledStop = client->stop();
    if (!cancelledStop.has_value() || !wait_state(*client, cue::PlayState::Stopped))
        return __LINE__;
    // 実 Window / SwapChain / SceneRenderer の経路も同じ Script Scene から完走させる
    auto visible = cue::PlayClient::create({a_executable, a_module, false, 10});
    if (!visible.has_value())
        return __LINE__;
    auto visibleScene = scene.try_value()->descriptor();
    visibleScene.objects[0].mesh = cue::SceneMeshDescriptor{};
    cue::SceneObjectDescriptor camera;
    camera.objectId = "35f6ad62-88f0-49dc-b067-f318c116eb61";
    camera.name = "Camera";
    camera.transform = cue::math::floatTransform{};
    camera.transform->position = {0, 0, 3};
    camera.camera = cue::SceneCameraDescriptor{};
    visibleScene.objects.push_back(std::move(camera));
    auto withCamera = cue::Scene::create(std::move(visibleScene));
    if (!withCamera.has_value())
        return __LINE__;
    auto visibleStarted = (*visible.try_value())->start(*withCamera.try_value());
    if (!visibleStarted.has_value() || !wait_state(**visible.try_value(), cue::PlayState::Stopped, 15000))
        return __LINE__;
    return 0;
}

/// @brief Native 終了・通常失敗・無応答のすべてで呼出元 Process が継続できることを検証する
int test_failure(const cue::Path &a_executable, const cue::Path &a_fixture)
{
    for (int mode : {1, 3, 2})
    {
        auto created = cue::PlayClient::create({a_executable, a_fixture, true, 0});
        if (!created.has_value())
            return __LINE__;
        auto client = created.take_value();
        auto scene = make_scene("test.Failure", mode);
        if (!scene.has_value())
            return __LINE__;
        auto started = client->start(*scene.try_value());
        if (!started.has_value())
            return __LINE__;
        if (mode == 2)
        {
            // start の中で固まる子にも停止要求は即座に返り、poll が期限で Job を終了する
            const auto initial = std::chrono::steady_clock::now();
            while (std::chrono::steady_clock::now() - initial < std::chrono::milliseconds(100))
            {
                auto polled = client->poll();
                if (!polled.has_value())
                    return __LINE__;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            auto stop = client->stop();
            if (!stop.has_value() || !wait_state(*client, cue::PlayState::Failed, 4000) ||
                client->error() != "Play.stop_timeout")
                return __LINE__;
        }
        else if (!wait_state(*client, cue::PlayState::Failed) || client->error().empty())
            return __LINE__;
    }
    return 0;
}

/// @brief 子が Code 0 で終了しても未知 Request / 不正 Header の応答を受理しない
int test_protocol(const cue::Path &a_executable)
{
    // 不正な選択構成を子へ送る前に拒否し、既存の既定構成指定なしは維持する
    auto invalidModule = a_executable.parent().join("invalid-configuration.dll");
    if (!invalidModule.has_value())
        return __LINE__;
    for (const auto value : {0u, 4u})
    {
        auto invalid = cue::PlayClient::create(
            {a_executable, *invalidModule.try_value(), true, 0, false, static_cast<cue::ScriptConfiguration>(value)});
        if (invalid.has_value() || invalid.try_error()->operation != "Play.module_configuration")
            return __LINE__;
    }
    for (const auto *name : {"bad-ack.dll", "bad-header.dll", "bad-truncated.dll"})
    {
        auto module = a_executable.parent().join(name);
        if (!module.has_value())
            return __LINE__;
        auto created = cue::PlayClient::create({a_executable, module.take_value(), true, 0});
        if (!created.has_value())
            return __LINE__;
        auto scene = make_scene();
        if (!scene.has_value())
            return __LINE__;
        auto started = (*created.try_value())->start(*scene.try_value());
        if (!started.has_value() || !wait_state(**created.try_value(), cue::PlayState::Failed) ||
            (*created.try_value())->error().empty())
            return __LINE__;
    }
    // 複数 poll にまたがる大きな Metadata を、子の正常終了後も EOF まで回収する
    auto module = a_executable.parent().join("large-metadata.dll");
    if (!module.has_value())
        return __LINE__;
    auto created = cue::PlayClient::create({a_executable, module.take_value(), true, 0});
    if (!created.has_value())
        return __LINE__;
    auto described = (*created.try_value())->describe();
    if (!described.has_value() || !wait_state(**created.try_value(), cue::PlayState::Stopped) ||
        (*created.try_value())->types().size() != 256 || (*created.try_value())->types()[255].properties.size() != 32)
        return __LINE__;
    // Debug 子が許可前に Metadata を送り出した場合も、通常の応答として受理しない
    auto prematureModule = a_executable.parent().join("premature-debug-types.dll");
    if (!prematureModule.has_value())
        return __LINE__;
    auto premature = cue::PlayClient::create({a_executable, prematureModule.take_value(), true, 0});
    auto debugScene = make_scene();
    if (!premature.has_value() || !debugScene.has_value())
        return __LINE__;
    auto startedDebug = (*premature.try_value())->start_debug(*debugScene.try_value());
    if (!startedDebug.has_value() || !wait_state(**premature.try_value(), cue::PlayState::Failed) ||
        (*premature.try_value())->error() != "Play.message_protocol")
        return __LINE__;
    return 0;
}

/// @brief 存在しない DLL でも接続待機へ到達し、未接続の開始拒否と Stop / 再要求を確認する
int test_debug_wait(const cue::Path &a_executable)
{
    auto missing = a_executable.parent().join("must-not-load-before-debugger.dll");
    if (!missing.has_value())
        return __LINE__;
    auto created = cue::PlayClient::create({a_executable, missing.take_value(), true, 0});
    if (!created.has_value())
        return __LINE__;
    auto client = created.take_value();
    auto scene = make_scene();
    if (!scene.has_value())
        return __LINE__;
    for (unsigned run = 0; run < 2; ++run)
    {
        auto started = client->start_debug(*scene.try_value());
        if (!started.has_value() || !wait_state(*client, cue::PlayState::WaitingForDebugger) || !client->process_id() ||
            !client->is_debugging() || client->is_debugger_attached() || !client->types().empty())
            return __LINE__;
        auto continued = client->continue_debug();
        if (continued.has_value() || continued.try_error()->operation != "Play.debugger_not_attached" ||
            client->state() != cue::PlayState::WaitingForDebugger)
            return __LINE__;
        auto busy = client->start(*scene.try_value());
        if (busy.has_value() || client->state() != cue::PlayState::WaitingForDebugger)
            return __LINE__;
        bool rejectedThread = false;
        std::thread wrong(
            [&]()
            {
                auto continuedWrong = client->continue_debug();
                rejectedThread =
                    !continuedWrong.has_value() && !client->process_id() && !client->is_debugger_attached();
            });
        wrong.join();
        if (!rejectedThread)
            return __LINE__;
        auto stopped = client->stop();
        if (!stopped.has_value() || !wait_state(*client, cue::PlayState::Stopped) || client->process_id())
            return __LINE__;
    }
    return 0;
}
} // namespace

/// @brief 同じ Build 構成の実行 File と DLL を受け取り、別 Process 契約を確認する
int main(int a_count, char **a_arguments)
{
    if (a_count != 5)
        return 2;
    auto executable = cue::Path::create(a_arguments[1]);
    auto module = cue::Path::create(a_arguments[2]);
    auto fixture = cue::Path::create(a_arguments[3]);
    auto protocol = cue::Path::create(a_arguments[4]);
    if (!executable.has_value() || !module.has_value() || !fixture.has_value() || !protocol.has_value())
        return 2;
    auto result = test_play(*executable.try_value(), *module.try_value());
    if (!result)
        result = test_failure(*executable.try_value(), *fixture.try_value());
    if (!result)
        result = test_protocol(*protocol.try_value());
    if (!result)
        result = test_debug_wait(*executable.try_value());
    if (result)
        std::fprintf(stderr, "Script Play test failed at line %d\n", result);
    return result ? 1 : 0;
}
