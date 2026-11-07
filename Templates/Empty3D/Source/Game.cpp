#include <memory>
#include <utility>
#include <vector>

#include <Script/Module.h>

namespace
{
/// @brief 空の Scene からゲーム処理を追加できる最小の登録型
class EmptyBehaviour final : public cue::Behaviour
{
  public:
    /// @brief 登録例では World を変更せず、時間入力を利用する処理は派生実装で追加する
    [[nodiscard]] cue::Result<void> update(cue::GameObject &, const cue::ScriptUpdateContext &) override
    {
        return cue::Result<void>::success();
    }
};

/// @brief 型情報を Module の中で所有し、初期 Scene へは Script を割り当てない
cue::Result<std::unique_ptr<cue::ScriptModule>> create_module()
{
    std::vector<cue::ScriptRegistration> types;
    types.push_back(cue::make_script_registration<EmptyBehaviour>({"game.Empty", "Empty Behaviour", 1, {}}));
    return cue::ScriptModule::create(std::move(types));
}
} // namespace

/// @brief Game DLL と製品 Static Link が共用する C ABI 入口
extern "C" CUE_SCRIPT_EXPORT CueScriptStatus CUE_SCRIPT_CALL CueScriptGetModule(std::uint32_t a_version,
                                                                                CueScriptModuleApi *a_api) noexcept
{
    try
    {
        if (!a_api)
            return cue::script_status({cue::ErrorCategory::InvalidArgument, "Game.api_null"});
        static auto module = create_module();
        return module.has_value() ? (*module.try_value())->get_api(a_version, *a_api)
                                  : cue::script_status(*module.try_error());
    }
    catch (...)
    {
        return cue::script_exception_status();
    }
}
