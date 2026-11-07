#pragma once

#include <Math/Matrix4x4.h>

namespace cue::math
{
/// @brief 位置・回転・拡縮を所有する数学型。Entity・親子・Dirty・行列 Cache は持たない
/// 既定は原点・単位回転・単位 Scale。負や零の Scale は許可するが、逆行列は存在しない場合がある
/// 任意 Thread で独立利用できる。借用は所有値の寿命まで有効、同じ値の書き換えは外部で直列化する
template <Real T = float> struct Transform final
{
    Vector3<T> position{};
    Quaternion<T> rotation{};
    Vector3<T> scale{T{1}, T{1}, T{1}};

    /// @brief 有限な S / R / T を検証し、行 Vector に S → R → T の順で作用する行列を返す
    /// 回転は変換時に単位化し、正本を変更しない。失敗時に部分行列は公開しない
    [[nodiscard]] Result<Matrix4x4<T>> to_matrix() const
    {
        if (!is_finite(position) || !is_finite(scale))
            return Result<Matrix4x4<T>>::failure(
                {ErrorCategory::InvalidArgument, "Math.Transform.to_matrix.non_finite"});
        auto matrix = rotation_matrix(rotation);
        if (!matrix.has_value())
            return Result<Matrix4x4<T>>::failure(*matrix.try_error());
        const auto result = scale_matrix(scale) * *matrix.try_value() * translation_matrix(position);
        if (!is_finite(result))
            return Result<Matrix4x4<T>>::failure({ErrorCategory::InvalidArgument, "Math.Transform.to_matrix.overflow"});
        return Result<Matrix4x4<T>>::success(result);
    }
};
using floatTransform = Transform<float>;
using doubleTransform = Transform<double>;

/// @brief 位置・Scale を線形、回転を最短経路の球面で補間する。係数は [0, 1] とする
template <Real T>
[[nodiscard]] Result<Transform<T>> interpolate(const Transform<T> &a_start, const Transform<T> &a_end, T a_amount)
{
    if (!is_finite(a_start.position) || !is_finite(a_end.position) || !is_finite(a_start.scale) ||
        !is_finite(a_end.scale))
        return Result<Transform<T>>::failure({ErrorCategory::InvalidArgument, "Math.interpolate.transform.non_finite"});
    auto rotation = slerp(a_start.rotation, a_end.rotation, a_amount);
    if (!rotation.has_value())
        return Result<Transform<T>>::failure(*rotation.try_error());
    return Result<Transform<T>>::success({lerp(a_start.position, a_end.position, a_amount), *rotation.try_value(),
                                          lerp(a_start.scale, a_end.scale, a_amount)});
}
} // namespace cue::math
