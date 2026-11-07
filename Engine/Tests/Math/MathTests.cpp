#include <Math/Math.h>

#include <array>
#include <cmath>
#include <concepts>
#include <cstdio>
#include <limits>

namespace
{
// 失敗した式と位置を出力し、Debug Assert を無効にした構成でも検証を維持する
#define CUE_MATH_CHECK(...)                                                                                            \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(__VA_ARGS__))                                                                                            \
        {                                                                                                              \
            std::fprintf(stderr, "Math test failed at line %d: %s\n", __LINE__, #__VA_ARGS__);                         \
            return __LINE__;                                                                                           \
        }                                                                                                              \
    } while (false)

/// @brief 整数・Vector 次元・明示的な除算失敗の契約を確認する
int test_integer_vectors()
{
    using cue::math::int2;
    using cue::math::int3;
    using cue::math::uint4;
    CUE_MATH_CHECK((int2{2, 3} + int2{4, 5}) == int2{6, 8});
    CUE_MATH_CHECK(cue::math::dot(int3{1, 2, 3}, int3{4, 5, 6}) == 32);
    CUE_MATH_CHECK(cue::math::cross(int3{1, 0, 0}, int3{0, 1, 0}) == int3{0, 0, 1});
    CUE_MATH_CHECK(cue::math::hadamard(uint4{1, 2, 3, 4}, uint4{2, 3, 4, 5}) == uint4{2, 6, 12, 20});
    auto divided = cue::math::divide(int2{7, -7}, 2);
    CUE_MATH_CHECK(divided.has_value() && *divided.try_value() == int2{3, -3});
    const int2 original{(std::numeric_limits<std::int32_t>::min)(), 3};
    auto overflow = cue::math::divide(original, -1);
    auto zero = cue::math::divide(int2{1, 2}, 0);
    CUE_MATH_CHECK(!overflow.has_value() && !zero.has_value() && original.y == 3);
    CUE_MATH_CHECK(overflow.try_error()->category == cue::ErrorCategory::InvalidArgument);
    return 0;
}

/// @brief float / double の小値・巨大値を含む正規化と失敗入力を確認する
template <cue::math::Real T> int test_real_vectors()
{
    using cue::math::Vector2;
    using cue::math::Vector3;
    using cue::math::Vector4;
    constexpr T tolerance = static_cast<T>(1e-5);
    auto unit = cue::math::normalize(Vector3<T>{T{3}, T{4}, T{0}});
    CUE_MATH_CHECK(unit.has_value());
    CUE_MATH_CHECK(cue::math::nearly_equal(*unit.try_value(), Vector3<T>{T{0.6}, T{0.8}, T{0}}, tolerance));
    CUE_MATH_CHECK(cue::math::length(Vector2<T>{T{3}, T{4}}) == T{5});
    for (const T magnitude : {(std::numeric_limits<T>::max)() / T{2}, (std::numeric_limits<T>::min)(),
                              std::numeric_limits<T>::denorm_min()})
    {
        auto extreme = cue::math::normalize(Vector4<T>{magnitude, magnitude, magnitude, magnitude});
        CUE_MATH_CHECK(extreme.has_value());
        CUE_MATH_CHECK(
            cue::math::nearly_equal(*extreme.try_value(), Vector4<T>{T{0.5}, T{0.5}, T{0.5}, T{0.5}}, tolerance));
    }
    const T nan = std::numeric_limits<T>::quiet_NaN();
    const T infinity = std::numeric_limits<T>::infinity();
    for (const auto value : {Vector3<T>{}, Vector3<T>{nan, T{0}, T{0}}, Vector3<T>{infinity, T{1}, T{1}}})
    {
        auto result = cue::math::normalize(value);
        CUE_MATH_CHECK(!result.has_value());
    }
    auto division = cue::math::divide(Vector3<T>{T{1}, T{2}, T{3}}, T{2});
    CUE_MATH_CHECK(division.has_value() && division.try_value()->z == T{1.5});
    auto invalidDivide = cue::math::divide(Vector2<T>{(std::numeric_limits<T>::max)(), T{1}}, T{0.5});
    CUE_MATH_CHECK(!invalidDivide.has_value());
    CUE_MATH_CHECK(!cue::math::nearly_equal(infinity, infinity) && !cue::math::nearly_equal(nan, nan));
    CUE_MATH_CHECK(!cue::math::nearly_equal((std::numeric_limits<T>::max)(), -(std::numeric_limits<T>::max)()));
    CUE_MATH_CHECK(cue::math::nearly_equal(cue::math::degrees_to_radians(T{180}), cue::math::k_pi<T>));
    return 0;
}

/// @brief 回転の向き・Hamilton 積の順序・球面補間を既知の座標で検証する
template <cue::math::Real T> int test_quaternions()
{
    using cue::math::Quaternion;
    using cue::math::Vector3;
    constexpr T tolerance = static_cast<T>(1e-5);
    const Vector3<T> xAxis{T{1}, T{0}, T{0}};
    const Vector3<T> yAxis{T{0}, T{1}, T{0}};
    const Vector3<T> zAxis{T{0}, T{0}, T{1}};
    auto qz = cue::math::rotation_axis_angle(zAxis * T{7}, cue::math::k_pi<T> / T{2});
    auto qx = cue::math::rotation_axis_angle(xAxis, cue::math::k_pi<T> / T{2});
    auto qy = cue::math::rotation_axis_angle(yAxis, cue::math::k_pi<T> / T{2});
    CUE_MATH_CHECK(qz.has_value() && qx.has_value() && qy.has_value());
    auto turned = cue::math::rotate(*qz.try_value(), xAxis);
    CUE_MATH_CHECK(turned.has_value() && cue::math::nearly_equal(*turned.try_value(), yAxis, tolerance));
    auto composed = cue::math::rotate(*qx.try_value() * *qy.try_value(), yAxis);
    CUE_MATH_CHECK(composed.has_value() && cue::math::nearly_equal(*composed.try_value(), zAxis, tolerance));
    auto euler = cue::math::rotation_euler_xyz(Vector3<T>{cue::math::k_pi<T> / T{2}, cue::math::k_pi<T> / T{2}, T{0}});
    CUE_MATH_CHECK(euler.has_value());
    auto eulerTurned = cue::math::rotate(*euler.try_value(), xAxis);
    CUE_MATH_CHECK(eulerTurned.has_value() && cue::math::nearly_equal(*eulerTurned.try_value(), -zAxis, tolerance));
    auto half = cue::math::slerp(Quaternion<T>{}, *qz.try_value(), T{0.5});
    CUE_MATH_CHECK(half.has_value());
    auto halfTurned = cue::math::rotate(*half.try_value(), xAxis);
    const T diagonal = std::sqrt(T{0.5});
    CUE_MATH_CHECK(halfTurned.has_value() &&
                   cue::math::nearly_equal(*halfTurned.try_value(), Vector3<T>{diagonal, diagonal, T{0}}, tolerance));
    auto same = cue::math::slerp(*qz.try_value(), -*qz.try_value(), T{0.5});
    CUE_MATH_CHECK(same.has_value() &&
                   cue::math::nearly_equal(std::abs(cue::math::dot(*same.try_value(), *qz.try_value())), T{1},
                                           tolerance, tolerance));
    const Quaternion<T> nonUnit{T{1}, T{2}, T{3}, T{4}};
    auto reciprocal = cue::math::inverse(nonUnit);
    CUE_MATH_CHECK(reciprocal.has_value());
    const auto identity = nonUnit * *reciprocal.try_value();
    CUE_MATH_CHECK(cue::math::nearly_equal(identity.x, T{0}) && cue::math::nearly_equal(identity.w, T{1}));
    auto invalid = cue::math::normalize(Quaternion<T>{T{0}, T{0}, T{0}, T{0}});
    auto zeroAxis = cue::math::rotation_axis_angle(Vector3<T>{}, T{0});
    auto outOfRange = cue::math::slerp(Quaternion<T>{}, Quaternion<T>{}, T{2});
    CUE_MATH_CHECK(!invalid.has_value() && !zeroAxis.has_value() && !outOfRange.has_value());
    return 0;
}

/// @brief S → R → T の既知の変換結果、逆変換、同次座標の失敗を検証する
template <cue::math::Real T> int test_matrices_and_transform()
{
    using cue::math::Matrix4x4;
    using cue::math::Transform;
    using cue::math::Vector3;
    constexpr T tolerance = static_cast<T>(1e-4);
    auto qz = cue::math::rotation_axis_angle(Vector3<T>{T{0}, T{0}, T{1}}, cue::math::k_pi<T> / T{2});
    CUE_MATH_CHECK(qz.has_value());
    const Transform<T> transform{{T{10}, T{20}, T{30}}, *qz.try_value(), {T{2}, T{3}, T{4}}};
    auto world = transform.to_matrix();
    CUE_MATH_CHECK(world.has_value());
    const Vector3<T> point{T{1}, T{2}, T{3}};
    auto moved = cue::math::transform_point(point, *world.try_value());
    auto direction = cue::math::transform_direction(point, *world.try_value());
    CUE_MATH_CHECK(moved.has_value() &&
                   cue::math::nearly_equal(*moved.try_value(), Vector3<T>{T{4}, T{22}, T{42}}, tolerance));
    CUE_MATH_CHECK(direction.has_value() &&
                   cue::math::nearly_equal(*direction.try_value(), Vector3<T>{T{-6}, T{2}, T{12}}, tolerance));
    auto inverse = cue::math::inverse(*world.try_value());
    CUE_MATH_CHECK(inverse.has_value());
    auto restored = cue::math::transform_point(*moved.try_value(), *inverse.try_value());
    CUE_MATH_CHECK(restored.has_value() && cue::math::nearly_equal(*restored.try_value(), point, tolerance));
    CUE_MATH_CHECK(cue::math::nearly_equal(*world.try_value() * *inverse.try_value(), Matrix4x4<T>{}, tolerance));
    CUE_MATH_CHECK(cue::math::nearly_equal(*inverse.try_value() * *world.try_value(), Matrix4x4<T>{}, tolerance));
    CUE_MATH_CHECK(cue::math::transpose(cue::math::transpose(*world.try_value())) == *world.try_value());
    auto singular = cue::math::inverse(cue::math::scale_matrix(Vector3<T>{T{1}, T{0}, T{1}}));
    CUE_MATH_CHECK(!singular.has_value());
    Matrix4x4<T> dependent;
    dependent.values = {{{T{-3}, T{4}, T{-4}, T{-1}},
                         {T{-4}, T{2}, T{2}, T{2}},
                         {T{5}, T{1}, T{-2}, T{-4}},
                         {T{-7}, T{6}, T{-2}, T{1}}}};
    auto dependentInverse = cue::math::inverse(dependent);
    auto badTolerance = cue::math::inverse(Matrix4x4<T>{}, T{-1});
    CUE_MATH_CHECK(!dependentInverse.has_value() && !badTolerance.has_value());
    Matrix4x4<T> homogeneous;
    homogeneous.values[3][3] = T{2};
    auto divided = cue::math::transform_point(Vector3<T>{T{2}, T{4}, T{6}}, homogeneous);
    CUE_MATH_CHECK(divided.has_value() && *divided.try_value() == point);
    homogeneous.values[3][3] = T{0};
    auto atInfinity = cue::math::transform_point(point, homogeneous);
    CUE_MATH_CHECK(!atInfinity.has_value());
    Matrix4x4<T> pivot;
    pivot.values[0][0] = T{0};
    pivot.values[0][1] = T{2};
    pivot.values[1][0] = T{1};
    pivot.values[1][1] = T{0};
    auto pivotInverse = cue::math::inverse(pivot);
    CUE_MATH_CHECK(pivotInverse.has_value() &&
                   cue::math::nearly_equal(pivot * *pivotInverse.try_value(), Matrix4x4<T>{}));
    const T small = static_cast<T>(1e-15);
    auto smallInverse = cue::math::inverse(cue::math::scale_matrix(Vector3<T>{small, small, small}));
    CUE_MATH_CHECK(smallInverse.has_value());
    auto bad = transform;
    bad.position.x = std::numeric_limits<T>::quiet_NaN();
    auto invalid = bad.to_matrix();
    auto badMatrix = Matrix4x4<T>{};
    badMatrix.values[0][0] = std::numeric_limits<T>::infinity();
    auto invalidInverse = cue::math::inverse(badMatrix);
    CUE_MATH_CHECK(!invalid.has_value() && !invalidInverse.has_value());
    auto blended = cue::math::interpolate(Transform<T>{}, transform, T{0.5});
    CUE_MATH_CHECK(blended.has_value() && blended.try_value()->position == Vector3<T>{T{5}, T{10}, T{15}});
    CUE_MATH_CHECK(blended.try_value()->scale == Vector3<T>{T{1.5}, T{2}, T{2.5}});
    return 0;
}

/// @brief 様々な姿勢・負 Scale でも Quaternion と行列の向き、および往復変換が一致するか確認する
template <cue::math::Real T> int test_rotation_round_trips()
{
    using cue::math::Vector3;
    constexpr T tolerance = static_cast<T>(3e-4);
    for (int index = 1; index <= 40; ++index)
    {
        const T amount = static_cast<T>(index) / T{10};
        auto rotation = cue::math::rotation_euler_xyz(Vector3<T>{amount, amount * T{2}, -amount});
        CUE_MATH_CHECK(rotation.has_value());
        auto matrix = cue::math::rotation_matrix(*rotation.try_value());
        CUE_MATH_CHECK(matrix.has_value());
        const Vector3<T> direction{T{1}, T{-2}, T{3}};
        auto byQuaternion = cue::math::rotate(*rotation.try_value(), direction);
        auto byMatrix = cue::math::transform_direction(direction, *matrix.try_value());
        CUE_MATH_CHECK(byQuaternion.has_value() && byMatrix.has_value() &&
                       cue::math::nearly_equal(*byQuaternion.try_value(), *byMatrix.try_value(), tolerance));
        const cue::math::Transform<T> transform{
            {amount, -amount, amount * T{3}}, *rotation.try_value(), {T{-2}, T{3}, T{0.5}}};
        auto world = transform.to_matrix();
        CUE_MATH_CHECK(world.has_value());
        auto inverse = cue::math::inverse(*world.try_value());
        CUE_MATH_CHECK(inverse.has_value());
        CUE_MATH_CHECK(
            cue::math::nearly_equal(*world.try_value() * *inverse.try_value(), cue::math::Matrix4x4<T>{}, tolerance));
    }
    // 型 T の内部 Storage と乗算が float へ切り詰められていないことを確認する
    if constexpr (std::same_as<T, double>)
    {
        cue::math::Matrix4x4<T> matrix;
        matrix.values[0][0] = T{1} + T{1e-12};
        CUE_MATH_CHECK((matrix * cue::math::Matrix4x4<T>{}).values[0][0] > T{1});
        auto rotated = cue::math::rotation_axis_angle(Vector3<T>{T{0}, T{0}, T{1}}, T{1e-12});
        CUE_MATH_CHECK(rotated.has_value() && rotated.try_value()->z > T{0});
    }
    return 0;
}
#undef CUE_MATH_CHECK
} // namespace

/// @brief 型ごとの数学演算と失敗時の非破壊性を検証する
int main()
{
    const std::array tests{test_integer_vectors,
                           test_real_vectors<float>,
                           test_real_vectors<double>,
                           test_quaternions<float>,
                           test_quaternions<double>,
                           test_matrices_and_transform<float>,
                           test_matrices_and_transform<double>,
                           test_rotation_round_trips<float>,
                           test_rotation_round_trips<double>};
    for (const auto test : tests)
        if (test() != 0)
            return 1;
    return 0;
}
