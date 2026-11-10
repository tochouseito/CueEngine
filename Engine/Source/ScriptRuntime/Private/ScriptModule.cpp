#include <ScriptRuntime/ScriptModule.h>

#include <algorithm>
#include <cstring>
#include <utility>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Foundation/Windows/UtfConversion.h>
#include <windows.h>
#endif

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
    bool canUnload = true;
#if defined(_WIN32)
    HMODULE library = nullptr;
#endif
    /// @brief Callback の保持先を解放する。Table を保持した Instance は先に破棄する
    ~State()
    {
#if defined(_WIN32)
        if (library && canUnload)
            FreeLibrary(library);
#endif
    }
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
        const auto status = a_entry(CUE_SCRIPT_ABI_VERSION, &state->api);
        if (status.code != 0)
            return ModuleResult::failure(status_error(status));
        const auto &api = state->api;
        if (api.abiVersion != CUE_SCRIPT_ABI_VERSION || api.structSize != sizeof(CueScriptModuleApi) ||
            api.pointerSize != sizeof(void *) || api.configuration != static_cast<std::uint32_t>(a_configuration) ||
            !api.context || !api.type_count || !api.type_info || !api.property_info || !api.create ||
            !api.set_property || !api.start || !api.update || !api.stop || !api.destroy)
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

Result<std::unique_ptr<RuntimeGameModule>> RuntimeGameModule::load(const Path &a_file,
                                                                   std::optional<ScriptConfiguration> a_configuration)
{
    using ModuleResult = Result<std::unique_ptr<RuntimeGameModule>>;
    if (a_configuration && !is_valid_script_configuration(*a_configuration))
        return ModuleResult::failure({ErrorCategory::InvalidArgument, "GameModule.configuration"});
#if defined(_WIN32)
    try
    {
        if (!a_file.is_absolute())
            return ModuleResult::failure({ErrorCategory::InvalidArgument, "GameModule.absolute_path_required"});
        auto wide = utf8_to_utf16(a_file.utf8());
        if (!wide.has_value())
            return ModuleResult::failure(*wide.try_error());
        // 明示 DLL の親と標準 Directory だけを検索し、CWD から依存 DLL を読み込まない
        auto owner = std::make_unique<State>();
        owner->library = LoadLibraryExW(wide.try_value()->c_str(), nullptr,
                                        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!owner->library)
            return ModuleResult::failure({ErrorCategory::PlatformFailure, "GameModule.LoadLibraryExW", GetLastError()});
        const auto entry =
            reinterpret_cast<CueScriptGetModuleFunction>(GetProcAddress(owner->library, "CueScriptGetModule"));
        // C ABI の POD / 借用 / Module 内破棄を維持して、選択した Game 構成だけを受け入れる
        auto module =
            from_entry(entry, a_configuration.value_or(static_cast<ScriptConfiguration>(CUE_SCRIPT_CONFIGURATION)));
        if (!module.has_value())
            return module;
        (*module.try_value())->m_state->library = std::exchange(owner->library, nullptr);
        return module;
    }
    catch (...)
    {
        return ModuleResult::failure({ErrorCategory::Fatal, "GameModule.load_exception"});
    }
#else
    (void)a_file;
    return ModuleResult::failure({ErrorCategory::InvalidState, "GameModule.dynamic_loading_unsupported"});
#endif
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
    m_state->canUnload = false;
}
} // namespace cue
