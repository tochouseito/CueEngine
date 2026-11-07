#include <GameRuntime/GameSession.h>

#include <chrono>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <thread>

#include <Runtime/Components/TransformComponent.h>
#include <Runtime/World.h>
#include <Scene/Scene.h>
#include <Script/Module.h>

namespace
{
#define CHECK(...)                                                                                                     \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(__VA_ARGS__))                                                                                            \
        {                                                                                                              \
            std::fprintf(stderr, "Game Session failed at %d: %s\n", __LINE__, #__VA_ARGS__);                           \
            return __LINE__;                                                                                           \
        }                                                                                                              \
    } while (false)

unsigned g_starts = 0;
unsigned g_stops = 0;
unsigned g_destroys = 0;
bool g_shouldThrow = false;
std::unique_ptr<cue::ScriptModule> g_module;
constexpr const char *k_script = "3ce1feb2-e711-4ca3-9698-2b32e0d9637c";
constexpr const char *k_cube = "9a00d731-7759-424c-9ade-fd086d624551";

/// @brief 同じ SDK を使う Play / 製品 Session の Lifecycle と Transform 操作を確認する
class Moving final : public cue::Behaviour
{
  public:
    /// @brief Instance が生成 Module 内で破棄されたことを数える
    ~Moving() override
    {
        ++g_destroys;
    }
    /// @brief 一度だけ開始する
    cue::Result<void> start(cue::GameObject &) override
    {
        ++g_starts;
        return cue::Result<void>::success();
    }
    /// @brief 実行例外と所有 Transform 値の読戻しを検証する
    cue::Result<void> update(cue::GameObject &a_object, const cue::ScriptUpdateContext &a_context) override
    {
        if (g_shouldThrow)
            throw std::runtime_error("session fixture");
        auto transform = a_object.transform();
        if (!transform.has_value())
            return cue::Result<void>::failure(*transform.try_error());
        transform.try_value()->position.x += m_speed * std::chrono::duration<float>(a_context.deltaTime).count();
        return a_object.set_transform(*transform.try_value());
    }
    /// @brief World の停止と再呼出しの分離を観測する
    cue::Result<void> stop(cue::GameObject &) override
    {
        ++g_stops;
        return cue::Result<void>::success();
    }
    /// @brief Inspector と共通の型付き設定を受け取る
    cue::Result<void> set_property(std::string_view a_id, const cue::ScriptPropertyValue &a_value) override
    {
        if (a_id != "speed")
            return cue::Result<void>::failure({cue::ErrorCategory::InvalidArgument, "Moving.property"});
        m_speed = static_cast<float>(a_value.number);
        return cue::Result<void>::success();
    }

  private:
    float m_speed = 1;
};

/// @brief 静的登録の C 入口を本番と同じ Adapter へ渡す
CueScriptStatus CUE_SCRIPT_CALL entry(std::uint32_t a_version, CueScriptModuleApi *a_api)
{
    return g_module->get_api(a_version, *a_api);
}

/// @brief 検証用 Camera と Script 付き Cube を所有 Scene として生成する
cue::Result<cue::Scene> scene()
{
    cue::SceneDescriptor descriptor;
    descriptor.sceneId = "8d94c2b1-9b6a-435c-ade7-c01f473c3951";
    descriptor.name = "Session";
    cue::SceneObjectDescriptor cube;
    cube.objectId = k_cube;
    cube.name = "Cube";
    cube.transform = cue::math::floatTransform{};
    cube.mesh = cue::SceneMeshDescriptor{};
    cube.scripts = {{k_script, "test.Moving", 1, true, {}}};
    descriptor.objects.push_back(std::move(cube));
    cue::SceneObjectDescriptor camera;
    camera.objectId = "35f6ad62-88f0-49dc-b067-f318c116eb61";
    camera.name = "Camera";
    camera.transform = cue::math::floatTransform{};
    camera.transform->position = {0, 0, 3};
    camera.camera = cue::SceneCameraDescriptor{};
    descriptor.objects.push_back(std::move(camera));
    return cue::Scene::create(std::move(descriptor));
}
} // namespace

/// @brief 更新・抽出・設定変更・異常停止・Owner と Script 回収を実行 Session 単位で確認する
int main()
{
    auto module = cue::ScriptModule::create({cue::make_script_registration<Moving>(
        {"test.Moving", "Moving", 1, {{"speed", "Speed", cue::ScriptPropertyKind::Number, 1, false}}})});
    CHECK(module.has_value());
    g_module = module.take_value();
    auto definition = scene();
    CHECK(definition.has_value());
    auto runtimeModule = cue::RuntimeGameModule::from_static(entry);
    CHECK(runtimeModule.has_value());
    auto created = cue::GameSession::create(*definition.try_value(), runtimeModule.take_value());
    CHECK(created.has_value());
    auto session = created.take_value();
    CHECK(g_starts == 1 && session->completed_ticks() == 0);
    auto initial = session->snapshot();
    CHECK(initial.has_value() && initial.try_value()->camera && initial.try_value()->objects.size() == 1);
    CHECK(initial.try_value()->objects.front().world.values[3][0] == 0);
    bool rejectedThread = false;
    std::thread wrong(
        [&]()
        {
            auto updated = session->update(std::chrono::seconds(1));
            rejectedThread = !updated.has_value();
        });
    wrong.join();
    CHECK(rejectedThread && session->completed_ticks() == 0);
    CHECK(session->update(std::chrono::seconds(1)).has_value());
    auto snapshot = session->snapshot();
    CHECK(snapshot.has_value() && snapshot.try_value()->objects.front().world.values[3][0] == 1);
    CHECK(session->set_property(k_cube, k_script, {"speed", cue::ScriptPropertyKind::Number, 2, false}).has_value());
    CHECK(session->update(std::chrono::seconds(1)).has_value());
    snapshot = session->snapshot();
    CHECK(snapshot.has_value() && snapshot.try_value()->objects.front().world.values[3][0] == 3);
    CHECK(session->completed_ticks() == 2);
    CHECK(session->shutdown().has_value() && session->shutdown().has_value());
    CHECK(g_stops == 1 && g_destroys == 1 && !session->world());
    CHECK(!session->update(std::chrono::seconds(1)).has_value());
    CHECK(!session->snapshot().has_value());
    runtimeModule = cue::RuntimeGameModule::from_static(entry);
    CHECK(runtimeModule.has_value());
    created = cue::GameSession::create(*definition.try_value(), runtimeModule.take_value());
    CHECK(created.has_value());
    session = created.take_value();
    g_shouldThrow = true;
    CHECK(!session->update(std::chrono::seconds(1)).has_value());
    CHECK(!session->world() && !session->snapshot().has_value());
    CHECK(!session->shutdown().has_value());
    CHECK(g_stops == 2 && g_destroys == 2);
    return 0;
}
