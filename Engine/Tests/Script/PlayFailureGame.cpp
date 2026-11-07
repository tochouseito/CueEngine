#include <memory>
#include <utility>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <Script/Module.h>

namespace
{
/// @brief 親 Editor の生存と停止期限を検証するため、子 Process だけを失敗させる
class FailureBehaviour final : public cue::Behaviour
{
  public:
    /// @brief Test のみが指定する障害の種類を受け取る
    [[nodiscard]] cue::Result<void> set_property(std::string_view a_id,
                                                 const cue::ScriptPropertyValue &a_value) override
    {
        if (a_id != "mode" || a_value.kind != cue::ScriptPropertyKind::Number)
            return cue::Result<void>::failure({cue::ErrorCategory::InvalidArgument, "Failure.property"});
        m_mode = a_value.number;
        return cue::Result<void>::success();
    }
    /// @brief Native Crash 相当の終了と無応答を子 Process に限定して再現する
    [[nodiscard]] cue::Result<void> start(cue::GameObject &) override
    {
        if (m_mode == 1)
            TerminateProcess(GetCurrentProcess(), 0xc0000005);
        if (m_mode == 2)
            Sleep(INFINITE);
        return cue::Result<void>::success();
    }
    /// @brief 通常の Result 失敗も子の異常停止として伝播する
    [[nodiscard]] cue::Result<void> update(cue::GameObject &, const cue::ScriptUpdateContext &) override
    {
        if (m_mode == 3)
            return cue::Result<void>::failure({cue::ErrorCategory::Fatal, "Failure.update"});
        return cue::Result<void>::success();
    }

  private:
    double m_mode = 0;
};

/// @brief 障害注入用の Metadata と Factory を同じ Module に登録する
cue::Result<std::unique_ptr<cue::ScriptModule>> create_module()
{
    cue::ScriptTypeDescriptor type{
        "test.Failure", "Failure Fixture", 1, {{"mode", "Mode", cue::ScriptPropertyKind::Number, 0, false}}};
    std::vector<cue::ScriptRegistration> registrations;
    registrations.push_back(cue::make_script_registration<FailureBehaviour>(std::move(type)));
    return cue::ScriptModule::create(std::move(registrations));
}
} // namespace

/// @brief 障害 Fixture の登録例外を ABI の内側で Status へ変換する
extern "C" CUE_SCRIPT_EXPORT CueScriptStatus CUE_SCRIPT_CALL CueScriptGetModule(std::uint32_t a_version,
                                                                                CueScriptModuleApi *a_api) noexcept
{
    try
    {
        if (!a_api)
            return cue::script_status({cue::ErrorCategory::InvalidArgument, "Failure.api_null"});
        static auto module = create_module();
        if (!module.has_value())
            return cue::script_status(*module.try_error());
        return (*module.try_value())->get_api(a_version, *a_api);
    }
    catch (...)
    {
        return cue::script_exception_status();
    }
}
