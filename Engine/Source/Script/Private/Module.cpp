#include <Script/Module.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace cue
{
namespace
{
/// @brief 外部入力に使う固定長 Error を確保なしで作る
CueScriptStatus invalid_status(const char *a_message) noexcept
{
    CueScriptStatus status{};
    status.code = 1;
    for (std::size_t index = 0; a_message[index] && index + 1 < sizeof(status.message); ++index)
        status.message[index] = a_message[index];
    return status;
}
/// @brief Module 所有文字列を次の Unload まで借用する
CueScriptString borrow(const std::string &a_text) noexcept
{
    return {a_text.data(), static_cast<std::uint32_t>(a_text.size())};
}
/// @brief Callback 中だけ使う Engine の公開 Table を検証する
bool valid_engine(const CueScriptEngineApi *a_api) noexcept
{
    return a_api && a_api->abiVersion == CUE_SCRIPT_ABI_VERSION && a_api->structSize == sizeof(CueScriptEngineApi) &&
           a_api->is_alive && a_api->get_transform && a_api->set_transform && a_api->find_by_name && a_api->report &&
           a_api->get_camera && a_api->get_mesh_renderer && a_api->find_script && a_api->get_scene_instance;
}
/// @brief ABI Property を検証して意味のある成分だけを採用する
bool valid_property(CueScriptPropertyValue a_value) noexcept
{
    return (a_value.kind == static_cast<std::uint32_t>(ScriptPropertyKind::Number) && std::isfinite(a_value.number)) ||
           (a_value.kind == static_cast<std::uint32_t>(ScriptPropertyKind::Boolean) && a_value.boolean <= 1);
}
/// @brief SDK が保持する Property を C ABI の固定幅値へ変換する
CueScriptPropertyValue default_value(const ScriptPropertyDescriptor &a_property) noexcept
{
    return {static_cast<std::uint32_t>(a_property.kind), a_property.defaultNumber, a_property.defaultBoolean ? 1u : 0u};
}
} // namespace

struct ScriptModule::State final
{
    struct Instance final
    {
        std::uint32_t typeIndex = 0;
        std::unique_ptr<Behaviour> behaviour;
        bool hasStarted = false;
        bool hasStopped = false;
        bool hasFailed = false;
    };

    std::vector<ScriptRegistration> registrations;
    std::unordered_map<CueScriptInstance, Instance> instances;
    CueScriptInstance nextInstance = 1;
    std::size_t callDepth = 0;
    GameObject::ScriptAccess access{this, &callDepth, find_script};

    /// @brief C ABI の Token だけを受け取り、型と実体を同じ Game Module 内で照合する
    static Result<Behaviour *> find_script(void *a_context, const GameObject &a_object,
                                           const std::type_info &a_type, std::string_view a_instanceId)
    {
        auto &state = *static_cast<State *>(a_context);
        const ScriptRegistration *registration = nullptr;
        for (const auto &candidate : state.registrations)
        {
            if (candidate.type && *candidate.type == a_type)
            {
                if (registration)
                {
                    return Result<Behaviour *>::failure({ErrorCategory::InvalidArgument, "Script.lookup.type_ambiguous"});
                }
                registration = &candidate;
            }
        }
        if (!registration || a_instanceId.size() > 36)
        {
            return Result<Behaviour *>::failure({ErrorCategory::InvalidArgument, "Script.lookup.type_or_instance"});
        }
        const auto &api = a_object.m_api;
        CueScriptInstance token = 0;
        const CueScriptString instance{a_instanceId.data(), static_cast<std::uint32_t>(a_instanceId.size())};
        auto result = script_result(api.find_script(api.context, a_object.handle(),
                                                    borrow(registration->descriptor.typeId), instance, &token));
        if (!result.has_value())
        {
            return Result<Behaviour *>::failure(*result.try_error());
        }
        const auto found = state.instances.find(token);
        if (found == state.instances.end() || !found->second.hasStarted || found->second.hasStopped ||
            found->second.hasFailed || &state.registrations[found->second.typeIndex] != registration)
        {
            return Result<Behaviour *>::failure({ErrorCategory::InvalidState, "Script.lookup.instance"});
        }
        return Result<Behaviour *>::success(found->second.behaviour.get());
    }

    /// @brief 停止または失敗した Script への通知を直ちに失効させる
    static void clear_subscriptions(Behaviour &a_behaviour) noexcept
    {
        a_behaviour.m_acceptsSubscriptions = false;
        a_behaviour.m_isActive = false;
        a_behaviour.m_eventSubscriptions.clear();
    }

    /// @brief ゲーム Callback の失敗を終端として記録し、停止以外の再実行を抑止する
    template <typename Callback> static CueScriptStatus invoke(Instance &a_instance, Callback &&a_callback) noexcept
    {
        try
        {
            auto result = a_callback();
            if (result.has_value())
                return {};
            a_instance.hasFailed = true;
            clear_subscriptions(*a_instance.behaviour);
            return script_status(*result.try_error());
        }
        catch (...)
        {
            a_instance.hasFailed = true;
            clear_subscriptions(*a_instance.behaviour);
            return script_exception_status();
        }
    }

    /// @brief 登録数を返す。登録は公開後に変化しない
    static std::uint32_t CUE_SCRIPT_CALL type_count(void *a_context) noexcept
    {
        return a_context ? static_cast<std::uint32_t>(static_cast<State *>(a_context)->registrations.size()) : 0;
    }
    /// @brief 文字列は Module 所有値を借用し、Engine は Unload 前に複写する
    static CueScriptStatus CUE_SCRIPT_CALL type_info(void *a_context, std::uint32_t a_index,
                                                     CueScriptTypeInfo *a_info) noexcept
    {
        if (!a_context || !a_info)
            return invalid_status("Script.type_info.arguments");
        auto &state = *static_cast<State *>(a_context);
        if (a_index >= state.registrations.size())
            return invalid_status("Script.type_info.index");
        const auto &type = state.registrations[a_index].descriptor;
        *a_info = {borrow(type.typeId), borrow(type.displayName), type.schemaVersion,
                   static_cast<std::uint32_t>(type.properties.size())};
        return {};
    }
    /// @brief 登録済みの Property 情報だけを公開する
    static CueScriptStatus CUE_SCRIPT_CALL property_info(void *a_context, std::uint32_t a_type, std::uint32_t a_index,
                                                         CueScriptPropertyInfo *a_info) noexcept
    {
        if (!a_context || !a_info)
            return invalid_status("Script.property_info.arguments");
        auto &state = *static_cast<State *>(a_context);
        if (a_type >= state.registrations.size() || a_index >= state.registrations[a_type].descriptor.properties.size())
            return invalid_status("Script.property_info.index");
        const auto &property = state.registrations[a_type].descriptor.properties[a_index];
        *a_info = {borrow(property.propertyId), borrow(property.displayName), default_value(property)};
        return {};
    }
    /// @brief ID と型を検証した後だけ SDK の Setter を実行する
    static CueScriptStatus assign(State &a_state, Instance &a_instance, CueScriptString a_id,
                                  CueScriptPropertyValue a_value)
    {
        if (!a_id.data || !a_id.size || a_id.size > 128 || !valid_property(a_value) || a_instance.hasStopped ||
            a_instance.hasFailed)
            return invalid_status("Script.set_property.arguments");
        const std::string_view id{a_id.data, a_id.size};
        const auto &properties = a_state.registrations[a_instance.typeIndex].descriptor.properties;
        const auto found = std::find_if(properties.begin(), properties.end(),
                                        [&](const auto &a_property) { return a_property.propertyId == id; });
        if (found == properties.end() || static_cast<std::uint32_t>(found->kind) != a_value.kind)
            return invalid_status("Script.set_property.schema");
        ScriptPropertyValue value{};
        value.propertyId = id;
        value.kind = found->kind;
        value.number = a_value.number;
        value.boolean = a_value.boolean != 0;
        return invoke(a_instance, [&]() { return a_instance.behaviour->set_property(id, value); });
    }
    /// @brief Factory と既定値の適用が全て成功してから Token を公開する
    static CueScriptStatus CUE_SCRIPT_CALL create(void *a_context, std::uint32_t a_type,
                                                  CueScriptInstance *a_token) noexcept
    {
        try
        {
            if (!a_context || !a_token)
                return invalid_status("Script.create.arguments");
            auto &state = *static_cast<State *>(a_context);
            if (a_type >= state.registrations.size() ||
                state.nextInstance == (std::numeric_limits<CueScriptInstance>::max)())
                return invalid_status("Script.create.type_or_capacity");
            Instance instance{a_type, state.registrations[a_type].create()};
            if (!instance.behaviour)
                return invalid_status("Script.create.null_instance");
            for (const auto &property : state.registrations[a_type].descriptor.properties)
            {
                auto assigned = assign(state, instance, borrow(property.propertyId), default_value(property));
                if (assigned.code)
                    return assigned;
            }
            const auto token = state.nextInstance++;
            state.instances.emplace(token, std::move(instance));
            *a_token = token;
            return {};
        }
        catch (...)
        {
            return script_exception_status();
        }
    }
    /// @brief 未生成・破棄済み Token を拒否し、Module の外へ例外を漏らさない
    static CueScriptStatus CUE_SCRIPT_CALL set_property(void *a_context, CueScriptInstance a_token,
                                                        CueScriptString a_id, CueScriptPropertyValue a_value) noexcept
    {
        try
        {
            if (!a_context)
                return invalid_status("Script.set_property.context");
            auto &state = *static_cast<State *>(a_context);
            const auto found = state.instances.find(a_token);
            if (found == state.instances.end())
                return invalid_status("Script.set_property.instance");
            return assign(state, found->second, a_id, a_value);
        }
        catch (...)
        {
            return script_exception_status();
        }
    }
    /// @brief 開始前に状態を記録し、例外や Result 失敗でも stop を許可する
    static CueScriptStatus CUE_SCRIPT_CALL start(void *a_context, CueScriptInstance a_token,
                                                 const CueScriptEngineApi *a_api, CueScriptEntity a_entity) noexcept
    {
        try
        {
            if (!a_context || !valid_engine(a_api))
                return invalid_status("Script.start.arguments");
            auto &state = *static_cast<State *>(a_context);
            auto found = state.instances.find(a_token);
            if (found == state.instances.end() || found->second.hasStarted || found->second.hasStopped ||
                found->second.hasFailed)
                return invalid_status("Script.start.lifecycle");
            found->second.hasStarted = true;
            found->second.behaviour->m_acceptsSubscriptions = true;
            GameObject object(*a_api, a_entity, &state.access);
            return invoke(found->second, [&]() { return found->second.behaviour->start(object); });
        }
        catch (...)
        {
            return script_exception_status();
        }
    }
    /// @brief 開始済みの Instance だけを直列更新する
    static CueScriptStatus CUE_SCRIPT_CALL update(void *a_context, CueScriptInstance a_token,
                                                  const CueScriptEngineApi *a_api, CueScriptEntity a_entity,
                                                  CueScriptUpdateContext a_update) noexcept
    {
        try
        {
            if (!a_context || !valid_engine(a_api) || a_update.deltaNanoseconds < 0)
                return invalid_status("Script.update.arguments");
            auto &state = *static_cast<State *>(a_context);
            auto found = state.instances.find(a_token);
            if (found == state.instances.end() || !found->second.hasStarted || found->second.hasStopped ||
                found->second.hasFailed)
                return invalid_status("Script.update.lifecycle");
            GameObject object(*a_api, a_entity, &state.access);
            const ScriptUpdateContext context{std::chrono::nanoseconds(a_update.deltaNanoseconds), a_update.tick};
            return invoke(found->second, [&]() { return found->second.behaviour->update(object, context); });
        }
        catch (...)
        {
            return script_exception_status();
        }
    }
    /// @brief 開始が失敗した場合も一度だけ停止する。後始末の失敗で再呼出しをしない
    static CueScriptStatus CUE_SCRIPT_CALL stop(void *a_context, CueScriptInstance a_token,
                                                const CueScriptEngineApi *a_api, CueScriptEntity a_entity) noexcept
    {
        try
        {
            if (!a_context || !valid_engine(a_api))
                return invalid_status("Script.stop.arguments");
            auto &state = *static_cast<State *>(a_context);
            auto found = state.instances.find(a_token);
            if (found == state.instances.end())
                return invalid_status("Script.stop.instance");
            if (!found->second.hasStarted || found->second.hasStopped)
                return {};
            found->second.hasStopped = true;
            clear_subscriptions(*found->second.behaviour);
            GameObject object(*a_api, a_entity, &state.access);
            return invoke(found->second, [&]() { return found->second.behaviour->stop(object); });
        }
        catch (...)
        {
            return script_exception_status();
        }
    }
    /// @brief Instance は生成した Module の Allocator で破棄し、Token は再利用しない
    static CueScriptStatus CUE_SCRIPT_CALL destroy(void *a_context, CueScriptInstance a_token) noexcept
    {
        try
        {
            if (!a_context)
                return invalid_status("Script.destroy.context");
            auto &state = *static_cast<State *>(a_context);
            auto found = state.instances.find(a_token);
            if (found == state.instances.end())
                return invalid_status("Script.destroy.instance");
            if (found->second.hasStarted && !found->second.hasStopped)
                return invalid_status("Script.destroy.running");
            state.instances.erase(found);
            return {};
        }
        catch (...)
        {
            return script_exception_status();
        }
    }

    /// @brief 無効化中は購読を保持しつつ通知を止める。再有効化後に同じ購読を使う
    static CueScriptStatus CUE_SCRIPT_CALL set_active(void *a_context, CueScriptInstance a_token,
                                                       std::uint32_t a_active) noexcept
    {
        if (!a_context || a_active > 1)
        {
            return invalid_status("Script.set_active.arguments");
        }
        auto &state = *static_cast<State *>(a_context);
        const auto found = state.instances.find(a_token);
        if (found == state.instances.end() || found->second.hasStopped || found->second.hasFailed)
        {
            return invalid_status("Script.set_active.instance");
        }
        found->second.behaviour->m_isActive = a_active != 0;
        return {};
    }
};

ScriptModule::ScriptModule(CreateToken, std::unique_ptr<State> a_state) noexcept : m_state(std::move(a_state))
{
}

ScriptModule::~ScriptModule() = default;

Result<std::unique_ptr<ScriptModule>> ScriptModule::create(std::vector<ScriptRegistration> a_registrations)
{
    try
    {
        if (a_registrations.empty() || a_registrations.size() > k_maxScriptTypes)
            return Result<std::unique_ptr<ScriptModule>>::failure(
                {ErrorCategory::InvalidArgument, "ScriptModule.type_count"});
        std::unordered_set<std::string_view> typeIds;
        for (const auto &registration : a_registrations)
        {
            const auto &type = registration.descriptor;
            auto valid = validate_script_type(type);
            if (!valid.has_value())
                return Result<std::unique_ptr<ScriptModule>>::failure(*valid.try_error());
            if (!registration.create || !typeIds.insert(type.typeId).second)
                return Result<std::unique_ptr<ScriptModule>>::failure(
                    {ErrorCategory::InvalidArgument, "ScriptModule.type"});
        }
        auto state = std::make_unique<State>();
        state->registrations = std::move(a_registrations);
        return Result<std::unique_ptr<ScriptModule>>::success(
            std::make_unique<ScriptModule>(CreateToken{}, std::move(state)));
    }
    catch (...)
    {
        return Result<std::unique_ptr<ScriptModule>>::failure({ErrorCategory::Fatal, "ScriptModule.create.exception"});
    }
}

CueScriptStatus ScriptModule::get_api(std::uint32_t a_version, CueScriptModuleApi &a_api) noexcept
{
    if (a_version != CUE_SCRIPT_ABI_VERSION)
        return invalid_status("ScriptModule.abi_version");
    a_api = {CUE_SCRIPT_ABI_VERSION,
             sizeof(CueScriptModuleApi),
             sizeof(void *),
             CUE_SCRIPT_CONFIGURATION,
             m_state.get(),
             State::type_count,
             State::type_info,
             State::property_info,
             State::create,
             State::set_property,
             State::start,
             State::update,
             State::stop,
             State::destroy,
             State::set_active};
    return {};
}
} // namespace cue
