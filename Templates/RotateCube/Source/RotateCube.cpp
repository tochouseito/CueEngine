#include <cmath>

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
    /// @brief 時間差に比例して回転し、暗黙操作の失敗は Module の Callback 境界へ伝える
    void update() override
    {
        if (!m_isRotating)
            return;
        transform.rotate.y += m_speedDegrees * delta_time_seconds();
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
