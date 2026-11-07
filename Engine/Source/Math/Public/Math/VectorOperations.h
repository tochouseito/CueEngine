#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

#include <Math/Scalar.h>

namespace cue::math
{
template <VectorScalar T> struct Vector2;
template <VectorScalar T> struct Vector3;
template <VectorScalar T> struct Vector4;

/// @brief 共通演算を Math 所有の Vector 型だけに適用する
template <typename V>
concept VectorType = requires { typename V::scalarType; } && (std::same_as<V, Vector2<typename V::scalarType>> ||
                                                              std::same_as<V, Vector3<typename V::scalarType>> ||
                                                              std::same_as<V, Vector4<typename V::scalarType>>);

// 通常演算は各成分が型の表現範囲に収まることを前提とする。整数の暗黙昇格・型混在は行わない
/// @brief 成分ごとの和を新しい所有値で返す
template <VectorType V> [[nodiscard]] constexpr V operator+(V a_left, const V &a_right) noexcept
{
    for (std::size_t index = 0; index < V::k_dimension; ++index)
        a_left[index] += a_right[index];
    return a_left;
}
/// @brief 成分ごとの差を新しい所有値で返す
template <VectorType V> [[nodiscard]] constexpr V operator-(V a_left, const V &a_right) noexcept
{
    for (std::size_t index = 0; index < V::k_dimension; ++index)
        a_left[index] -= a_right[index];
    return a_left;
}
/// @brief 同じ型の Vector を加算し、所有値を更新する
template <VectorType V> constexpr V &operator+=(V &a_left, const V &a_right) noexcept
{
    a_left = a_left + a_right;
    return a_left;
}
/// @brief 同じ型の Vector を減算し、所有値を更新する
template <VectorType V> constexpr V &operator-=(V &a_left, const V &a_right) noexcept
{
    a_left = a_left - a_right;
    return a_left;
}
/// @brief 成分の符号を反転する。符号なし Vector には提供しない
template <VectorType V>
    requires(!std::unsigned_integral<typename V::scalarType>)
[[nodiscard]] constexpr V operator-(V a_value) noexcept
{
    for (std::size_t index = 0; index < V::k_dimension; ++index)
        a_value[index] = -a_value[index];
    return a_value;
}
/// @brief 全成分に同じ型の Scalar を掛ける
template <VectorType V, typename S>
    requires std::same_as<S, typename V::scalarType>
[[nodiscard]] constexpr V operator*(V a_value, S a_scalar) noexcept
{
    for (std::size_t index = 0; index < V::k_dimension; ++index)
        a_value[index] *= a_scalar;
    return a_value;
}
/// @brief 左側の Scalar を Vector の各成分に掛ける
template <VectorType V, typename S>
    requires std::same_as<S, typename V::scalarType>
[[nodiscard]] constexpr V operator*(S a_scalar, V a_value) noexcept
{
    return a_value * a_scalar;
}
/// @brief 成分型と一致する Scalar を乗算し、所有値を更新する
template <VectorType V, typename S>
    requires std::same_as<S, typename V::scalarType>
constexpr V &operator*=(V &a_value, S a_scalar) noexcept
{
    a_value = a_value * a_scalar;
    return a_value;
}
/// @brief 成分ごとの積を返す。内積とは区別する
template <VectorType V> [[nodiscard]] constexpr V hadamard(V a_left, const V &a_right) noexcept
{
    for (std::size_t index = 0; index < V::k_dimension; ++index)
        a_left[index] *= a_right[index];
    return a_left;
}
/// @brief 内積を返す。整数では積と総和が表現範囲内であることを前提とする
template <VectorType V> [[nodiscard]] constexpr typename V::scalarType dot(const V &a_left, const V &a_right) noexcept
{
    typename V::scalarType result{};
    for (std::size_t index = 0; index < V::k_dimension; ++index)
        result += a_left[index] * a_right[index];
    return result;
}
/// @brief 全成分が有限値か返す
template <VectorType V>
    requires Real<typename V::scalarType>
[[nodiscard]] bool is_finite(const V &a_value) noexcept
{
    for (std::size_t index = 0; index < V::k_dimension; ++index)
        if (!std::isfinite(a_value[index]))
            return false;
    return true;
}
/// @brief 全成分を同じ絶対・相対許容誤差で比較する
template <VectorType V>
    requires Real<typename V::scalarType>
[[nodiscard]] bool nearly_equal(
    const V &a_left, const V &a_right,
    typename V::scalarType a_tolerance = std::numeric_limits<typename V::scalarType>::epsilon() * 16) noexcept
{
    for (std::size_t index = 0; index < V::k_dimension; ++index)
        if (!nearly_equal(a_left[index], a_right[index], a_tolerance, a_tolerance))
            return false;
    return true;
}
/// @brief 二乗の途中 Overflow / Underflow を避けて長さを求める。結果自体が範囲外なら Infinity となる
template <VectorType V>
    requires Real<typename V::scalarType>
[[nodiscard]] typename V::scalarType length(const V &a_value) noexcept
{
    using T = typename V::scalarType;
    T result{};
    for (std::size_t index = 0; index < V::k_dimension; ++index)
        result = std::hypot(result, a_value[index]);
    return result;
}
/// @brief ゼロ長・非有限値を拒否して単位 Vector を返す。入力を変更せず、全 Thread で独立利用できる
template <VectorType V>
    requires Real<typename V::scalarType>
[[nodiscard]] Result<V> normalize(const V &a_value)
{
    using T = typename V::scalarType;
    if (!is_finite(a_value))
        return Result<V>::failure({ErrorCategory::InvalidArgument, "Math.normalize.vector.non_finite"});
    T scale{};
    for (std::size_t index = 0; index < V::k_dimension; ++index)
        scale = (std::max)(scale, std::abs(a_value[index]));
    if (scale == T{0})
        return Result<V>::failure({ErrorCategory::InvalidArgument, "Math.normalize.vector.zero_length"});
    // 最大成分で先に割り、非常に小さい / 大きい入力も正規化する
    V result = a_value;
    for (std::size_t index = 0; index < V::k_dimension; ++index)
        result[index] /= scale;
    const T magnitude = std::sqrt(dot(result, result));
    for (std::size_t index = 0; index < V::k_dimension; ++index)
        result[index] /= magnitude;
    return Result<V>::success(result);
}
/// @brief ゼロ除算、非有限値、整数の最小値 / -1 を変更前に拒否する
template <VectorType V, typename S>
    requires std::same_as<S, typename V::scalarType>
[[nodiscard]] Result<V> divide(V a_value, S a_divisor)
{
    using T = typename V::scalarType;
    if (a_divisor == T{0})
        return Result<V>::failure({ErrorCategory::InvalidArgument, "Math.divide.zero_divisor"});
    if constexpr (Real<T>)
    {
        if (!std::isfinite(a_divisor) || !is_finite(a_value))
            return Result<V>::failure({ErrorCategory::InvalidArgument, "Math.divide.non_finite"});
    }
    for (std::size_t index = 0; index < V::k_dimension; ++index)
    {
        if constexpr (std::signed_integral<T>)
            if (a_value[index] == (std::numeric_limits<T>::min)() && a_divisor == T{-1})
                return Result<V>::failure({ErrorCategory::InvalidArgument, "Math.divide.overflow"});
        a_value[index] /= a_divisor;
    }
    if constexpr (Real<T>)
        if (!is_finite(a_value))
            return Result<V>::failure({ErrorCategory::InvalidArgument, "Math.divide.overflow"});
    return Result<V>::success(a_value);
}
/// @brief Vector の各成分を線形補間する。係数は有限で、結果が表現可能なことを前提とする
template <VectorType V, typename S>
    requires Real<typename V::scalarType> && std::same_as<S, typename V::scalarType>
[[nodiscard]] V lerp(V a_start, const V &a_end, S a_amount) noexcept
{
    for (std::size_t index = 0; index < V::k_dimension; ++index)
        a_start[index] = lerp(a_start[index], a_end[index], a_amount);
    return a_start;
}
} // namespace cue::math
