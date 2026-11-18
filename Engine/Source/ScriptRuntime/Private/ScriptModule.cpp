#include <ScriptRuntime/ScriptModule.h>

#include <algorithm>
#include <cstring>
#include <utility>

#include <Foundation/Utf8.h>

namespace cue
{
namespace
{
/// @brief C ABI の固定長診断を所有 Error へ変換する
Error status_error(const CueScriptStatus &a_status)
{
    const auto end = std::find(std::begin(a_status.message), std::end(a_status.message), '\0');
    return {a_status.code == 1   ? ErrorCategory::InvalidArgument
            : a_status.code == 2 ? ErrorCategory::PlatformFailure
                                 : ErrorCategory::Fatal,
            "GameModule:" + std::string(a_status.message, end), a_status.code};
}
/// @brief 借用 Metadata の上限を確認して DLL から独立した文字列を作る
Result<std::string> copy_text(CueScriptString a_value)
{
    if (!a_value.data || a_value.size == 0 || a_value.size > 256)
        return Result<std::string>::failure({ErrorCategory::InvalidArgument, "GameModule.metadata_text"});
    std::string value(a_value.data, a_value.size);
    if (!is_valid_utf8_text(value))
        return Result<std::string>::failure({ErrorCategory::InvalidArgument, "GameModule.metadata_utf8"});
    return Result<std::string>::success(std::move(value));
}
} // namespace

struct RuntimeGameModule::State final
{
    CueScriptModuleApi api{};
    std::vector<ScriptTypeDescriptor> types;
    std::thread::id owner = std::this_thread::get_id();
    std::unique_ptr<IGameModuleLibrary> library;
};

RuntimeGameModule::RuntimeGameModule(CreateToken, std::unique_ptr<State> a_state) noexcept : m_state(std::move(a_state))
{
}
RuntimeGameModule::~RuntimeGameModule() = default;

Result<std::unique_ptr<RuntimeGameModule>> RuntimeGameModule::from_static(CueScriptGetModuleFunction a_entry)
{
    // Static Link は同じ構成・CRT の一つの実行 Binary に閉じ、交差構成を許可しない
    return from_entry(a_entry, static_cast<ScriptConfiguration>(CUE_SCRIPT_CONFIGURATION));
}

Result<std::unique_ptr<RuntimeGameModule>> RuntimeGameModule::from_entry(CueScriptGetModuleFunction a_entry,
                                                                         ScriptConfiguration a_configuration)
{
    using ModuleResult = Result<std::unique_ptr<RuntimeGameModule>>;
    try
    {
        if (!a_entry)
            return ModuleResult::failure({ErrorCategory::InvalidArgument, "GameModule.entry_missing"});
        auto state = std::make_unique<State>();
        std::uint32_t version = CUE_SCRIPT_ABI_VERSION;
        auto status = a_entry(version, &state->api);
        // Preview 7 の Module は明示的な Version 拒否を返す。初期化失敗はそのまま報告する
        constexpr char k_versionError[] = "ScriptModule.abi_version";
        if (status.code == 1 && std::strncmp(status.message, k_versionError, sizeof(k_versionError)) == 0)
        {
            version = CUE_SCRIPT_ABI_VERSION_2;
            state->api = {};
            status = a_entry(version, &state->api);
        }
        if (status.code != 0)
            return ModuleResult::failure(status_error(status));
        const auto &api = state->api;
        if (api.abiVersion != version || api.structSize != sizeof(CueScriptModuleApi) ||
            api.pointerSize != sizeof(void *) || api.configuration != static_cast<std::uint32_t>(a_configuration) ||
            !api.context || !api.type_count || !api.type_info || !api.property_info || !api.create ||
            !api.set_property || !api.start || !api.update || !api.stop || !api.destroy || !api.set_active)
            return ModuleResult::failure({ErrorCategory::InvalidArgument, "GameModule.abi_mismatch"});
        const auto count = api.type_count(api.context);
        if (count > k_maxScriptTypes)
            return ModuleResult::failure({ErrorCategory::InvalidArgument, "GameModule.type_limit"});
        state->types.reserve(count);
        for (std::uint32_t index = 0; index < count; ++index)
        {
            CueScriptTypeInfo info{};
            const auto queried = api.type_info(api.context, index, &info);
            if (queried.code != 0)
                return ModuleResult::failure(status_error(queried));
            auto id = copy_text(info.typeId);
            auto name = copy_text(info.displayName);
            if (!id.has_value() || !name.has_value() || info.propertyCount > k_maxScriptProperties)
                return ModuleResult::failure({ErrorCategory::InvalidArgument, "GameModule.type_metadata"});
            ScriptTypeDescriptor type{id.take_value(), name.take_value(), info.schemaVersion, {}};
            for (std::uint32_t propertyIndex = 0; propertyIndex < info.propertyCount; ++propertyIndex)
            {
                CueScriptPropertyInfo property{};
                const auto got = api.property_info(api.context, index, propertyIndex, &property);
                if (got.code != 0)
                    return ModuleResult::failure(status_error(got));
                auto propertyId = copy_text(property.propertyId);
                auto propertyName = copy_text(property.displayName);
                if (!propertyId.has_value() || !propertyName.has_value() || property.defaultValue.boolean > 1)
                    return ModuleResult::failure({ErrorCategory::InvalidArgument, "GameModule.property_metadata"});
                type.properties.push_back({propertyId.take_value(), propertyName.take_value(),
                                           static_cast<ScriptPropertyKind>(property.defaultValue.kind),
                                           property.defaultValue.number, property.defaultValue.boolean != 0});
            }
            auto valid = validate_script_type(type);
            if (!valid.has_value())
                return ModuleResult::failure(*valid.try_error());
            for (const auto &previous : state->types)
                if (previous.typeId == type.typeId)
                    return ModuleResult::failure({ErrorCategory::InvalidArgument, "GameModule.type_duplicate"});
            state->types.push_back(std::move(type));
        }
        return ModuleResult::success(std::make_unique<RuntimeGameModule>(CreateToken{}, std::move(state)));
    }
    catch (...)
    {
        return ModuleResult::failure({ErrorCategory::Fatal, "GameModule.registration_exception"});
    }
}

Result<std::unique_ptr<RuntimeGameModule>> RuntimeGameModule::from_dynamic(
    CueScriptGetModuleFunction a_entry, std::unique_ptr<IGameModuleLibrary> a_library,
    std::optional<ScriptConfiguration> a_configuration)
{
    using ModuleResult = Result<std::unique_ptr<RuntimeGameModule>>;
    if (a_configuration && !is_valid_script_configuration(*a_configuration))
        return ModuleResult::failure({ErrorCategory::InvalidArgument, "GameModule.configuration"});
    if (!a_library)
        return ModuleResult::failure({ErrorCategory::InvalidArgument, "GameModule.library_missing"});

    // ABI が参照する Metadata と Table が検証を終えるまでライブラリを生存させる
    const auto expected = a_configuration.value_or(static_cast<ScriptConfiguration>(CUE_SCRIPT_CONFIGURATION));
    auto module = from_entry(a_entry, expected);
    if (module.has_value())
        (*module.try_value())->m_state->library = std::move(a_library);
    return module;
}

const std::vector<ScriptTypeDescriptor> &RuntimeGameModule::types() const noexcept
{
    return m_state->types;
}
Result<void> RuntimeGameModule::check_owner() const
{
    if (m_state->owner != std::this_thread::get_id())
        return Result<void>::failure({ErrorCategory::WrongThread, "GameModule.owner_thread"});
    return Result<void>::success();
}
const CueScriptModuleApi &RuntimeGameModule::api() const noexcept
{
    return m_state->api;
}
void RuntimeGameModule::retain_until_process_exit() noexcept
{
    if (m_state->library)
        m_state->library->retain_until_process_exit();
}
} // namespace cue
