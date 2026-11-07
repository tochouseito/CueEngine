#include <ScriptRuntime/ScriptSystem.h>

#include <algorithm>
#include <cstring>
#include <optional>
#include <utility>

#include <Foundation/Utf8.h>
#include <Runtime/Components/TransformComponent.h>
#include <Runtime/World.h>
#include <Scene/Scene.h>

namespace cue
{
namespace
{
/// @brief Callback の固定長診断を例外なしで組み立てる
CueScriptStatus status(std::uint32_t a_code, std::string_view a_message = {}) noexcept
{
    CueScriptStatus result{};
    result.code = a_code;
    const auto length = (std::min)(a_message.size(), sizeof(result.message) - 1);
    if (length)
        std::memcpy(result.message, a_message.data(), length);
    return result;
}
/// @brief Engine の Result を境界内で固定長の Status へ変換する
CueScriptStatus status(const Error &a_error) noexcept
{
    return status(a_error.category == ErrorCategory::Fatal             ? 3u
                  : a_error.category == ErrorCategory::PlatformFailure ? 2u
                                                                       : 1u,
                  a_error.operation);
}
/// @brief Game Module の Status を所有 Result へ変換する
Result<void> checked(CueScriptStatus a_status)
{
    if (a_status.code == 0)
        return Result<void>::success();
    const auto end = std::find(std::begin(a_status.message), std::end(a_status.message), '\0');
    return Result<void>::failure({a_status.code == 1   ? ErrorCategory::InvalidArgument
                                  : a_status.code == 2 ? ErrorCategory::PlatformFailure
                                                       : ErrorCategory::Fatal,
                                  "Script:" + std::string(a_status.message, end), a_status.code});
}
/// @brief World を借用せず、ABI の値から内部 Handle を復元する
EntityHandle entity(CueScriptEntity a_value) noexcept
{
    return {a_value.world, a_value.index, a_value.generation};
}
/// @brief World の生 Pointer を渡さず Handle の値だけを複写する
CueScriptEntity entity(EntityHandle a_value) noexcept
{
    return {a_value.world, a_value.index, a_value.generation};
}
/// @brief 型付き Property を ABI の固定幅値へ変換する
CueScriptPropertyValue property_value(const ScriptPropertyValue &a_value) noexcept
{
    return {static_cast<std::uint32_t>(a_value.kind), a_value.number, a_value.boolean ? 1u : 0u};
}
/// @brief 文字列を Callback 中だけ借用する
CueScriptString text(std::string_view a_value) noexcept
{
    return {a_value.data(), static_cast<std::uint32_t>(a_value.size())};
}
} // namespace

Result<void> ScriptComponent::set_scripts(std::vector<ScriptDescriptor> a_scripts)
{
    if (a_scripts.size() > k_maxScriptsPerObject)
        return Result<void>::failure({ErrorCategory::InvalidArgument, "ScriptComponent.limit"});
    for (std::size_t index = 0; index < a_scripts.size(); ++index)
    {
        auto valid = validate_script(a_scripts[index]);
        if (!valid.has_value())
            return valid;
        for (std::size_t previous = 0; previous < index; ++previous)
            if (a_scripts[previous].instanceId == a_scripts[index].instanceId)
                return Result<void>::failure({ErrorCategory::InvalidArgument, "ScriptComponent.duplicate"});
    }
    m_scripts = std::move(a_scripts);
    return Result<void>::success();
}
const std::vector<ScriptDescriptor> &ScriptComponent::scripts() const noexcept
{
    return m_scripts;
}

struct ScriptSystem::State final
{
    struct Record final
    {
        EntityHandle entity;
        ScriptDescriptor descriptor;
        CueScriptInstance token = 0;
        bool hasStarted = false;
        bool hasStopped = false;
    };
    struct Desired final
    {
        EntityHandle entity;
        ScriptDescriptor descriptor;
        bool isActive;
    };

