#include <Script/ModuleEntry.h>

#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace
{
#define CUE_SCRIPT_CHECK(...)                                                                                          \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(__VA_ARGS__))                                                                                            \
        {                                                                                                              \
            std::fprintf(stderr, "Script SDK check failed at %d: %s\n", __LINE__, #__VA_ARGS__);                       \
            return __LINE__;                                                                                           \
        }                                                                                                              \
    } while (false)

static_assert(std::is_standard_layout_v<CueScriptModuleApi>);
static_assert(std::is_trivially_copyable_v<CueScriptTransform>);

struct Engine final
{
    CueScriptTransform transform{{1, 2, 3}, {0, 0, 0, 1}, {1, 1, 1}};
    std::uint32_t reports = 0;

    /// @brief Fake World の世代を検証する
    static CueScriptStatus CUE_SCRIPT_CALL alive(void *, CueScriptEntity a_entity, std::uint32_t *a_alive)
    {
        *a_alive = a_entity.world == 11 && a_entity.index == 2 && a_entity.generation == 0x100000001ULL;
        return {};
    }
    /// @brief Engine 側の値だけを複写し、生 Pointer を返さない
    static CueScriptStatus CUE_SCRIPT_CALL get(void *a_context, CueScriptEntity, CueScriptTransform *a_value)
    {
        *a_value = static_cast<Engine *>(a_context)->transform;
        return {};
    }
    /// @brief Script の更新結果を Fake World へ反映する
    static CueScriptStatus CUE_SCRIPT_CALL set(void *a_context, CueScriptEntity, const CueScriptTransform *a_value)
    {
        static_cast<Engine *>(a_context)->transform = *a_value;
        return {};
    }
    /// @brief SceneInstance の違いが検索に渡されることを確認する
    static CueScriptStatus CUE_SCRIPT_CALL find(void *, CueScriptSceneInstance a_instance, CueScriptString,
                                                CueScriptEntity *a_entity)
    {
        *a_entity = {a_instance.world, a_instance.index, a_instance.generation};
        return {};
    }
    /// @brief stop の回数を外部出力なしで数える
    static CueScriptStatus CUE_SCRIPT_CALL report(void *a_context, std::uint32_t, CueScriptString)
    {
        ++static_cast<Engine *>(a_context)->reports;
        return {};
    }
    /// @brief 明示的な context で Fake Engine API を構築する
    CueScriptEngineApi api()
    {
        // 新しい Table の関数を公開し、この Fixture が持たない Component や Script は欠損扱いにする
        return {CUE_SCRIPT_ABI_VERSION, sizeof(CueScriptEngineApi), this, alive, get, set, find, report,
                [](void *, CueScriptEntity, CueScriptCamera *) { return CueScriptStatus{1, "Fixture.camera_missing"}; },
                [](void *, CueScriptEntity, CueScriptMeshRenderer *)
                { return CueScriptStatus{1, "Fixture.mesh_missing"}; },
                [](void *, CueScriptEntity, CueScriptString, CueScriptString, CueScriptInstance *)
                { return CueScriptStatus{1, "Fixture.script_missing"}; },
                [](void *, CueScriptEntity a_entity, CueScriptSceneInstance *a_instance)
                {
                    *a_instance = {a_entity.world, a_entity.index, a_entity.generation};
                    return CueScriptStatus{};
                }};
    }
};

class Move final : public cue::Behaviour
{
  public:
    /// @brief 登録された既定値と差分の両方を受け取る
    cue::Result<void> set_property(std::string_view a_id, const cue::ScriptPropertyValue &a_value) override
    {
        if (a_id != "speed")
            return cue::Result<void>::failure({cue::ErrorCategory::InvalidArgument, "Move.property"});
        m_speed = a_value.number;
        return cue::Result<void>::success();
    }
    /// @brief 大きな世代値と借用値の Transform 往復を確認する
    cue::Result<void> start(cue::GameObject &a_self) override
    {
        auto alive = a_self.is_alive();
        if (!alive.has_value() || !*alive.try_value())
            return cue::Result<void>::failure({cue::ErrorCategory::InvalidState, "Move.entity"});
        return cue::Result<void>::success();
    }
    /// @brief 速度と DeltaTime で X を進め、負速度では C++ 例外境界を検証する
    cue::Result<void> update(cue::GameObject &a_self, const cue::ScriptUpdateContext &a_context) override
    {
        if (m_speed < 0)
            throw std::runtime_error("test update failure");
        auto result = a_self.transform();
        if (!result.has_value())
            return cue::Result<void>::failure(*result.try_error());
        auto transform = *result.try_value();
        transform.position.x +=
            static_cast<float>(m_speed * std::chrono::duration<double>(a_context.deltaTime).count());
        return a_self.set_transform(transform);
    }
    /// @brief 停止が一度だけ実行されたことを通知する
    cue::Result<void> stop(cue::GameObject &a_self) override
    {
        return a_self.report(0, "stopped");
    }

  private:
    double m_speed = 0;
};

class FailedStart final : public cue::Behaviour
{
  public:
    /// @brief 部分初期化の失敗を返す
    cue::Result<void> start(cue::GameObject &) override
    {
        return cue::Result<void>::failure({cue::ErrorCategory::PlatformFailure, "FailedStart.start"});
    }
    /// @brief 開始に失敗した Instance は更新対象にしない
    cue::Result<void> update(cue::GameObject &, const cue::ScriptUpdateContext &) override
    {
        return cue::Result<void>::success();
    }
    /// @brief 開始失敗でも後始末の入口が呼べることを確認する
    cue::Result<void> stop(cue::GameObject &a_self) override
    {
        return a_self.report(0, "partial cleanup");
    }
};

/// @brief 型 ID を安定させ、表示名と独立した Property を一つ登録する
cue::ScriptTypeDescriptor move_type()
{
    cue::ScriptTypeDescriptor type;
    type.typeId = "test.move";
    type.displayName = "Move";
    type.properties.push_back({"speed", "Speed", cue::ScriptPropertyKind::Number, 2.5, false});
    return type;
}

/// @brief ABI 不一致・既定値・生存・例外・停止・失効 Token を一連で確認する
int test_module()
{
    auto created =
        cue::ScriptModule::create({cue::make_script_registration<Move>(move_type()),
                                   cue::make_script_registration<FailedStart>({"test.fail", "Fail", 1, {}})});
    CUE_SCRIPT_CHECK(created.has_value());
    auto module = created.take_value();
    CueScriptModuleApi api{};
    CUE_SCRIPT_CHECK(module->get_api(CUE_SCRIPT_ABI_VERSION + 1, api).code != 0 && api.context == nullptr);
    CUE_SCRIPT_CHECK(module->get_api(CUE_SCRIPT_ABI_VERSION, api).code == 0);
    CUE_SCRIPT_CHECK(api.type_count(api.context) == 2 && api.pointerSize == sizeof(void *));
    CueScriptTypeInfo type{};
    CUE_SCRIPT_CHECK(api.type_info(api.context, 0, &type).code == 0 && type.propertyCount == 1);
    CUE_SCRIPT_CHECK(std::string_view(type.typeId.data, type.typeId.size) == "test.move");
    CueScriptPropertyInfo property{};
    CUE_SCRIPT_CHECK(api.property_info(api.context, 0, 0, &property).code == 0 && property.defaultValue.number == 2.5);
    CUE_SCRIPT_CHECK(api.property_info(api.context, 0, 1, &property).code != 0);
    CueScriptInstance instance = 0;
    CUE_SCRIPT_CHECK(api.create(api.context, 0, &instance).code == 0 && instance != 0);
    Engine engine;
    const auto engineApi = engine.api();
    const CueScriptEntity entity{11, 2, 0x100000001ULL};
    CUE_SCRIPT_CHECK(api.update(api.context, instance, &engineApi, entity, {1, 0}).code != 0);
    CUE_SCRIPT_CHECK(api.start(api.context, instance, &engineApi, entity).code == 0);
    CUE_SCRIPT_CHECK(api.start(api.context, instance, &engineApi, entity).code != 0);
    CUE_SCRIPT_CHECK(api.destroy(api.context, instance).code != 0);
    CUE_SCRIPT_CHECK(api.update(api.context, instance, &engineApi, entity, {1000000000, 1}).code == 0);
    CUE_SCRIPT_CHECK(engine.transform.position[0] == 3.5f);
    CUE_SCRIPT_CHECK(
        api.set_property(api.context, instance, {"speed", 5}, {1, std::numeric_limits<double>::quiet_NaN(), 0}).code !=
        0);
    CUE_SCRIPT_CHECK(api.set_property(api.context, instance, {"speed", 5}, {2, 0, 1}).code != 0);
    CUE_SCRIPT_CHECK(api.set_property(api.context, instance, {"speed", 5}, {1, -1, 0}).code == 0);
    auto exception = api.update(api.context, instance, &engineApi, entity, {1000000000, 2});
    CUE_SCRIPT_CHECK(exception.code == 3 && std::string_view(exception.message) == "Script.unhandled_exception");
    CUE_SCRIPT_CHECK(api.set_property(api.context, instance, {"speed", 5}, {1, 5, 0}).code != 0);
    CUE_SCRIPT_CHECK(api.update(api.context, instance, &engineApi, entity, {1000000000, 3}).code != 0);
    CUE_SCRIPT_CHECK(api.stop(api.context, instance, &engineApi, entity).code == 0);
    CUE_SCRIPT_CHECK(api.stop(api.context, instance, &engineApi, entity).code == 0 && engine.reports == 1);
    CUE_SCRIPT_CHECK(api.update(api.context, instance, &engineApi, entity, {1, 3}).code != 0);
    CUE_SCRIPT_CHECK(api.destroy(api.context, instance).code == 0);
    CUE_SCRIPT_CHECK(api.destroy(api.context, instance).code != 0);
    CueScriptInstance partial = 0;
    CUE_SCRIPT_CHECK(api.create(api.context, 1, &partial).code == 0 && partial != instance);
    CUE_SCRIPT_CHECK(api.start(api.context, partial, &engineApi, entity).code == 2);
    CUE_SCRIPT_CHECK(api.update(api.context, partial, &engineApi, entity, {1000000000, 1}).code != 0);
    CUE_SCRIPT_CHECK(api.stop(api.context, partial, &engineApi, entity).code == 0 && engine.reports == 2);
    CUE_SCRIPT_CHECK(api.destroy(api.context, partial).code == 0);
    auto duplicate = cue::ScriptModule::create(
        {cue::make_script_registration<Move>(move_type()), cue::make_script_registration<Move>(move_type())});
    CUE_SCRIPT_CHECK(!duplicate.has_value());
    return 0;
}

std::uint32_t registrationCalls = 0;
bool shouldThrowRegistration = true;

/// @brief 登録式の評価時の例外と初期化の一回性を検証する
cue::ScriptRegistration entry_registration()
{
    ++registrationCalls;
    if (shouldThrowRegistration)
        throw std::runtime_error("test registration failure");
    return cue::make_script_registration<Move>(move_type());
}
} // namespace

