#include <limits>
#include <stdexcept>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <Math/Quaternion.h>
#include <Script/ModuleEntry.h>

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
    void start() override
    {
        if (m_mode == 1)
            TerminateProcess(GetCurrentProcess(), 0xc0000005);
        if (m_mode == 2)
            Sleep(INFINITE);
    }
    /// @brief void の入口の例外も子の異常停止として伝播する
    void update() override
    {
        ++m_ticks;
        if (m_mode == 3)
            throw cue::Error{cue::ErrorCategory::Fatal, "Failure.update"};
        if (m_mode == 8)
            throw std::runtime_error("Failure.fixture_exception");
        // Ready と最初の View の後に障害を起こし、実行中の分離・回復を確認する
        if (m_ticks > 20 && m_mode == 4)
            RaiseException(EXCEPTION_ACCESS_VIOLATION, EXCEPTION_NONCONTINUABLE, 0, nullptr);
        if (m_ticks > 20 && m_mode == 5)
            Sleep(INFINITE);
        if (m_mode == 6 || (m_ticks > 20 && m_mode == 7))
        {
            auto pose = transform.get();
            if (!pose.has_value())
                throw *pose.try_error();
            if (m_mode == 6)
            {
                auto delta = cue::math::rotation_axis_angle(cue::math::float3{0, 1, 0}, delta_time_seconds());
                if (!delta.has_value())
                    throw *delta.try_error();
                auto rotation = cue::math::normalize(*delta.try_value() * pose.try_value()->rotation);
                if (!rotation.has_value())
                    throw *rotation.try_error();
                pose.try_value()->rotation = rotation.take_value();
            }
            else
            {
                const auto huge = (std::numeric_limits<float>::max)() * 0.9f;
                pose.try_value()->scale = {huge, huge, huge};
            }
            auto written = transform.set(*pose.try_value());
            if (!written.has_value())
                throw *written.try_error();
        }
    }

  private:
    double m_mode = 0;
    std::uint64_t m_ticks = 0;
};

} // namespace

// ゲーム Module 全体の登録と接続入口を一つだけ定義する
CUE_DEFINE_SCRIPT_MODULE(cue::make_script_registration<FailureBehaviour>(
    {"test.Failure", "Failure Fixture", 1, {{"mode", "Mode", cue::ScriptPropertyKind::Number, 0, false}}}))
