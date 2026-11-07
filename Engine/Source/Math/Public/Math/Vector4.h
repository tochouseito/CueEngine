#pragma once

#include <cassert>
#include <cstddef>

#include <Math/VectorOperations.h>

namespace cue::math
{
/// @brief 4 成分の所有値。既定はゼロ。共有書き込みを除き全 Thread で独立利用できる
/// 生配列との Union 別名参照や SIMD 固有の整列を持たず、GPU 転送形式は利用側が別に定義する
template <VectorScalar T> struct Vector4 final
{
    using scalarType = T;
    static constexpr std::size_t k_dimension = 4;
    T x{};
    T y{};
    T z{};
    T w{};

    /// @brief 成分への借用参照を返す。Index は [0, 4) が前提、参照は所有値の寿命まで有効
    [[nodiscard]] constexpr T &operator[](std::size_t a_index) noexcept
    {
        assert(a_index < k_dimension);
        if (a_index == 0)
            return x;
        if (a_index == 1)
            return y;
        if (a_index == 2)
            return z;
        return w;
    }
    /// @brief 読み取り専用の成分参照を返す。Index は [0, 4) が前提
    [[nodiscard]] constexpr const T &operator[](std::size_t a_index) const noexcept
    {
        assert(a_index < k_dimension);
        if (a_index == 0)
            return x;
        if (a_index == 1)
            return y;
        if (a_index == 2)
            return z;
        return w;
    }
    /// @brief 成分の厳密一致を返す。浮動小数の誤差を許容する比較は nearly_equal を使う
    [[nodiscard]] constexpr bool operator==(const Vector4 &) const noexcept = default;
};
using float4 = Vector4<float>;
using double4 = Vector4<double>;
using int4 = Vector4<std::int32_t>;
using uint4 = Vector4<std::uint32_t>;
} // namespace cue::math