    // Module は最初に宣言し、全 Token と Callback の保持値より後に破棄する
    std::unique_ptr<RuntimeGameModule> module;
    std::vector<Record> records;
    scriptDiagnosticCallback reportCallback;
    World *world = nullptr;
    World *callbackWorld = nullptr;
    bool isRunning = false;
    bool isBusy = false;
    CueScriptEngineApi engine{};

    /// @brief Game Callback の間だけ World API を有効にし、例外の経路でも借用を解除する
    class CallbackScope final
    {
      public:
        /// @brief 呼出し対象の World を一時的に借用する
        CallbackScope(State &a_state, World &a_world) noexcept : m_state(a_state)
        {
            m_state.callbackWorld = &a_world;
        }
        /// @brief Callback の外では Engine API の利用を拒否する
        ~CallbackScope()
        {
            m_state.callbackWorld = nullptr;
        }

      private:
        State &m_state;
    };

    /// @brief 失効した Context と誤 Thread を ABI の入口で検証する
    static World *callback_world(void *a_context) noexcept
    {
        auto *state = static_cast<State *>(a_context);
        if (!state || !state->callbackWorld)
            return nullptr;
        return state->callbackWorld;
    }
    /// @brief 例外を DLL の外へ出さず、生存を世代付き Handle で検証する
    static CueScriptStatus CUE_SCRIPT_CALL is_alive(void *a_context, CueScriptEntity a_entity,
                                                    std::uint32_t *a_alive) noexcept
    {
        try
        {
            auto *world = callback_world(a_context);
            if (!world || !a_alive)
                return status(1, "Script.callback_context");
            auto result = world->is_alive(entity(a_entity));
            if (!result.has_value())
                return status(*result.try_error());
            *a_alive = *result.try_value() ? 1u : 0u;
            return status(0);
        }
        catch (...)
        {
            return status(3, "Script.is_alive.exception");
        }
    }
    /// @brief Component を呼出中だけ借用し、数学型の所有値を ABI へ複写する
    static CueScriptStatus CUE_SCRIPT_CALL get_transform(void *a_context, CueScriptEntity a_entity,
                                                         CueScriptTransform *a_value) noexcept
    {
        try
        {
            auto *world = callback_world(a_context);
            if (!world || !a_value)
                return status(1, "Script.callback_context");
            auto component = world->get_component<TransformComponent>(entity(a_entity));
            if (!component.has_value())
                return status(*component.try_error());
            const auto value = (*component.try_value())->local();
            *a_value = {{value.position.x, value.position.y, value.position.z},
                        {value.rotation.x, value.rotation.y, value.rotation.z, value.rotation.w},
                        {value.scale.x, value.scale.y, value.scale.z}};
            return status(0);
        }
        catch (...)
        {
            return status(3, "Script.get_transform.exception");
        }
    }
    /// @brief Math の検証を通して Transform を変更し、次の World 同期で行列を確定する
    static CueScriptStatus CUE_SCRIPT_CALL set_transform(void *a_context, CueScriptEntity a_entity,
                                                         const CueScriptTransform *a_value) noexcept
    {
        try
        {
            auto *world = callback_world(a_context);
            if (!world || !a_value)
                return status(1, "Script.callback_context");
            auto component = world->get_component<TransformComponent>(entity(a_entity));
            if (!component.has_value())
                return status(*component.try_error());
            auto result = (*component.try_value())
                              ->set_local({{a_value->position[0], a_value->position[1], a_value->position[2]},
                                           {a_value->rotation[0], a_value->rotation[1], a_value->rotation[2],
                                            a_value->rotation[3]},
                                           {a_value->scale[0], a_value->scale[1], a_value->scale[2]}});
            return result.has_value() ? status(0) : status(*result.try_error());
        }
        catch (...)
        {
            return status(3, "Script.set_transform.exception");
        }
    }
    /// @brief SceneInstance を明示して同名 Object の別 Instance 混同を防ぐ
    static CueScriptStatus CUE_SCRIPT_CALL find_by_name(void *a_context, CueScriptSceneInstance a_instance,
                                                        CueScriptString a_name, CueScriptEntity *a_entity) noexcept
    {
        try
        {
            auto *world = callback_world(a_context);
            if (!world || !a_entity || !a_name.data || a_name.size == 0 || a_name.size > 1024)
                return status(1, "Script.find.arguments");
            auto found = world->find_by_name({a_instance.world, a_instance.index, a_instance.generation},
                                             std::string_view(a_name.data, a_name.size));
            if (!found.has_value())
                return status(*found.try_error());
            *a_entity = entity(*found.try_value());
            return status(0);
        }
        catch (...)
        {
            return status(3, "Script.find.exception");
        }
    }
    /// @brief 明示要求の診断だけを Host の配送先へ送る。File / Platform 出力を直接決めない
    static CueScriptStatus CUE_SCRIPT_CALL report(void *a_context, std::uint32_t a_severity,
                                                  CueScriptString a_message) noexcept
    {
        try
        {
            if (!callback_world(a_context) || !a_message.data || a_message.size > 4096 || a_severity > 3)
                return status(1, "Script.report.arguments");
            std::string message(a_message.data, a_message.size);
            if (!is_valid_utf8_text(message))
                return status(1, "Script.report.text");
            auto &state = *static_cast<State *>(a_context);
            if (!state.reportCallback)
                return status(1, "Script.report.not_configured");
            state.reportCallback(a_severity, message);
            return status(0);
        }
        catch (...)
        {
            return status(3, "Script.report.exception");
        }
    }
    /// @brief 既定値を補完し、型不一致や未知 Property を実行前に検出する
    Result<ScriptDescriptor> resolve(const ScriptDescriptor &a_descriptor) const
    {
        const auto &types = module->types();
        const auto type = std::find_if(types.begin(), types.end(),
                                       [&](const auto &a_type) { return a_type.typeId == a_descriptor.typeId; });
        if (type == types.end())
            return Result<ScriptDescriptor>::failure(
                {ErrorCategory::InvalidArgument, "Script.type_missing:" + a_descriptor.typeId});
        auto valid = validate_script_binding(a_descriptor, *type);
        if (!valid.has_value())
            return Result<ScriptDescriptor>::failure(*valid.try_error());
        auto resolved = a_descriptor;
        resolved.properties.clear();
        for (const auto &property : type->properties)
        {
            const auto value =
                std::find_if(a_descriptor.properties.begin(), a_descriptor.properties.end(),
                             [&](const auto &a_value) { return a_value.propertyId == property.propertyId; });
            resolved.properties.push_back(value != a_descriptor.properties.end()
                                              ? *value
                                              : ScriptPropertyValue{property.propertyId, property.kind,
                                                                    property.defaultNumber, property.defaultBoolean});
        }
        return Result<ScriptDescriptor>::success(std::move(resolved));
    }
    /// @brief 停止失敗でも destroy へ進み、Token を二重に呼ばない
    Result<void> release(Record &a_record, World &a_world)
    {
        std::optional<Error> failure;
        const auto &api = module->api();
        CallbackScope scope(*this, a_world);
        if (a_record.hasStarted && !a_record.hasStopped)
        {
            a_record.hasStopped = true;
            auto stopped = checked(api.stop(api.context, a_record.token, &engine, entity(a_record.entity)));
            if (!stopped.has_value())
                failure = *stopped.try_error();
        }
        if (a_record.token)
        {
            auto destroyed = checked(api.destroy(api.context, a_record.token));
            if (destroyed.has_value())
                a_record.token = 0;
            else
                module->retain_until_process_exit();
            if (!destroyed.has_value() && !failure)
                failure = *destroyed.try_error();
        }
        return failure ? Result<void>::failure(*failure) : Result<void>::success();
    }
    /// @brief Component の借用を終えてから Game を呼ぶため、所有 Snapshot を先に構築する
    Result<std::vector<Desired>> desired(World &a_world)
    {
        auto handles = a_world.entities();
        if (!handles.has_value())
            return Result<std::vector<Desired>>::failure(*handles.try_error());
        std::vector<Desired> result;
        for (auto handle : *handles.try_value())
        {
            auto component = a_world.get_component<ScriptComponent>(handle);
            if (!component.has_value())
            {
                if (component.try_error()->operation == "World.component_missing")
                    continue;
                return Result<std::vector<Desired>>::failure(*component.try_error());
            }
            auto active = a_world.is_active(handle);
            if (!active.has_value())
                return Result<std::vector<Desired>>::failure(*active.try_error());
            for (const auto &script : (*component.try_value())->scripts())
            {
                auto resolved = resolve(script);
                if (!resolved.has_value())
                    return Result<std::vector<Desired>>::failure(*resolved.try_error());
                const bool enabled = *active.try_value() && (*component.try_value())->is_active() && script.isEnabled;
                result.push_back({handle, resolved.take_value(), enabled});
            }
        }
        return Result<std::vector<Desired>>::success(std::move(result));
    }
    /// @brief 型と割当 ID を照合し、既存 Instance の Property だけを更新する
    Result<void> reconcile(World &a_world, const std::vector<Desired> &a_desired)
    {
        const auto &api = module->api();
        // ABI Token を作る前に配列を確保し、確保失敗で所有 Token を失わない
        records.reserve(records.size() + a_desired.size());
        for (std::size_t index = records.size(); index > 0; --index)
        {
            auto &record = records[index - 1];
            const auto found = std::find_if(a_desired.begin(), a_desired.end(),
                                            [&](const Desired &a_value)
                                            {
                                                return a_value.entity == record.entity &&
                                                       a_value.descriptor.instanceId == record.descriptor.instanceId &&
                                                       a_value.descriptor.typeId == record.descriptor.typeId;
                                            });
            if (found == a_desired.end())
            {
                auto removed = release(record, a_world);
                records.erase(records.begin() + static_cast<std::ptrdiff_t>(index - 1));
                if (!removed.has_value())
                    return removed;
            }
        }
        for (const auto &desired : a_desired)
        {
            auto record = std::find_if(records.begin(), records.end(),
                                       [&](const Record &a_record)
                                       {
                                           return a_record.entity == desired.entity &&
                                                  a_record.descriptor.instanceId == desired.descriptor.instanceId;
                                       });
            bool isNew = record == records.end();
            if (isNew)
            {
                Record candidate{desired.entity, desired.descriptor};
                const auto &types = module->types();
                const auto type = std::find_if(types.begin(), types.end(), [&](const auto &a_type)
                                               { return a_type.typeId == desired.descriptor.typeId; });
                auto created = checked(
                    api.create(api.context, static_cast<std::uint32_t>(type - types.begin()), &candidate.token));
                if (!created.has_value())
                {
                    if (candidate.token)
                    {
                        auto cleanup = api.destroy(api.context, candidate.token);
                        (void)cleanup;
                    }
                    return created;
                }
                if (!candidate.token)
                    return Result<void>::failure({ErrorCategory::Fatal, "Script.create.invalid_token"});
                records.push_back(std::move(candidate));
                record = records.end() - 1;
            }
            if (isNew || record->descriptor.properties != desired.descriptor.properties)
            {
                for (const auto &property : desired.descriptor.properties)
                {
                    // create は既定値を適用済み。初回は上書きが必要な設定だけを渡す
                    if (isNew)
                    {
                        const auto &types = module->types();
                        const auto type = std::find_if(types.begin(), types.end(), [&](const auto &a_type)
                                                       { return a_type.typeId == desired.descriptor.typeId; });
                        const auto initial =
                            std::find_if(type->properties.begin(), type->properties.end(), [&](const auto &a_property)
                                         { return a_property.propertyId == property.propertyId; });
                        if ((property.kind == ScriptPropertyKind::Number &&
                             property.number == initial->defaultNumber) ||
                            (property.kind == ScriptPropertyKind::Boolean &&
                             property.boolean == initial->defaultBoolean))
                            continue;
                    }
                    auto set = checked(api.set_property(api.context, record->token, text(property.propertyId),
                                                        property_value(property)));
                    if (!set.has_value())
                        return set;
                }
                record->descriptor = desired.descriptor;
            }
            if (desired.isActive && !record->hasStarted)
            {
                record->hasStarted = true;
                CallbackScope scope(*this, a_world);
                auto started = checked(api.start(api.context, record->token, &engine, entity(record->entity)));
                if (!started.has_value())
                    return started;
            }
        }
        return Result<void>::success();
    }
};

ScriptSystem::ScriptSystem(CreateToken, std::unique_ptr<State> a_state) noexcept : m_state(std::move(a_state))
{
}
ScriptSystem::~ScriptSystem()
{
    // World の通常停止は stop を含む。異常経路でも Module より先に残存 Token を回収する
    const auto &api = m_state->module->api();
    for (auto &record : m_state->records)
        if (record.token)
        {
            auto destroyed = api.destroy(api.context, record.token);
            if (destroyed.code != 0)
                m_state->module->retain_until_process_exit();
        }
}
Result<std::unique_ptr<ScriptSystem>> ScriptSystem::create(std::unique_ptr<RuntimeGameModule> a_module,
                                                           scriptDiagnosticCallback a_report)
{
    using SystemResult = Result<std::unique_ptr<ScriptSystem>>;
    try
    {
        if (!a_module)
            return SystemResult::failure({ErrorCategory::InvalidArgument, "ScriptSystem.module"});
        auto owner = a_module->check_owner();
        if (!owner.has_value())
            return SystemResult::failure(*owner.try_error());
        auto state = std::make_unique<State>();
        state->module = std::move(a_module);
        state->reportCallback = std::move(a_report);
        state->engine = {CUE_SCRIPT_ABI_VERSION, sizeof(CueScriptEngineApi), state.get(),         State::is_alive,
                         State::get_transform,   State::set_transform,       State::find_by_name, State::report};
        return SystemResult::success(std::make_unique<ScriptSystem>(CreateToken{}, std::move(state)));
    }
    catch (...)
    {
        return SystemResult::failure({ErrorCategory::Fatal, "ScriptSystem.create.exception"});
    }
}
Result<void> ScriptSystem::initialize(World &a_world)
{
    auto owner = m_state->module->check_owner();
    if (!owner.has_value())
        return owner;
    if (m_state->world || m_state->isBusy)
        return Result<void>::failure({ErrorCategory::InvalidState, "ScriptSystem.initialize.lifecycle"});
    ScopedFlag busy(m_state->isBusy);
    m_state->world = &a_world;
    m_state->isRunning = true;
    auto desired = m_state->desired(a_world);
    if (!desired.has_value())
        return Result<void>::failure(*desired.try_error());
    return m_state->reconcile(a_world, *desired.try_value());
}
Result<void> ScriptSystem::update(World &a_world, const WorldUpdateContext &a_context)
{
    auto owner = m_state->module->check_owner();
    if (!owner.has_value())
        return owner;
    if (m_state->world != &a_world || !m_state->isRunning || m_state->isBusy)
        return Result<void>::failure({ErrorCategory::InvalidState, "ScriptSystem.update.lifecycle"});
    ScopedFlag busy(m_state->isBusy);
    auto desired = m_state->desired(a_world);
    if (!desired.has_value())
        return Result<void>::failure(*desired.try_error());
    auto reconciled = m_state->reconcile(a_world, *desired.try_value());
    if (!reconciled.has_value())
        return reconciled;
    const auto &api = m_state->module->api();
    for (const auto &item : *desired.try_value())
    {
        if (!item.isActive)
            continue;
        const auto record = std::find_if(
            m_state->records.begin(), m_state->records.end(), [&](const auto &a_record)
            { return a_record.entity == item.entity && a_record.descriptor.instanceId == item.descriptor.instanceId; });
        State::CallbackScope scope(*m_state, a_world);
        auto result = checked(api.update(api.context, record->token, &m_state->engine, entity(item.entity),
                                         {a_context.deltaTime.count(), a_context.tick}));
        if (!result.has_value())
            return result;
    }
    return Result<void>::success();
}
Result<void> ScriptSystem::shutdown(World &a_world)
{
    auto owner = m_state->module->check_owner();
    if (!owner.has_value())
        return owner;
    if (m_state->world != &a_world || m_state->isBusy)
        return Result<void>::failure({ErrorCategory::InvalidState, "ScriptSystem.shutdown.lifecycle"});
    ScopedFlag busy(m_state->isBusy);
    m_state->isRunning = false;
    std::optional<Error> failure;
    for (auto iterator = m_state->records.rbegin(); iterator != m_state->records.rend(); ++iterator)
    {
        auto released = m_state->release(*iterator, a_world);
        if (!released.has_value() && !failure)
            failure = *released.try_error();
    }
    m_state->records.clear();
    return failure ? Result<void>::failure(*failure) : Result<void>::success();
}
Result<void> ScriptSystem::set_property(World &a_world, EntityHandle a_entity, std::string_view a_instanceId,
                                        const ScriptPropertyValue &a_value)
{
    auto owner = m_state->module->check_owner();
    if (!owner.has_value())
        return owner;
    if (m_state->world != &a_world || !m_state->isRunning || m_state->isBusy)
        return Result<void>::failure({ErrorCategory::InvalidState, "ScriptSystem.property.lifecycle"});
    ScopedFlag busy(m_state->isBusy);
    auto component = a_world.get_component<ScriptComponent>(a_entity);
    if (!component.has_value())
        return Result<void>::failure(*component.try_error());
    auto scripts = (*component.try_value())->scripts();
    auto target = std::find_if(scripts.begin(), scripts.end(),
                               [&](const auto &a_script) { return a_script.instanceId == a_instanceId; });
    if (target == scripts.end())
        return Result<void>::failure({ErrorCategory::InvalidArgument, "ScriptSystem.property.instance_missing"});
    auto property = std::find_if(target->properties.begin(), target->properties.end(),
                                 [&](const auto &a_property) { return a_property.propertyId == a_value.propertyId; });
    if (property == target->properties.end())
        target->properties.push_back(a_value);
    else
        *property = a_value;
    auto resolved = m_state->resolve(*target);
    if (!resolved.has_value())
        return Result<void>::failure(*resolved.try_error());
    // 受理済み設定だけを公開し、次の reconcile が同じ Setter を重複実行しないよう更新する
    const auto record =
        std::find_if(m_state->records.begin(), m_state->records.end(), [&](const auto &a_record)
                     { return a_record.entity == a_entity && a_record.descriptor.instanceId == a_instanceId; });
    if (record == m_state->records.end())
        return Result<void>::failure({ErrorCategory::InvalidState, "ScriptSystem.property.not_instantiated"});
    const auto &api = m_state->module->api();
    auto changed =
        checked(api.set_property(api.context, record->token, text(a_value.propertyId), property_value(a_value)));
    if (!changed.has_value())
        return changed;
    auto stored = (*component.try_value())->set_scripts(std::move(scripts));
    if (!stored.has_value())
        return stored;
    record->descriptor = resolved.take_value();
    return Result<void>::success();
}

Result<void> attach_scene_scripts(const Scene &a_scene, World &a_world, SceneInstanceHandle a_instance)
{
    for (const auto &object : a_scene.descriptor().objects)
    {
        if (object.scripts.empty())
            continue;
        auto entity = a_world.find_by_id(a_instance, object.objectId);
        if (!entity.has_value())
            return Result<void>::failure(*entity.try_error());
        auto component = a_world.add_component<ScriptComponent>(*entity.try_value());
        if (!component.has_value())
            return Result<void>::failure(*component.try_error());
        auto set = (*component.try_value())->set_scripts(object.scripts);
        if (!set.has_value())
            return set;
    }
    return Result<void>::success();
}
} // namespace cue
