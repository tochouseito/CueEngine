#include <cmath>

#include <Math/Quaternion.h>
#include <Script/ModuleEntry.h>

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
    [[nodiscard]] cue::Result<void> update() override
    {
        if (!m_isRotating)
            return cue::Result<void>::success();
        auto current = transform.get();
        if (!current.has_value())
            return cue::Result<void>::failure(*current.try_error());
        const float seconds = delta_time_seconds();
        auto delta = cue::math::rotation_axis_angle(cue::math::float3{0, 1, 0},
                                                    cue::math::degrees_to_radians(m_speedDegrees) * seconds);
        if (!delta.has_value())
            return cue::Result<void>::failure(*delta.try_error());
        auto rotation = cue::math::normalize(*delta.try_value() * current.try_value()->rotation);
        if (!rotation.has_value())
            return cue::Result<void>::failure(*rotation.try_error());
        current.try_value()->rotation = *rotation.try_value();
        return transform.set(*current.try_value());
    }

  private:
    float m_speedDegrees = 90;
    bool m_isRotating = true;
};

} // namespace

// 共通 Module 入口から呼ぶ、この Script の型登録を定義する
CUE_DEFINE_SCRIPT(RotateCubeBehaviour,
                  cue::make_script_registration<RotateCubeBehaviour>(
                      {"sample.RotateCube",
                       "Rotate Cube",
                       1,
                       {{"speed", "Speed (degrees/sec)", cue::ScriptPropertyKind::Number, 90, false},
                        {"isRotating", "Rotate", cue::ScriptPropertyKind::Boolean, 0, true}}}))
