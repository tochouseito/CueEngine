#include <ScriptRuntime/ScriptModule.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

#include <Runtime/Components/TransformComponent.h>
#include <Runtime/World.h>
#include <SceneRuntime/SceneInstantiation.h>
#include <ScriptRuntime/ScriptSystem.h>

extern "C" CueScriptStatus CUE_SCRIPT_CALL CueScriptGetModule(std::uint32_t, CueScriptModuleApi *) noexcept;

namespace
{
#define CHECK(...)                                                                                                     \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(__VA_ARGS__))                                                                                            \
        {                                                                                                              \
            std::fprintf(stderr, "Script module test line %d: %s\n", __LINE__, #__VA_ARGS__);                          \
            return __LINE__;                                                                                           \
        }                                                                                                              \
    } while (false)

/// @brief 動的 / 静的登録を同じ World 契約へ通し、設定変更と回転結果を確認する
int verify(std::unique_ptr<cue::RuntimeGameModule> a_module)
{
    CHECK(a_module->types().size() == 1 && a_module->types()[0].typeId == "sample.RotateCube");
    bool rejectedThread = false;
    std::thread other(
        [&]()
        {
            auto owner = a_module->check_owner();
            rejectedThread = !owner.has_value();
        });
    other.join();
    CHECK(rejectedThread);
    auto world = cue::World::create();
    CHECK(world.has_value());
    auto instance = (*world.try_value())->create_scene_instance("test-scene");
    CHECK(instance.has_value());
    auto cube = (*world.try_value())->create_entity(*instance.try_value(), "cube", "Cube");
    CHECK(cube.has_value());
    auto transform = (*world.try_value())->add_component<cue::TransformComponent>(*cube.try_value());
    CHECK(transform.has_value());
    auto component = (*world.try_value())->add_component<cue::ScriptComponent>(*cube.try_value());
    CHECK(component.has_value());
    const std::string assignment = "f0b0901d-2c24-4e28-b956-463af0a74bc5";
    auto assigned = (*component.try_value())->set_scripts({{assignment, "sample.RotateCube", 1, true, {}}});
    CHECK(assigned.has_value());
    auto system = cue::ScriptSystem::create(std::move(a_module));
    CHECK(system.has_value());
    auto *scriptSystem = system.try_value()->get();
    auto registered = (*world.try_value())->add_system(system.take_value());
    CHECK(registered.has_value());
    auto started = (*world.try_value())->initialize();
    CHECK(started.has_value());
    auto updated = (*world.try_value())->update(std::chrono::seconds(1));
    CHECK(updated.has_value());
    auto after = (*world.try_value())->get_component<cue::TransformComponent>(*cube.try_value());
    CHECK(after.has_value());
    const auto rotation = (*after.try_value())->local().rotation;
    CHECK(std::abs(rotation.y - std::sqrt(0.5f)) < 0.0001f && std::abs(rotation.w - std::sqrt(0.5f)) < 0.0001f);
    auto changed = scriptSystem->set_property(**world.try_value(), *cube.try_value(), assignment,
                                              {"isRotating", cue::ScriptPropertyKind::Boolean, 0, false});
    CHECK(changed.has_value());
    updated = (*world.try_value())->update(std::chrono::seconds(1));
    CHECK(updated.has_value());
    after = (*world.try_value())->get_component<cue::TransformComponent>(*cube.try_value());
    CHECK(after.has_value() && (*after.try_value())->local().rotation == rotation);
    auto bad = scriptSystem->set_property(**world.try_value(), *cube.try_value(), assignment,
                                          {"unknown", cue::ScriptPropertyKind::Number, 1, false});
    CHECK(!bad.has_value());
    updated = (*world.try_value())->update(std::chrono::milliseconds(16));
    CHECK(updated.has_value());
    auto stopped = (*world.try_value())->shutdown();
    CHECK(stopped.has_value());
    return 0;
}
} // namespace

/// @brief 実 DLL と製品用 Static 登録が同じ Script SDK として動作することを確認する
int main(int a_count, char **a_arguments)
{
    CHECK(a_count == 2);
    auto file = cue::Path::create(a_arguments[1]);
    CHECK(file.has_value());
    auto dynamic = cue::RuntimeGameModule::load(*file.try_value());
    if (!dynamic.has_value())
        std::fprintf(stderr, "%s\n", dynamic.try_error()->operation.c_str());
    CHECK(dynamic.has_value());
    auto result = verify(dynamic.take_value());
    if (result)
        return result;
    auto linked = cue::RuntimeGameModule::from_static(CueScriptGetModule);
    CHECK(linked.has_value());
    // 同構成の既定 Load を保ち、明示した選択値と DLL の構成が一致した場合だけ公開する
    const auto nativeConfiguration = linked.try_value()->get()->api().configuration;
    for (const auto configuration :
         {cue::ScriptConfiguration::Debug, cue::ScriptConfiguration::Development, cue::ScriptConfiguration::Release})
    {
        auto checked = cue::RuntimeGameModule::load(*file.try_value(), configuration);
        CHECK(checked.has_value() == (static_cast<std::uint32_t>(configuration) == nativeConfiguration));
        if (!checked.has_value())
            CHECK(checked.try_error()->operation == "GameModule.abi_mismatch");
    }
    auto unknown = cue::RuntimeGameModule::load(*file.try_value(), static_cast<cue::ScriptConfiguration>(99));
    CHECK(!unknown.has_value() && unknown.try_error()->operation == "GameModule.configuration");
    return verify(linked.take_value());
}
