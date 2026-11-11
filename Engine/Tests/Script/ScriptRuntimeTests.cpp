#include <ScriptRuntime/ScriptSystem.h>

#include <chrono>
#include <cstdio>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <Runtime/Components/CameraComponent.h>
#include <Runtime/Components/MeshRendererComponent.h>
#include <Runtime/Components/TransformComponent.h>
#include <Runtime/World.h>
#include <Scene/Scene.h>
#include <SceneRuntime/SceneInstantiation.h>
#include <Script/Module.h>
#include <Script/ScriptEvent.h>

namespace
{
#define CUE_RUNTIME_SCRIPT_CHECK(...)                                                                                  \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(__VA_ARGS__))                                                                                            \
        {                                                                                                              \
            std::fprintf(stderr, "Script Runtime check failed at %d: %s\n", __LINE__, #__VA_ARGS__);                   \
            return __LINE__;                                                                                           \
        }                                                                                                              \
    } while (false)

constexpr const char *k_firstScriptId = "3ce1feb2-e711-4ca3-9698-2b32e0d9637c";
constexpr const char *k_secondScriptId = "684f8846-e5ba-45d9-90be-7cbf6f3810be";
constexpr const char *k_objectId = "9a00d731-7759-424c-9ade-fd086d624551";

struct Counters final
{
    unsigned created = 0;
    unsigned destroyed = 0;
    unsigned started = 0;
    unsigned updated = 0;
    unsigned stopped = 0;
    unsigned setters = 0;
    unsigned stoppedDeadEntity = 0;
    bool shouldFailStart = false;
    bool shouldThrowUpdate = false;
    bool shouldReport = false;
};

// C の登録入口を持つ静的 Module Fixture。各 Test の World より長く所有する
Counters g_counters;
std::unique_ptr<cue::ScriptModule> g_module;
std::optional<std::uint32_t> g_configurationOverride;

class Move final : public cue::Behaviour
{
  public:
    /// @brief Module Factory が実体を作った回数を数える
    Move()
    {
        ++g_counters.created;
    }
    /// @brief World 停止時に Module 内で実体が回収されたことを数える
    ~Move() override
    {
        ++g_counters.destroyed;
    }
    /// @brief 既定値と実行時変更の重複適用を検出する
    cue::Result<void> set_property(std::string_view a_id, const cue::ScriptPropertyValue &a_value) override
    {
        if (a_id != "speed")
            return cue::Result<void>::failure({cue::ErrorCategory::InvalidArgument, "Move.property"});
        ++g_counters.setters;
        m_speed = a_value.number;
        return cue::Result<void>::success();
    }
    /// @brief Active 化時の一度だけの開始と部分失敗を検証する
    cue::Result<void> start(cue::GameObject &a_self) override
    {
        ++g_counters.started;
        if (g_counters.shouldFailStart)
            return cue::Result<void>::failure({cue::ErrorCategory::PlatformFailure, "Move.partial_start"});
        auto alive = a_self.is_alive();
        if (!alive.has_value() || !*alive.try_value())
            return cue::Result<void>::failure({cue::ErrorCategory::InvalidState, "Move.start.entity"});
        if (g_counters.shouldReport)
            return a_self.report(2, "fixture diagnostic");
        return cue::Result<void>::success();
    }
    /// @brief ABI 経由の Transform 変更と終端例外を検証する
    cue::Result<void> update(cue::GameObject &a_self, const cue::ScriptUpdateContext &a_context) override
    {
        ++g_counters.updated;
        if (g_counters.shouldThrowUpdate)
            throw std::runtime_error("Move.update fixture exception");
        auto value = a_self.transform();
        if (!value.has_value())
            return cue::Result<void>::failure(*value.try_error());
        auto transform = *value.try_value();
        transform.position.x +=
            static_cast<float>(m_speed * std::chrono::duration<double>(a_context.deltaTime).count());
        return a_self.set_transform(transform);
    }
    /// @brief 削除済み Entity でも停止が一度だけ呼ばれることを数える
    cue::Result<void> stop(cue::GameObject &a_self) override
    {
        ++g_counters.stopped;
        auto alive = a_self.is_alive();
        if (alive.has_value() && !*alive.try_value())
            ++g_counters.stoppedDeadEntity;
        return cue::Result<void>::success();
    }

