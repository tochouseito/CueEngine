#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>

#include <Math/Quaternion.h>

namespace cue::math
{
/// @brief 行優先で values[row][column] に保持する 4x4 の所有値。既定は単位行列
/// 行 Vector を左から掛ける。GPU 配置・投影の Depth 範囲・Camera の向きはこの型で規定しない
/// 借用は所有値の寿命まで有効。同じ値の変更を除き全 Thread で独立利用できる
template <Real T = float> struct Matrix4x4 final
{
    std::array<std::array<T, 4>, 4> values{
        {{T{1}, T{0}, T{0}, T{0}}, {T{0}, T{1}, T{0}, T{0}}, {T{0}, T{0}, T{1}, T{0}}, {T{0}, T{0}, T{0}, T{1}}}};

    /// @brief 単位行列を返す
    [[nodiscard]] static constexpr Matrix4x4 identity() noexcept
    {
        return {};
    }
    /// @brief 全成分がゼロの行列を返す
    [[nodiscard]] static constexpr Matrix4x4 zero() noexcept
    {
        Matrix4x4 result;
        result.values = {};
        return result;
    }
    /// @brief 全成分の厳密一致を返す
    [[nodiscard]] constexpr bool operator==(const Matrix4x4 &) const noexcept = default;
};
using float4x4 = Matrix4x4<float>;
using double4x4 = Matrix4x4<double>;

/// @brief 行列積を返す。行 Vector では左側、右側の順で適用する。表現可能な中間値を前提とする
template <Real T>
[[nodiscard]] constexpr Matrix4x4<T> operator*(const Matrix4x4<T> &a_left, const Matrix4x4<T> &a_right) noexcept
{
    auto result = Matrix4x4<T>::zero();
    for (std::size_t row = 0; row < 4; ++row)
        for (std::size_t column = 0; column < 4; ++column)
            for (std::size_t index = 0; index < 4; ++index)
                result.values[row][column] += a_left.values[row][index] * a_right.values[index][column];
    return result;
}
/// @brief 左の行 Vector へ行列を適用する。位置は w=1、方向は w=0 を使う
template <Real T>
[[nodiscard]] constexpr Vector4<T> operator*(const Vector4<T> &a_value, const Matrix4x4<T> &a_matrix) noexcept
{
    Vector4<T> result;
    for (std::size_t column = 0; column < 4; ++column)
        for (std::size_t row = 0; row < 4; ++row)
            result[column] += a_value[row] * a_matrix.values[row][column];
    return result;
}
/// @brief 入力を変更せず転置する
template <Real T> [[nodiscard]] constexpr Matrix4x4<T> transpose(const Matrix4x4<T> &a_matrix) noexcept
{
    auto result = Matrix4x4<T>::zero();
    for (std::size_t row = 0; row < 4; ++row)
        for (std::size_t column = 0; column < 4; ++column)
            result.values[row][column] = a_matrix.values[column][row];
    return result;
}
/// @brief 全成分が有限値か返す
template <Real T> [[nodiscard]] bool is_finite(const Matrix4x4<T> &a_matrix) noexcept
{
    for (const auto &row : a_matrix.values)
        for (const T value : row)
            if (!std::isfinite(value))
                return false;
    return true;
}
/// @brief 全成分を絶対・相対許容誤差で比較する
template <Real T>
[[nodiscard]] bool nearly_equal(const Matrix4x4<T> &a_left, const Matrix4x4<T> &a_right,
                                T a_tolerance = std::numeric_limits<T>::epsilon() * T{16}) noexcept
{
    for (std::size_t row = 0; row < 4; ++row)
        for (std::size_t column = 0; column < 4; ++column)
            if (!nearly_equal(a_left.values[row][column], a_right.values[row][column], a_tolerance, a_tolerance))
                return false;
    return true;
}
/// @brief 部分 Pivot 選択で逆行列を求め、左右の積が単位行列へ戻ることを検証する
/// 入力は変更しない。零 Pivot・非有限値・中間 Overflow・絶対残差の許容超過は失敗とする
/// 許容値は成分ごとの最大絶対誤差。大きな座標では精度に合わせた値を明示できるが Condition Number は評価しない
template <Real T>
[[nodiscard]] Result<Matrix4x4<T>> inverse(const Matrix4x4<T> &a_matrix,
                                           T a_residualTolerance = std::numeric_limits<T>::epsilon() * T{128})
{
    if (!is_finite(a_matrix) || !std::isfinite(a_residualTolerance) || a_residualTolerance < T{0})
        return Result<Matrix4x4<T>>::failure({ErrorCategory::InvalidArgument, "Math.inverse.matrix.non_finite"});
    std::array<std::array<T, 8>, 4> augmented{};
    for (std::size_t row = 0; row < 4; ++row)
    {
        for (std::size_t column = 0; column < 4; ++column)
            augmented[row][column] = a_matrix.values[row][column];
        augmented[row][row + 4] = T{1};
    }
    for (std::size_t column = 0; column < 4; ++column)
    {
        // 最大絶対値の Pivot を使い、不要な小さい値での除算を避ける
        std::size_t pivot = column;
        for (std::size_t row = column + 1; row < 4; ++row)
            if (std::abs(augmented[row][column]) > std::abs(augmented[pivot][column]))
                pivot = row;
        if (augmented[pivot][column] == T{0})
            return Result<Matrix4x4<T>>::failure({ErrorCategory::InvalidArgument, "Math.inverse.matrix.singular"});
        std::swap(augmented[pivot], augmented[column]);
        const T divisor = augmented[column][column];
        for (auto &value : augmented[column])
            value /= divisor;
        // Pivot 行で他の行の対象列を消去する
        for (std::size_t row = 0; row < 4; ++row)
        {
            if (row == column)
                continue;
            const T factor = augmented[row][column];
            if (factor == T{0})
                continue;
            for (std::size_t index = 0; index < 8; ++index)
                augmented[row][index] -= factor * augmented[column][index];
        }
        for (const auto &row : augmented)
            for (const T value : row)
                if (!std::isfinite(value))
                    return Result<Matrix4x4<T>>::failure(
                        {ErrorCategory::InvalidArgument, "Math.inverse.matrix.overflow"});
    }
    auto result = Matrix4x4<T>::zero();
    for (std::size_t row = 0; row < 4; ++row)
        for (std::size_t column = 0; column < 4; ++column)
            result.values[row][column] = augmented[row][column + 4];
    // 丸め誤差で零 Pivot が微小値になった特異行列を、有限な結果だけで成功にしない
    for (const auto &product : {a_matrix * result, result * a_matrix})
        for (std::size_t row = 0; row < 4; ++row)
            for (std::size_t column = 0; column < 4; ++column)
            {
                const T expected = row == column ? T{1} : T{0};
                const T error = std::abs(product.values[row][column] - expected);
                if (!std::isfinite(error) || error > a_residualTolerance)
                    return Result<Matrix4x4<T>>::failure(
                        {ErrorCategory::InvalidArgument, "Math.inverse.matrix.residual"});
            }
    return Result<Matrix4x4<T>>::success(result);
}
/// @brief 平行移動を最後の行に持つ行 Vector 用行列を作る。有限入力を前提とする
template <Real T> [[nodiscard]] constexpr Matrix4x4<T> translation_matrix(const Vector3<T> &a_position) noexcept
{
    Matrix4x4<T> result;
    result.values[3][0] = a_position.x;
    result.values[3][1] = a_position.y;
    result.values[3][2] = a_position.z;
    return result;
}
/// @brief 対角に Scale を持つ行列を作る。零・負の Scale も許可する
template <Real T> [[nodiscard]] constexpr Matrix4x4<T> scale_matrix(const Vector3<T> &a_scale) noexcept
{
    Matrix4x4<T> result;
    result.values[0][0] = a_scale.x;
    result.values[1][1] = a_scale.y;
    result.values[2][2] = a_scale.z;
    return result;
}
/// @brief Quaternion を単位化して行 Vector 用の回転行列を作る。無効な回転は失敗とする
template <Real T> [[nodiscard]] Result<Matrix4x4<T>> rotation_matrix(const Quaternion<T> &a_rotation)
{
    auto unit = normalize(a_rotation);
    if (!unit.has_value())
        return Result<Matrix4x4<T>>::failure(*unit.try_error());
    const auto &q = *unit.try_value();
    Matrix4x4<T> result;
    result.values[0][0] = T{1} - T{2} * (q.y * q.y + q.z * q.z);
    result.values[0][1] = T{2} * (q.x * q.y + q.z * q.w);
    result.values[0][2] = T{2} * (q.x * q.z - q.y * q.w);
    result.values[1][0] = T{2} * (q.x * q.y - q.z * q.w);
    result.values[1][1] = T{1} - T{2} * (q.x * q.x + q.z * q.z);
    result.values[1][2] = T{2} * (q.y * q.z + q.x * q.w);
    result.values[2][0] = T{2} * (q.x * q.z + q.y * q.w);
    result.values[2][1] = T{2} * (q.y * q.z - q.x * q.w);
    result.values[2][2] = T{1} - T{2} * (q.x * q.x + q.y * q.y);
    return Result<Matrix4x4<T>>::success(result);
}
/// @brief 位置を変換し、w で除算する。非有限値・w=0・結果 Overflow を拒否する
template <Real T>
[[nodiscard]] Result<Vector3<T>> transform_point(const Vector3<T> &a_point, const Matrix4x4<T> &a_matrix)
{
    if (!is_finite(a_point) || !is_finite(a_matrix))
        return Result<Vector3<T>>::failure({ErrorCategory::InvalidArgument, "Math.transform_point.non_finite"});
    const auto value = Vector4<T>{a_point.x, a_point.y, a_point.z, T{1}} * a_matrix;
    if (value.w == T{0} || !is_finite(value))
        return Result<Vector3<T>>::failure({ErrorCategory::InvalidArgument, "Math.transform_point.homogeneous"});
    return divide(Vector3<T>{value.x, value.y, value.z}, value.w);
}
/// @brief 平行移動を除いて方向を変換する。法線はこの関数ではなく逆転置行列で変換する
template <Real T>
[[nodiscard]] Result<Vector3<T>> transform_direction(const Vector3<T> &a_direction, const Matrix4x4<T> &a_matrix)
{
    if (!is_finite(a_direction) || !is_finite(a_matrix))
        return Result<Vector3<T>>::failure({ErrorCategory::InvalidArgument, "Math.transform_direction.non_finite"});
    const auto value = Vector4<T>{a_direction.x, a_direction.y, a_direction.z, T{0}} * a_matrix;
    if (!is_finite(value) || value.w != T{0})
        return Result<Vector3<T>>::failure({ErrorCategory::InvalidArgument, "Math.transform_direction.homogeneous"});
    return Result<Vector3<T>>::success({value.x, value.y, value.z});
}
} // namespace cue::math
