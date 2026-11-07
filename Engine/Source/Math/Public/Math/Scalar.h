#pragma once

#include <algorithm>
#include <cmath>
#include <concepts>
#include <cstdint>
#include <limits>
#include <numbers>

#include <Foundation/Result.h>

namespace cue::math
{
/// @brief 浮動小数の数学演算は float / double に限定する
template <typename T>
concept Real = std::same_as<T, float> || std::same_as<T, double>;
/// @brief Vector の成分は実数または 32-bit 整数とする。bool / 狭い整数は含めない
template <typename T>
concept VectorScalar = Real<T> || std::same_as<T, std::int32_t> || std::same_as<T, std::uint32_t>;

template <Real T> inline constexpr T k_pi = std::numbers::pi_v<T>;

/// @brief 弧度法へ変換する。通常演算と同じく表現範囲内の入力を前提とする
template <Real T> [[nodiscard]] constexpr T degrees_to_radians(T a_degrees) noexcept
{
    return a_degrees * (k_pi<T> / T{180});
}
/// @brief 度数法へ変換する
template <Real T> [[nodiscard]] constexpr T radians_to_degrees(T a_radians) noexcept
{
    return a_radians * (T{180} / k_pi<T>);
}
/// @brief NaN / Infinity を同値とせず、絶対誤差と相対誤差のいずれかで比較する
template <Real T>
[[nodiscard]] bool nearly_equal(T a_left, T a_right, T a_absolute = std::numeric_limits<T>::epsilon() * T{16},
                                T a_relative = std::numeric_limits<T>::epsilon() * T{16}) noexcept
{
    if (!std::isfinite(a_left) || !std::isfinite(a_right) || !std::isfinite(a_absolute) || !std::isfinite(a_relative) ||
        a_absolute < T{0} || a_relative < T{0})
        return false;
    if (a_left == a_right)
        return true;
    const T magnitude = (std::max)(std::abs(a_left), std::abs(a_right));
    // 同符号・異符号の巨大値でも差や許容誤差の Overflow で誤って一致させない
    return std::abs(a_left - a_right) <= a_absolute ||
           (magnitude > T{0} && std::abs(a_left / magnitude - a_right / magnitude) <= a_relative);
}
/// @brief 線形補間する。範囲外の係数は外挿となり、有限で表現可能な結果を前提とする
template <Real T> [[nodiscard]] T lerp(T a_start, T a_end, T a_amount) noexcept
{
    return std::lerp(a_start, a_end, a_amount);
}
} // namespace cue::math