  private:
    double m_speed = 0;
};

/// @brief SDK の登録を公開する C 互換入口。Fixture 所有者は Runtime より長く生存する
CueScriptStatus CUE_SCRIPT_CALL entry(std::uint32_t a_version, CueScriptModuleApi *a_api) noexcept
{
    if (!g_module || !a_api)
    {
        CueScriptStatus error{};
        error.code = 1;
        return error;
    }
    const auto status = g_module->get_api(a_version, *a_api);
    if (status.code == 0 && g_configurationOverride)
        a_api->configuration = *g_configurationOverride;
    return status;
}

/// @brief 前 Test の全 World 破棄後に静的 Module と観測値を作り直す
bool reset_module()
{
    g_module.reset();
    g_configurationOverride.reset();
    g_counters = {};
    cue::ScriptTypeDescriptor type{
        "test.move", "Move", 1, {{"speed", "Speed", cue::ScriptPropertyKind::Number, 2, false}}};
    auto module = cue::ScriptModule::create({cue::make_script_registration<Move>(std::move(type))});
    if (!module.has_value())
        return false;
    g_module = module.take_value();
    return true;
}

/// @brief 明示値を省略し、Module に登録した既定値を利用する割当を作る
cue::ScriptDescriptor assignment(const char *a_instanceId = k_firstScriptId)
{
    cue::ScriptDescriptor result;
    result.instanceId = a_instanceId;
    result.typeId = "test.move";
    return result;
}

struct Fixture final
{
    std::unique_ptr<cue::World> world;
    cue::ScriptSystem *scripts = nullptr;
    cue::SceneInstanceHandle instance;
    cue::EntityHandle entity;
};

/// @brief Scene の保存値を実体化し、Script の開始直前まで準備する
cue::Result<Fixture> prepare(std::vector<cue::ScriptDescriptor> a_scripts, cue::scriptDiagnosticCallback a_report = {})
{
    using namespace cue;
    auto module = RuntimeGameModule::from_static(entry);
    if (!module.has_value())
        return Result<Fixture>::failure(*module.try_error());
    auto system = ScriptSystem::create(module.take_value(), std::move(a_report));
    auto created = World::create();
    if (!system.has_value() || !created.has_value())
        return Result<Fixture>::failure({ErrorCategory::Fatal, "Fixture.prepare"});
    Fixture fixture;
    fixture.world = created.take_value();
    fixture.scripts = system.try_value()->get();
    auto added = fixture.world->add_system(system.take_value());
    if (!added.has_value())
        return Result<Fixture>::failure(*added.try_error());
    SceneDescriptor descriptor;
    descriptor.sceneId = "8d94c2b1-9b6a-435c-ade7-c01f473c3951";
    descriptor.name = "Script Scene";
    SceneObjectDescriptor object;
    object.objectId = k_objectId;
    object.name = "Cube";
    object.transform.emplace();
    object.scripts = std::move(a_scripts);
    descriptor.objects.push_back(std::move(object));
    auto scene = Scene::create(std::move(descriptor));
    if (!scene.has_value())
        return Result<Fixture>::failure(*scene.try_error());
    auto instance = instantiate_scene(*scene.try_value(), *fixture.world);
    if (!instance.has_value())
        return Result<Fixture>::failure(*instance.try_error());
    fixture.instance = *instance.try_value();
    auto attached = attach_scene_scripts(*scene.try_value(), *fixture.world, fixture.instance);
    if (!attached.has_value())
        return Result<Fixture>::failure(*attached.try_error());
    auto entity = fixture.world->find_by_id(fixture.instance, k_objectId);
    if (!entity.has_value())
        return Result<Fixture>::failure(*entity.try_error());
    fixture.entity = *entity.try_value();
    return Result<Fixture>::success(std::move(fixture));
}

/// @brief 既定値の適用、無効化と再有効化、値変更、削除時回収を検証する
int test_lifecycle()
{
    using namespace cue;
    using namespace std::chrono_literals;
    CUE_RUNTIME_SCRIPT_CHECK(reset_module());
    auto script = assignment();
    script.isEnabled = false;
    auto prepared = prepare({script});
    CUE_RUNTIME_SCRIPT_CHECK(prepared.has_value());
    auto fixture = prepared.take_value();
    auto &world = *fixture.world;
    CUE_RUNTIME_SCRIPT_CHECK(world.initialize().has_value());
    CUE_RUNTIME_SCRIPT_CHECK(g_counters.created == 1 && g_counters.setters == 1 && g_counters.started == 0);
    CUE_RUNTIME_SCRIPT_CHECK(world.update(1s).has_value() && g_counters.updated == 0 && g_counters.setters == 1);
    auto component = world.get_component<ScriptComponent>(fixture.entity);
    CUE_RUNTIME_SCRIPT_CHECK(component.has_value());
    script.isEnabled = true;
    CUE_RUNTIME_SCRIPT_CHECK((*component.try_value())->set_scripts({script}).has_value());
    CUE_RUNTIME_SCRIPT_CHECK(world.update(1s).has_value() && g_counters.started == 1 && g_counters.updated == 1);
    CUE_RUNTIME_SCRIPT_CHECK(world.set_active(fixture.entity, false).has_value());
    CUE_RUNTIME_SCRIPT_CHECK(world.update(1s).has_value() && g_counters.updated == 1);
    CUE_RUNTIME_SCRIPT_CHECK(world.set_active(fixture.entity, true).has_value());
    CUE_RUNTIME_SCRIPT_CHECK(world.update(1s).has_value() && g_counters.started == 1 && g_counters.updated == 2);
    script.isEnabled = false;
    CUE_RUNTIME_SCRIPT_CHECK((*component.try_value())->set_scripts({script}).has_value());
    CUE_RUNTIME_SCRIPT_CHECK(world.update(1s).has_value() && g_counters.updated == 2);
    script.isEnabled = true;
    CUE_RUNTIME_SCRIPT_CHECK((*component.try_value())->set_scripts({script}).has_value());
    CUE_RUNTIME_SCRIPT_CHECK(world.update(1s).has_value() && g_counters.started == 1 && g_counters.setters == 1);

    const ScriptPropertyValue speed{"speed", ScriptPropertyKind::Number, 5, false};
    CUE_RUNTIME_SCRIPT_CHECK(fixture.scripts->set_property(world, fixture.entity, k_firstScriptId, speed).has_value());
    CUE_RUNTIME_SCRIPT_CHECK(g_counters.setters == 2);
    CUE_RUNTIME_SCRIPT_CHECK(world.update(1s).has_value() && g_counters.setters == 2);
    auto transform = world.get_component<TransformComponent>(fixture.entity);
    CUE_RUNTIME_SCRIPT_CHECK(transform.has_value() && (*transform.try_value())->local().position.x == 11);
    CUE_RUNTIME_SCRIPT_CHECK(!(*transform.try_value())->is_dirty());
    CUE_RUNTIME_SCRIPT_CHECK(
        !fixture.scripts
             ->set_property(world, fixture.entity, k_firstScriptId, {"missing", ScriptPropertyKind::Number, 1, false})
             .has_value());
    CUE_RUNTIME_SCRIPT_CHECK(g_counters.setters == 2);

    bool wrongThread = false;
    std::thread other(
        [&]()
        {
            auto changed = fixture.scripts->set_property(world, fixture.entity, k_firstScriptId, speed);
            wrongThread = !changed.has_value() && changed.try_error()->category == ErrorCategory::WrongThread;
        });
    other.join();
    CUE_RUNTIME_SCRIPT_CHECK(wrongThread && g_counters.setters == 2);
    CUE_RUNTIME_SCRIPT_CHECK(world.destroy_entity(fixture.entity).has_value());
    CUE_RUNTIME_SCRIPT_CHECK(world.update(1s).has_value());
    CUE_RUNTIME_SCRIPT_CHECK(g_counters.stopped == 1 && g_counters.destroyed == 1 && g_counters.stoppedDeadEntity == 1);
    CUE_RUNTIME_SCRIPT_CHECK(world.shutdown().has_value() && world.shutdown().has_value());
    CUE_RUNTIME_SCRIPT_CHECK(g_counters.stopped == 1 && g_counters.destroyed == 1);
    return 0;
}

/// @brief 同一 Entity の複数割当と同じ Scene の別 Instance が独立して動くことを確認する
int test_multiple_and_instances()
{
    using namespace cue;
    using namespace std::chrono_literals;
    CUE_RUNTIME_SCRIPT_CHECK(reset_module());
    auto first = assignment();
    auto second = assignment(k_secondScriptId);
    second.properties.push_back({"speed", ScriptPropertyKind::Number, 3, false});
    auto prepared = prepare({first, second});
    CUE_RUNTIME_SCRIPT_CHECK(prepared.has_value());
    auto fixture = prepared.take_value();
    auto &world = *fixture.world;
    SceneDescriptor descriptor;
    descriptor.sceneId = "8d94c2b1-9b6a-435c-ade7-c01f473c3951";
    descriptor.name = "Script Scene";
    SceneObjectDescriptor object;
    object.objectId = k_objectId;
    object.name = "Cube";
    object.transform.emplace();
    object.scripts = {first, second};
    descriptor.objects.push_back(std::move(object));
    auto scene = Scene::create(std::move(descriptor));
    CUE_RUNTIME_SCRIPT_CHECK(scene.has_value());
    auto extra = instantiate_scene(*scene.try_value(), world);
    CUE_RUNTIME_SCRIPT_CHECK(extra.has_value() && *extra.try_value() != fixture.instance);
    CUE_RUNTIME_SCRIPT_CHECK(attach_scene_scripts(*scene.try_value(), world, *extra.try_value()).has_value());
    auto extraEntity = world.find_by_name(*extra.try_value(), "Cube");
    CUE_RUNTIME_SCRIPT_CHECK(extraEntity.has_value() && *extraEntity.try_value() != fixture.entity);
    CUE_RUNTIME_SCRIPT_CHECK(world.initialize().has_value());
    CUE_RUNTIME_SCRIPT_CHECK(g_counters.created == 4 && g_counters.started == 4 && g_counters.setters == 6);
    CUE_RUNTIME_SCRIPT_CHECK(world.update(1s).has_value() && g_counters.updated == 4 && g_counters.setters == 6);
    auto left = world.get_component<TransformComponent>(fixture.entity);
    auto right = world.get_component<TransformComponent>(*extraEntity.try_value());
    CUE_RUNTIME_SCRIPT_CHECK(left.has_value() && right.has_value() && (*left.try_value())->local().position.x == 5 &&
                             (*right.try_value())->local().position.x == 5);
    CUE_RUNTIME_SCRIPT_CHECK(fixture.scripts
                                 ->set_property(world, *extraEntity.try_value(), k_firstScriptId,
                                                {"speed", ScriptPropertyKind::Number, 7, false})
                                 .has_value());
    CUE_RUNTIME_SCRIPT_CHECK(world.update(1s).has_value());
    CUE_RUNTIME_SCRIPT_CHECK((*left.try_value())->local().position.x == 10 &&
                             (*right.try_value())->local().position.x == 15);
    CUE_RUNTIME_SCRIPT_CHECK(world.destroy_scene_instance(fixture.instance).has_value() &&
                             world.update(1s).has_value());
    CUE_RUNTIME_SCRIPT_CHECK(g_counters.stopped == 2 && g_counters.destroyed == 2);
    CUE_RUNTIME_SCRIPT_CHECK(world.shutdown().has_value() && g_counters.stopped == 4 && g_counters.destroyed == 4);
    return 0;
}

/// @brief GameObject の診断が Native API を介さず Host の注入 Callback へ届くことを確認する
int test_diagnostics()
{
    CUE_RUNTIME_SCRIPT_CHECK(reset_module());
    g_counters.shouldReport = true;
    unsigned received = 0;
    std::uint32_t severity = 0;
    std::string message;
    auto prepared = prepare({assignment()},
                            [&](std::uint32_t a_severity, std::string_view a_message)
                            {
                                ++received;
                                severity = a_severity;
                                message = a_message;
                            });
    CUE_RUNTIME_SCRIPT_CHECK(prepared.has_value());
    auto fixture = prepared.take_value();
    CUE_RUNTIME_SCRIPT_CHECK(fixture.world->initialize().has_value());
    CUE_RUNTIME_SCRIPT_CHECK(received == 1 && severity == 2 && message == "fixture diagnostic");
    CUE_RUNTIME_SCRIPT_CHECK(fixture.world->shutdown().has_value());
    CUE_RUNTIME_SCRIPT_CHECK(received == 1 && g_counters.stopped == 1 && g_counters.destroyed == 1);
    return 0;
}

/// @brief 型 / Schema の不一致を全 Script の開始前に検出し、部分開始と更新例外を回収する
int test_failures()
{
    using namespace cue;
    using namespace std::chrono_literals;
    for (const bool missingType : {true, false})
    {
        CUE_RUNTIME_SCRIPT_CHECK(reset_module());
        auto invalid = assignment(k_secondScriptId);
        if (missingType)
            invalid.typeId = "test.missing";
        else
            invalid.schemaVersion = 2;
        invalid.isEnabled = false;
        auto prepared = prepare({assignment(), invalid});
        CUE_RUNTIME_SCRIPT_CHECK(prepared.has_value());
        auto fixture = prepared.take_value();
        CUE_RUNTIME_SCRIPT_CHECK(!fixture.world->initialize().has_value());
        CUE_RUNTIME_SCRIPT_CHECK(g_counters.created == 0 && g_counters.started == 0);
        auto stopped = fixture.world->shutdown();
        CUE_RUNTIME_SCRIPT_CHECK(!stopped.has_value() && g_counters.destroyed == 0);
    }
    {
        CUE_RUNTIME_SCRIPT_CHECK(reset_module());
        auto prepared = prepare({assignment()});
        CUE_RUNTIME_SCRIPT_CHECK(prepared.has_value());
        auto fixture = prepared.take_value();
        g_counters.shouldFailStart = true;
        CUE_RUNTIME_SCRIPT_CHECK(!fixture.world->initialize().has_value() && g_counters.started == 1);
        CUE_RUNTIME_SCRIPT_CHECK(!fixture.world->update(1s).has_value() && g_counters.updated == 0);
        auto stopped = fixture.world->shutdown();
        CUE_RUNTIME_SCRIPT_CHECK(!stopped.has_value() && g_counters.stopped == 1 && g_counters.destroyed == 1);
        auto repeated = fixture.world->shutdown();
        CUE_RUNTIME_SCRIPT_CHECK(!repeated.has_value() && g_counters.stopped == 1 && g_counters.destroyed == 1);
    }
    {
        CUE_RUNTIME_SCRIPT_CHECK(reset_module());
        auto prepared = prepare({assignment()});
        CUE_RUNTIME_SCRIPT_CHECK(prepared.has_value());
        auto fixture = prepared.take_value();
        CUE_RUNTIME_SCRIPT_CHECK(fixture.world->initialize().has_value());
        g_counters.shouldThrowUpdate = true;
        auto failed = fixture.world->update(1s);
        CUE_RUNTIME_SCRIPT_CHECK(!failed.has_value() && failed.try_error()->category == ErrorCategory::Fatal);
        CUE_RUNTIME_SCRIPT_CHECK(!fixture.world->update(1s).has_value() && g_counters.updated == 1);
        auto stopped = fixture.world->shutdown();
        CUE_RUNTIME_SCRIPT_CHECK(!stopped.has_value() && g_counters.stopped == 1 && g_counters.destroyed == 1);
    }
    return 0;
}

/// @brief Static 登録は交差構成を拒否し、未知の選択構成は DLL を開く前に拒否する
int test_configurations()
{
    CUE_RUNTIME_SCRIPT_CHECK(reset_module());
    CueScriptModuleApi native{};
    CUE_RUNTIME_SCRIPT_CHECK(g_module->get_api(CUE_SCRIPT_ABI_VERSION, native).code == 0);
    CUE_RUNTIME_SCRIPT_CHECK(
        cue::is_valid_script_configuration(static_cast<cue::ScriptConfiguration>(native.configuration)));
    for (const auto configuration : {0u, 1u, 2u, 3u, 4u})
    {
        g_configurationOverride = configuration;
        auto checked = cue::RuntimeGameModule::from_static(entry);
        CUE_RUNTIME_SCRIPT_CHECK(checked.has_value() == (configuration == native.configuration));
        if (!checked.has_value())
            CUE_RUNTIME_SCRIPT_CHECK(checked.try_error()->operation == "GameModule.abi_mismatch");
    }
    g_configurationOverride.reset();
    auto missing = cue::Path::create("missing-configuration-fixture.dll");
    CUE_RUNTIME_SCRIPT_CHECK(missing.has_value());
    for (const auto invalid : {0u, 4u})
    {
        auto rejected =
            cue::RuntimeGameModule::load(*missing.try_value(), static_cast<cue::ScriptConfiguration>(invalid));
        CUE_RUNTIME_SCRIPT_CHECK(!rejected.has_value() &&
                                 rejected.try_error()->operation == "GameModule.configuration");
    }
    return 0;
}

unsigned g_received = 0;

class PulseSource final : public cue::Behaviour
{
  public:
    cue::ScriptEvent<unsigned> pulse;
    unsigned value = 1;

