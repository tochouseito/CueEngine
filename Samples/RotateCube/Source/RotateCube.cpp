#include <cmath>
#include <memory>
#include <utility>

#include <Math/Quaternion.h>
#include <Script/Module.h>

namespace
{
/// @brief ECS の Transform を取得して回転する、Game Module 側の Behaviour
class RotateCubeBehaviour final : public cue::Behaviour
{
  public:
    /// @brief 公開 Schema の設定を受け取る。変更途中の実行 Instance を黙って継続しない
    [[nodiscard]] cue::Result<void> set_property(std::string_view a_id,
                                                 const cue::ScriptPropertyValue &a_value) override
    {
        if (a_id == "speed" && a_value.kind == cue::ScriptPropertyKind::Number && std::isfinite(a_value.number) &&
            std::abs(a_value.number) <= 36000.0)
        {
            m_speedDegrees = static_cast<float>(a_value.number);
            return cue::Result<void>::success();
        }
        if (a_id == "isRotating" && a_value.kind == cue::ScriptPropertyKind::Boolean)
        {
            m_isRotating = a_value.boolean;
            return cue::Result<void>::success();
        }
        return cue::Result<void>::failure({cue::ErrorCategory::InvalidArgument, "RotateCube.property"});
    }
    /// @brief 時間差に比例した回転を Math で計算し、Component Pointer を保持せず書き戻す
    [[nodiscard]] cue::Result<void> update(cue::GameObject &a_object,
                                           const cue::ScriptUpdateContext &a_context) override
    {
        if (!m_isRotating)
            return cue::Result<void>::success();
        auto transform = a_object.transform();
        if (!transform.has_value())
            return cue::Result<void>::failure(*transform.try_error());
        const auto seconds = std::chrono::duration<float>(a_context.deltaTime).count();
        auto delta = cue::math::rotation_axis_angle(cue::math::float3{0, 1, 0},
                                                    cue::math::degrees_to_radians(m_speedDegrees) * seconds);
        if (!delta.has_value())
            return cue::Result<void>::failure(*delta.try_error());
        auto rotation = cue::math::normalize(*delta.try_value() * transform.try_value()->rotation);
        if (!rotation.has_value())
            return cue::Result<void>::failure(*rotation.try_error());
        transform.try_value()->rotation = *rotation.try_value();
        return a_object.set_transform(*transform.try_value());
    }

  private:
    float m_speedDegrees = 90;
    bool m_isRotating = true;
};

/// @brief Scene へ保存する安定 ID と Inspector の表示項目を明示登録する
cue::Result<std::unique_ptr<cue::ScriptModule>> create_module()
{
    cue::ScriptTypeDescriptor type{"sample.RotateCube",
                                   "Rotate Cube",
                                   1,
                                   {{"speed", "Speed (degrees/sec)", cue::ScriptPropertyKind::Number, 90, false},
                                    {"isRotating", "Rotate", cue::ScriptPropertyKind::Boolean, 0, true}}};
    std::vector<cue::ScriptRegistration> registrations;
    registrations.push_back(cue::make_script_registration<RotateCubeBehaviour>(std::move(type)));
    return cue::ScriptModule::create(std::move(registrations));
}
} // namespace

/// @brief DLL / Static Link に共通の C 入口。登録時の例外も Game Module の内側で Status へ変換する
extern "C" CUE_SCRIPT_EXPORT CueScriptStatus CUE_SCRIPT_CALL CueScriptGetModule(std::uint32_t a_version,
                                                                                CueScriptModuleApi *a_api) noexcept
{
    try
    {
        if (!a_api)
            return cue::script_status({cue::ErrorCategory::InvalidArgument, "RotateCube.api_null"});
        // Module 自身の登録だけを保持する。Current World / Project は Global に置かない
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
