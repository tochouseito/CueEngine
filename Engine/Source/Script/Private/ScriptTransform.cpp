#include <Script/ScriptTransform.h>

#include <utility>

#include <Script/Behaviour.h>

namespace cue
{
namespace
{
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

ScriptTransform::ScriptTransform(Behaviour &a_owner) noexcept
    : position(*this, CUE_SCRIPT_TRANSFORM_POSITION), scale(*this, CUE_SCRIPT_TRANSFORM_SCALE), m_owner(a_owner)
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
} // namespace cue