    /// @brief 設定を別 Script から直接受け取り、購読者へ通知する
    cue::Result<void> update(cue::GameObject &, const cue::ScriptUpdateContext &) override
    {
        return pulse.publish(value);
    }
};

class PulseReceiver final : public cue::Behaviour
{
  public:
    /// @brief 型付き Component と対象の GameObject を確認し、通知を一度だけ購読する
    cue::Result<void> update(cue::GameObject &a_self, const cue::ScriptUpdateContext &) override
    {
        if (m_subscribed)
        {
            return cue::Result<void>::success();
        }
        auto cube = a_self.find_by_name("Cube");
        if (!cube.has_value() || !a_self.get_component<cue::ScriptCamera>().has_value() ||
            !a_self.get_component<cue::ScriptMeshRenderer>().has_value())
        {
            return cue::Result<void>::failure({cue::ErrorCategory::InvalidState, "PulseReceiver.components"});
        }
        auto connected = cube.try_value()->with_script<PulseSource>([&](PulseSource &a_source, cue::GameObject &a_target)
        {
            auto transform = a_target.get_component<cue::math::floatTransform>();
            if (!transform.has_value() || a_target.handle().index != a_self.handle().index)
            {
                return cue::Result<void>::failure({cue::ErrorCategory::InvalidState, "PulseReceiver.target"});
            }
            a_source.value = 2;
            return a_source.pulse.subscribe(*this, [](const unsigned &a_value)
            {
                g_received += a_value;
                return cue::Result<void>::success();
            });
        }, k_firstScriptId);
        m_subscribed = connected.has_value();
        if (!connected.has_value())
        {
            return connected;
        }
        auto value = cube.try_value()->with_script<PulseSource>([](const PulseSource &a_source)
        {
            return a_source.value;
        });
        return value.has_value() && *value.try_value() == 2
                   ? cue::Result<void>::success()
                   : cue::Result<void>::failure({cue::ErrorCategory::InvalidState, "PulseReceiver.value"});
    }