// 二つの型と Property 付き登録を、実際に Export する入口から取得する
CUE_DEFINE_SCRIPT_MODULE(entry_registration(), cue::make_script_registration<FailedStart>({"test.fail", "Fail", 1, {}}))

namespace
{
/// @brief Null 入力・登録例外・ABI 不一致・複数型・借用寿命を共通入口から検証する
int test_module_entry()
{
    static_assert(noexcept(CueScriptGetModule(CUE_SCRIPT_ABI_VERSION, nullptr)));
    CueScriptModuleApi api{};
    CUE_SCRIPT_CHECK(CueScriptGetModule(CUE_SCRIPT_ABI_VERSION, nullptr).code == 1);
    CUE_SCRIPT_CHECK(registrationCalls == 0);
    const auto exception = CueScriptGetModule(CUE_SCRIPT_ABI_VERSION, &api);
    CUE_SCRIPT_CHECK(exception.code == 3 && std::string_view(exception.message) == "Script.unhandled_exception");
    CUE_SCRIPT_CHECK(registrationCalls == 1 && api.context == nullptr);

    // 登録中の例外は Module を公開せず、次の呼出で初期化を再試行する
    shouldThrowRegistration = false;
    CUE_SCRIPT_CHECK(CueScriptGetModule(CUE_SCRIPT_ABI_VERSION + 1, &api).code == 1);
    CUE_SCRIPT_CHECK(api.context == nullptr && registrationCalls == 2);
    CUE_SCRIPT_CHECK(CueScriptGetModule(CUE_SCRIPT_ABI_VERSION, &api).code == 0);
    CUE_SCRIPT_CHECK(api.type_count(api.context) == 2);
    CueScriptTypeInfo type{};
    CUE_SCRIPT_CHECK(api.type_info(api.context, 0, &type).code == 0);
    CUE_SCRIPT_CHECK(std::string_view(type.typeId.data, type.typeId.size) == "test.move");
    CueScriptPropertyInfo property{};
    CUE_SCRIPT_CHECK(api.property_info(api.context, 0, 0, &property).code == 0);
    CUE_SCRIPT_CHECK(property.defaultValue.number == 2.5);
    CUE_SCRIPT_CHECK(api.type_info(api.context, 1, &type).code == 0);
    CUE_SCRIPT_CHECK(std::string_view(type.typeId.data, type.typeId.size) == "test.fail");

    CueScriptModuleApi repeated{};
    CUE_SCRIPT_CHECK(CueScriptGetModule(CUE_SCRIPT_ABI_VERSION, &repeated).code == 0);
    CUE_SCRIPT_CHECK(repeated.context == api.context && registrationCalls == 2);
    for (std::uint32_t index = 0; index < 2; ++index)
    {
        CueScriptInstance instance = 0;
        CUE_SCRIPT_CHECK(api.create(api.context, index, &instance).code == 0);
        CUE_SCRIPT_CHECK(repeated.destroy(repeated.context, instance).code == 0);
    }
    return 0;
}
} // namespace

/// @brief Engine / GPU を起動せず SDK の ABI と所有権契約を検証する
int main()
{
    const auto result = test_module();
    return result != 0 ? result : test_module_entry();
}
