#pragma once

#include <algorithm>
#include <cmath>
#include <limits>

#include <Math/Vector3.h>
#include <Math/Vector4.h>

namespace cue::math
{
/// @brief x / y / z / w で保持する回転の所有値。既定は単位回転、正規化は生成・変換境界で行う
/// 通常の積は Hamilton 積。a * b は Vector へ b、a の順で適用する。全 Thread で独立利用できる
template <Real T = float> struct Quaternion final
{
    T x{};
    T y{};
    T z{};
    T w{1};

    /// @brief 単位回転の所有値を返す
    [[nodiscard]] static constexpr Quaternion identity() noexcept
    {
        return {};
    }
    /// @brief 成分の厳密一致。q と -q の回転同値判定とは異なる
    [[nodiscard]] constexpr bool operator==(const Quaternion &) const noexcept = default;
};
using floatQuaternion = Quaternion<float>;
using doubleQuaternion = Quaternion<double>;

/// @brief 全成分が有限値か返す
template <Real T> [[nodiscard]] bool is_finite(const Quaternion<T> &a_value) noexcept
{
    return std::isfinite(a_value.x) && std::isfinite(a_value.y) && std::isfinite(a_value.z) && std::isfinite(a_value.w);
}
/// @brief Quaternion の符号を反転する。回転としては同じ姿勢を表す
template <Real T> [[nodiscard]] constexpr Quaternion<T> operator-(const Quaternion<T> &a_value) noexcept
{
    return {-a_value.x, -a_value.y, -a_value.z, -a_value.w};
}
/// @brief Hamilton 積を返す。中間演算が表現範囲に収まることを前提とする
template <Real T>
[[nodiscard]] constexpr Quaternion<T> operator*(const Quaternion<T> &a_left, const Quaternion<T> &a_right) noexcept
{
    return {a_left.w * a_right.x + a_left.x * a_right.w + a_left.y * a_right.z - a_left.z * a_right.y,
            a_left.w * a_right.y - a_left.x * a_right.z + a_left.y * a_right.w + a_left.z * a_right.x,
            a_left.w * a_right.z + a_left.x * a_right.y - a_left.y * a_right.x + a_left.z * a_right.w,
            a_left.w * a_right.w - a_left.x * a_right.x - a_left.y * a_right.y - a_left.z * a_right.z};
}
/// @brief Quaternion の内積を返す。主に単位 Quaternion の補間に利用する
template <Real T> [[nodiscard]] constexpr T dot(const Quaternion<T> &a_left, const Quaternion<T> &a_right) noexcept
{
    return a_left.x * a_right.x + a_left.y * a_right.y + a_left.z * a_right.z + a_left.w * a_right.w;
}
/// @brief 共役を返す。単位 Quaternion に限り逆回転と一致する
template <Real T> [[nodiscard]] constexpr Quaternion<T> conjugate(const Quaternion<T> &a_value) noexcept
{
    return {-a_value.x, -a_value.y, -a_value.z, a_value.w};
}
/// @brief ゼロ長・非有限値を拒否して単位 Quaternion を返す。入力は変更しない
template <Real T> [[nodiscard]] Result<Quaternion<T>> normalize(const Quaternion<T> &a_value)
{
    auto unit = normalize(Vector4<T>{a_value.x, a_value.y, a_value.z, a_value.w});
    if (!unit.has_value())
        return Result<Quaternion<T>>::failure(*unit.try_error());
    const auto &value = *unit.try_value();
    return Result<Quaternion<T>>::success({value.x, value.y, value.z, value.w});
}
/// @brief ゼロ長・非有限値・結果の Overflow を拒否して乗法逆元を返す
template <Real T> [[nodiscard]] Result<Quaternion<T>> inverse(const Quaternion<T> &a_value)
{
    if (!is_finite(a_value))
        return Result<Quaternion<T>>::failure({ErrorCategory::InvalidArgument, "Math.inverse.quaternion.non_finite"});
    const T scale = (std::max)({std::abs(a_value.x), std::abs(a_value.y), std::abs(a_value.z), std::abs(a_value.w)});
    if (scale == T{0})
        return Result<Quaternion<T>>::failure({ErrorCategory::InvalidArgument, "Math.inverse.quaternion.zero_length"});
    const Quaternion<T> scaled{a_value.x / scale, a_value.y / scale, a_value.z / scale, a_value.w / scale};
    const T square = dot(scaled, scaled);
    const Quaternion<T> result{-scaled.x / square / scale, -scaled.y / square / scale, -scaled.z / square / scale,
                               scaled.w / square / scale};
    if (!is_finite(result))
        return Result<Quaternion<T>>::failure({ErrorCategory::InvalidArgument, "Math.inverse.quaternion.overflow"});
    return Result<Quaternion<T>>::success(result);
}
/// @brief 任意長の軸と Radian から右手則の回転を作る。ゼロ軸・非有限値は拒否する
template <Real T> [[nodiscard]] Result<Quaternion<T>> rotation_axis_angle(const Vector3<T> &a_axis, T a_radians)
{
    if (!std::isfinite(a_radians))
        return Result<Quaternion<T>>::failure({ErrorCategory::InvalidArgument, "Math.rotation_axis_angle.angle"});
    auto axis = normalize(a_axis);
    if (!axis.has_value())
        return Result<Quaternion<T>>::failure(*axis.try_error());
    const auto &unit = *axis.try_value();
    const T sine = std::sin(a_radians / T{2});
    return normalize(Quaternion<T>{unit.x * sine, unit.y * sine, unit.z * sine, std::cos(a_radians / T{2})});
}
/// @brief 固定軸 X → Y → Z の順に回転する Radian 入力から姿勢を作る。Euler は正本に保持しない
template <Real T> [[nodiscard]] Result<Quaternion<T>> rotation_euler_xyz(const Vector3<T> &a_radians)
{
    if (!is_finite(a_radians))
        return Result<Quaternion<T>>::failure({ErrorCategory::InvalidArgument, "Math.rotation_euler_xyz.angle"});
    const T sx = std::sin(a_radians.x / T{2});
    const T cx = std::cos(a_radians.x / T{2});
    const T sy = std::sin(a_radians.y / T{2});
    const T cy = std::cos(a_radians.y / T{2});
    const T sz = std::sin(a_radians.z / T{2});
    const T cz = std::cos(a_radians.z / T{2});
    return normalize(Quaternion<T>{T{0}, T{0}, sz, cz} * Quaternion<T>{T{0}, sy, T{0}, cy} *
                     Quaternion<T>{sx, T{0}, T{0}, cx});
}
/// @brief 姿勢を固定軸 X → Y → Z の Radian に分解する。Y は [-pi/2, pi/2] とする
/// 特異点では Z=0 の同値な姿勢を返す。入力は単位化し、符号反転した Quaternion も同じ角度へ解決する
/// UI の連続編集値は呼出側で保持し、毎 Frame の往復変換を正本の更新に使わない
template <Real T> [[nodiscard]] Result<Vector3<T>> euler_xyz(const Quaternion<T> &a_rotation)
{
    // 特異点付近の差し引きで float の有効桁を失わないよう、中間計算だけ double へ広げる
    if constexpr (std::same_as<T, float>)
    {
        auto precise = euler_xyz(Quaternion<double>{a_rotation.x, a_rotation.y, a_rotation.z, a_rotation.w});
        if (!precise.has_value())
            return Result<Vector3<T>>::failure(*precise.try_error());
        const auto &value = *precise.try_value();
        return Result<Vector3<T>>::success({static_cast<T>(value.x), static_cast<T>(value.y), static_cast<T>(value.z)});
    }
    auto unit = normalize(a_rotation);
    if (!unit.has_value())
        return Result<Vector3<T>>::failure(*unit.try_error());
    const auto &q = *unit.try_value();
    const T sinY = std::clamp(T{2} * (q.w * q.y - q.z * q.x), T{-1}, T{1});
    const T r00 = T{1} - T{2} * (q.y * q.y + q.z * q.z);
    const T r10 = T{2} * (q.x * q.y + q.w * q.z);
    const T cosY = std::hypot(r00, r10);
    if (cosY <= std::numeric_limits<T>::epsilon() * T{16})
    {
        return Result<Vector3<T>>::success(
            {std::atan2(T{2} * (q.w * q.x - q.y * q.z), T{1} - T{2} * (q.x * q.x + q.z * q.z)),
             std::copysign(k_pi<T> / T{2}, sinY), T{0}});
    }
    return Result<Vector3<T>>::success(
        {std::atan2(T{2} * (q.w * q.x + q.y * q.z), T{1} - T{2} * (q.x * q.x + q.y * q.y)), std::atan2(sinY, cosY),
         std::atan2(r10, r00)});
}
/// @brief 単位化した姿勢で Vector を回す。位置や Scale は変更しない
template <Real T> [[nodiscard]] Result<Vector3<T>> rotate(const Quaternion<T> &a_rotation, const Vector3<T> &a_value)
{
    if (!is_finite(a_value))
        return Result<Vector3<T>>::failure({ErrorCategory::InvalidArgument, "Math.rotate.vector"});
    auto unit = normalize(a_rotation);
    if (!unit.has_value())
        return Result<Vector3<T>>::failure(*unit.try_error());
    const auto &q = *unit.try_value();
    const Vector3<T> axis{q.x, q.y, q.z};
    const auto twiceCross = cross(axis, a_value) * T{2};
    const auto result = a_value + twiceCross * q.w + cross(axis, twiceCross);
    if (!is_finite(result))
        return Result<Vector3<T>>::failure({ErrorCategory::InvalidArgument, "Math.rotate.overflow"});
    return Result<Vector3<T>>::success(result);
}
/// @brief 係数 [0, 1] の最短経路で球面補間し、単位 Quaternion を返す
template <Real T>
[[nodiscard]] Result<Quaternion<T>> slerp(const Quaternion<T> &a_start, const Quaternion<T> &a_end, T a_amount)
{
    if (!std::isfinite(a_amount) || a_amount < T{0} || a_amount > T{1})
        return Result<Quaternion<T>>::failure({ErrorCategory::InvalidArgument, "Math.slerp.amount"});
    auto start = normalize(a_start);
    auto end = normalize(a_end);
    if (!start.has_value())
        return Result<Quaternion<T>>::failure(*start.try_error());
    if (!end.has_value())
        return Result<Quaternion<T>>::failure(*end.try_error());
    const auto left = *start.try_value();
    auto right = *end.try_value();
    T cosine = dot(left, right);
    // q と -q が同じ回転であることを利用し、長い経路や反対符号の零補間を避ける
    if (cosine < T{0})
    {
        right = -right;
        cosine = -cosine;
    }
    cosine = std::clamp(cosine, T{0}, T{1});
    T startWeight = T{1} - a_amount;
    T endWeight = a_amount;
    if (cosine < T{0.9995})
    {
        const T angle = std::acos(cosine);
        const T sine = std::sin(angle);
        startWeight = std::sin((T{1} - a_amount) * angle) / sine;
        endWeight = std::sin(a_amount * angle) / sine;
    }
    return normalize(
        Quaternion<T>{left.x * startWeight + right.x * endWeight, left.y * startWeight + right.y * endWeight,
                      left.z * startWeight + right.z * endWeight, left.w * startWeight + right.w * endWeight});
}
} // namespace cue::math