  private:
    bool m_subscribed = false;
};

/// @brief 無効化時の配送停止と再有効化、購読者削除、型付き ABI の取得経路を確認する
int test_script_communication()
{
    using namespace cue;
    using namespace std::chrono_literals;
    CUE_RUNTIME_SCRIPT_CHECK(reset_module());
    auto module = ScriptModule::create({make_script_registration<PulseSource>({"test.pulse", "Pulse", 1, {}}),
                                        make_script_registration<PulseReceiver>({"test.receiver", "Receiver", 1, {}})});
    CUE_RUNTIME_SCRIPT_CHECK(module.has_value());
    g_module = module.take_value();
    g_received = 0;
    auto source = assignment(k_firstScriptId);
    source.typeId = "test.pulse";
    auto receiver = assignment(k_secondScriptId);
    receiver.typeId = "test.receiver";
    auto prepared = prepare({source, receiver});
    CUE_RUNTIME_SCRIPT_CHECK(prepared.has_value());
    auto fixture = prepared.take_value();
    auto &world = *fixture.world;
    CUE_RUNTIME_SCRIPT_CHECK(world.add_component<CameraComponent>(fixture.entity).has_value());
    CUE_RUNTIME_SCRIPT_CHECK(world.add_component<MeshRendererComponent>(fixture.entity).has_value());
    CUE_RUNTIME_SCRIPT_CHECK(world.initialize().has_value());
    CUE_RUNTIME_SCRIPT_CHECK(world.update(1ms).has_value());
    const auto receivedBefore = g_received;
    CUE_RUNTIME_SCRIPT_CHECK(world.update(1ms).has_value() && g_received == receivedBefore + 2);
    auto scripts = world.get_component<ScriptComponent>(fixture.entity);
    CUE_RUNTIME_SCRIPT_CHECK(scripts.has_value());
    receiver.isEnabled = false;
    CUE_RUNTIME_SCRIPT_CHECK((*scripts.try_value())->set_scripts({source, receiver}).has_value());
    CUE_RUNTIME_SCRIPT_CHECK(world.update(1ms).has_value() && g_received == receivedBefore + 2);
    receiver.isEnabled = true;
    CUE_RUNTIME_SCRIPT_CHECK((*scripts.try_value())->set_scripts({source, receiver}).has_value());
    CUE_RUNTIME_SCRIPT_CHECK(world.update(1ms).has_value() && g_received == receivedBefore + 4);
    CUE_RUNTIME_SCRIPT_CHECK((*scripts.try_value())->set_scripts({source}).has_value());
    CUE_RUNTIME_SCRIPT_CHECK(world.update(1ms).has_value() && g_received == receivedBefore + 4);
    CUE_RUNTIME_SCRIPT_CHECK(world.shutdown().has_value());
    return 0;
}
} // namespace

/// @brief GPU / Editor を起動せず SDK・C ABI・World・Scene の接続契約を検証する
int main()
{
    for (auto test :
         {test_lifecycle, test_multiple_and_instances, test_diagnostics, test_failures, test_configurations,
          test_script_communication})
    {
        const int result = test();
        if (result)
            return result;
    }
    return 0;
}
