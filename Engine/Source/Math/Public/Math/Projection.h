#pragma once

#include <cmath>

#include <Math/Transform.h>

namespace cue::math
{
/// @brief 右手系・Camera 前方 -Z・NDC Depth [0, 1] の有限透視投影を作る
/// 行 Vector に左から掛ける。FOV は垂直方向の Radian、Aspect は表示先の幅 / 高さとする
/// 不正入力・表現範囲外では部分行列を公開しない。入力を共有変更しなければ任意 Thread で利用できる
template <Real T>
[[nodiscard]] Result<Matrix4x4<T>> perspective_rh_zo(T a_verticalFovRadians, T a_aspect, T a_nearClip, T a_farClip)
{
    if (!std::isfinite(a_verticalFovRadians) || !std::isfinite(a_aspect) || !std::isfinite(a_nearClip) ||
        !std::isfinite(a_farClip) || a_verticalFovRadians <= T{0} || a_verticalFovRadians >= k_pi<T> ||
        a_aspect <= T{0} || a_nearClip <= T{0} || a_farClip <= a_nearClip)
        return Result<Matrix4x4<T>>::failure({ErrorCategory::InvalidArgument, "Math.perspective.parameters"});

    const T yScale = T{1} / std::tan(a_verticalFovRadians / T{2});
    const T xScale = yScale / a_aspect;
    // near * far の積を先に作らず、巨大な Clip 距離での不要な Overflow を避ける
    const T depthScale = a_farClip / (a_nearClip - a_farClip);
    const T depthTranslation = a_nearClip * depthScale;
    auto result = Matrix4x4<T>::zero();
    result.values[0][0] = xScale;
    result.values[1][1] = yScale;
    result.values[2][2] = depthScale;
    result.values[2][3] = T{-1};
    result.values[3][2] = depthTranslation;
    if (!is_finite(result) || xScale <= T{0} || yScale <= T{0} || depthTranslation >= T{0})
        return Result<Matrix4x4<T>>::failure({ErrorCategory::InvalidArgument, "Math.perspective.range"});
    return Result<Matrix4x4<T>>::success(result);
}

/// @brief Transform の位置・回転から Camera の View を作り、有限な Scale は投影へ影響させない
/// Camera は右手系・上方向 +Y・正面 -Z。姿勢は正規化し、入力や行列 Cache を変更しない
/// 非有限 Transform・不正回転・計算範囲外では失敗する。任意 Thread で独立利用できる
template <Real T> [[nodiscard]] Result<Matrix4x4<T>> view_from_transform(const Transform<T> &a_transform)
{
    if (!is_finite(a_transform.position) || !is_finite(a_transform.scale))
        return Result<Matrix4x4<T>>::failure({ErrorCategory::InvalidArgument, "Math.view.transform"});
    auto rotation = rotation_matrix(a_transform.rotation);
    if (!rotation.has_value())
        return Result<Matrix4x4<T>>::failure(*rotation.try_error());
    // Camera の拡縮で空間が歪まないよう、剛体姿勢の逆変換だけを構築する
    const auto result = translation_matrix(-a_transform.position) * transpose(*rotation.try_value());
    if (!is_finite(result))
        return Result<Matrix4x4<T>>::failure({ErrorCategory::InvalidArgument, "Math.view.range"});
    return Result<Matrix4x4<T>>::success(result);
}
} // namespace cue::math
