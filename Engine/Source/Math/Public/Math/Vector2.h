#pragma once

#include <cassert>
#include <cstddef>

#include <Math/VectorOperations.h>

namespace cue::math
{
/// @brief 2 成分の所有値。既定はゼロ。共有書き込みを除き全 Thread で独立利用できる
/// 生配列との Union 別名参照や SIMD 固有の整列を持たず、GPU 転送形式は利用側が別に定義する
template <VectorScalar T> struct Vector2 final
{
    using scalarType = T;
    static constexpr std::size_t k_dimension = 2;
    T x{};
    T y{};

    /// @brief 成分への借用参照を返す。Index は [0, 2) が前提、参照は所有値の寿命まで有効
    [[nodiscard]] constexpr T &operator[](std::size_t a_index) noexcept
    {
        assert(a_index < k_dimension);
        if (a_index == 0)
            return x;
        return y;
    }
    /// @brief 読み取り専用の成分参照を返す。Index は [0, 2) が前提
    [[nodiscard]] constexpr const T &operator[](std::size_t a_index) const noexcept
    {
        assert(a_index < k_dimension);
        if (a_index == 0)
            return x;
        return y;
    }
    /// @brief 成分の厳密一致を返す。浮動小数の誤差を許容する比較は nearly_equal を使う
    [[nodiscard]] constexpr bool operator==(const Vector2 &) const noexcept = default;
};
using float2 = Vector2<float>;
using double2 = Vector2<double>;
using int2 = Vector2<std::int32_t>;
using uint2 = Vector2<std::uint32_t>;
} // namespace cue::math
