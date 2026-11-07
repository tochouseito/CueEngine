#pragma once

#include <cassert>
#include <cstddef>

#include <Math/VectorOperations.h>

namespace cue::math
{
/// @brief 3 成分の所有値。既定はゼロ。共有書き込みを除き全 Thread で独立利用できる
/// 生配列との Union 別名参照や SIMD 固有の整列を持たず、GPU 転送形式は利用側が別に定義する
template <VectorScalar T> struct Vector3 final
{
    using scalarType = T;
    static constexpr std::size_t k_dimension = 3;
    T x{};
    T y{};
    T z{};

    /// @brief 成分への借用参照を返す。Index は [0, 3) が前提、参照は所有値の寿命まで有効
    [[nodiscard]] constexpr T &operator[](std::size_t a_index) noexcept
    {
        assert(a_index < k_dimension);
        if (a_index == 0)
            return x;
        if (a_index == 1)
            return y;
        return z;
    }
    /// @brief 読み取り専用の成分参照を返す。Index は [0, 3) が前提
    [[nodiscard]] constexpr const T &operator[](std::size_t a_index) const noexcept
    {
        assert(a_index < k_dimension);
        if (a_index == 0)
            return x;
        if (a_index == 1)
            return y;
        return z;
    }
    /// @brief 成分の厳密一致を返す。浮動小数の誤差を許容する比較は nearly_equal を使う
    [[nodiscard]] constexpr bool operator==(const Vector3 &) const noexcept = default;
};
using float3 = Vector3<float>;
using double3 = Vector3<double>;
using int3 = Vector3<std::int32_t>;
using uint3 = Vector3<std::uint32_t>;

/// @brief 右手則の外積を返す。整数では中間値が表現範囲内であることを前提とする
template <VectorScalar T>
[[nodiscard]] constexpr Vector3<T> cross(const Vector3<T> &a_left, const Vector3<T> &a_right) noexcept
{
    return {a_left.y * a_right.z - a_left.z * a_right.y, a_left.z * a_right.x - a_left.x * a_right.z,
            a_left.x * a_right.y - a_left.y * a_right.x};
}
} // namespace cue::math
