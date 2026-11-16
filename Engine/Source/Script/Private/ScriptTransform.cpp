#include <Script/ScriptTransform.h>

#include <utility>

#include <Math/Quaternion.h>
#include <Math/Scalar.h>
#include <Math/VectorOperations.h>
#include <Script/Behaviour.h>

namespace cue
{
namespace
{
constexpr math::float3 k_rotationAxes[]{{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}};

/// @brief 有効な Script Callback の外で World へのアクセスを拒否する
Error missing_context()
{
    return {ErrorCategory::InvalidState, "ScriptTransform.callback_context"};
}
} // namespace

ScriptTransform::Axis::Axis(const Vector &a_vector, std::uint32_t a_axis) noexcept
    : m_vector(a_vector), m_axis(a_axis)
{
}

Result<float> ScriptTransform::Axis::get() const
{
    auto vector = m_vector.get();
    if (!vector.has_value())
    {
        return Result<float>::failure(std::move(*vector.try_error()));
    }
    const auto &value = *vector.try_value();
    const float axes[]{value.x, value.y, value.z};
    return Result<float>::success(axes[m_axis]);
}

Result<void> ScriptTransform::Axis::change(std::uint32_t a_operation, float a_value) const
{
    return m_vector.m_transform.change({m_vector.m_field, m_axis, a_operation, {a_value, 0.0f, 0.0f}});
}

Result<void> ScriptTransform::Axis::set(float a_value) const
{
    return change(CUE_SCRIPT_TRANSFORM_SET, a_value);
}

Result<void> ScriptTransform::Axis::add(float a_value) const
{
    return change(CUE_SCRIPT_TRANSFORM_ADD, a_value);
}

Result<void> ScriptTransform::Axis::multiply(float a_factor) const
{
    return change(CUE_SCRIPT_TRANSFORM_MULTIPLY, a_factor);
}

void ScriptTransform::Axis::operator=(float a_value) const
{
    m_vector.m_transform.remember(set(a_value));
}

void ScriptTransform::Axis::operator+=(float a_value) const
{
    m_vector.m_transform.remember(add(a_value));
}

void ScriptTransform::Axis::operator-=(float a_value) const
{
    m_vector.m_transform.remember(add(-a_value));
}

void ScriptTransform::Axis::operator*=(float a_factor) const
{
    m_vector.m_transform.remember(multiply(a_factor));
}

ScriptTransform::Vector::Vector(const ScriptTransform &a_transform, std::uint32_t a_field) noexcept
    : x(*this, 0), y(*this, 1), z(*this, 2), m_transform(a_transform), m_field(a_field)
{
}

Result<math::float3> ScriptTransform::Vector::get() const
{
    auto transform = m_transform.get();
    if (!transform.has_value())
    {
        return Result<math::float3>::failure(std::move(*transform.try_error()));
    }
    const auto &value = *transform.try_value();
    return Result<math::float3>::success(m_field == CUE_SCRIPT_TRANSFORM_POSITION ? value.position : value.scale);
}

ScriptTransform::RotationAxis::RotationAxis(const ScriptTransform &a_transform, std::uint32_t a_axis) noexcept
    : m_transform(a_transform), m_axis(a_axis)
{
}

Result<void> ScriptTransform::RotationAxis::add(float a_degrees) const
{
    auto delta = math::rotation_axis_angle(k_rotationAxes[m_axis], math::degrees_to_radians(a_degrees));
    if (!delta.has_value())
    {
        return Result<void>::failure(std::move(*delta.try_error()));
    }
    auto current = m_transform.get();
    if (!current.has_value())
    {
        return Result<void>::failure(std::move(*current.try_error()));
    }

    // 既存の姿勢が非単位でも、積の中間値が元の Quaternion の大きさに左右されないようにする
    auto unit = math::normalize(current.try_value()->rotation);
    if (!unit.has_value())
    {
        return Result<void>::failure(std::move(*unit.try_error()));
    }
    // 固定軸まわりに適用するため、差分回転を左から掛ける
    auto rotation = math::normalize(*delta.try_value() * *unit.try_value());
    if (!rotation.has_value())
    {
        return Result<void>::failure(std::move(*rotation.try_error()));
    }
    current.try_value()->rotation = rotation.take_value();
    return m_transform.set(*current.try_value());
}

void ScriptTransform::RotationAxis::operator+=(float a_degrees) const
{
    m_transform.remember(add(a_degrees));
}

void ScriptTransform::RotationAxis::operator-=(float a_degrees) const
{
    m_transform.remember(add(-a_degrees));
}

ScriptTransform::Rotation::Rotation(const ScriptTransform &a_transform) noexcept
    : x(a_transform, 0), y(a_transform, 1), z(a_transform, 2)
{
}

ScriptTransform::ScriptTransform(Behaviour &a_owner) noexcept
    : position(*this, CUE_SCRIPT_TRANSFORM_POSITION), scale(*this, CUE_SCRIPT_TRANSFORM_SCALE), rotate(*this),
      m_owner(a_owner)
{
}

Result<math::floatTransform> ScriptTransform::get() const
{
    return m_owner.get_component<math::floatTransform>();
}

Result<void> ScriptTransform::set(const math::floatTransform &a_value) const
{
    const auto *callback = m_owner.m_callback;
    return callback ? callback->object.set_transform(a_value) : Result<void>::failure(missing_context());
}

Result<void> ScriptTransform::translate(float a_x, float a_y, float a_z) const
{
    return translate(math::float3{a_x, a_y, a_z});
}

Result<void> ScriptTransform::translate(math::float3 a_offset) const
{
    return change({CUE_SCRIPT_TRANSFORM_POSITION, 0, CUE_SCRIPT_TRANSFORM_TRANSLATE,
                   {a_offset.x, a_offset.y, a_offset.z}});
}

Result<void> ScriptTransform::change(const CueScriptTransformChange &a_change) const
{
    const auto *callback = m_owner.m_callback;
    return callback ? callback->object.change_transform(a_change) : Result<void>::failure(missing_context());
}

void ScriptTransform::remember(Result<void> a_result) const
{
    if (a_result.has_value())
    {
        return;
    }
    auto *callback = m_owner.m_callback;
    if (!callback)
    {
        // 初期 Property の設定など World を借用しない経路では Module 側で失敗へ変換する
        throw std::move(*a_result.try_error());
    }
    callback->remember(std::move(a_result));
}

float math::length(const ScriptTransform::Vector &a_value)
{
    auto value = a_value.get();
    if (const auto *vector = value.try_value())
    {
        return math::length(*vector);
    }
    // 取得失敗は既存の Script Callback 境界へ伝え、長さを返す式を中断する
    throw std::move(*value.try_error());
}
} // namespace cue
