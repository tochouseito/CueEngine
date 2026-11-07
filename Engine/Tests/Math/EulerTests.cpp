#include <Math/Math.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <limits>

namespace
{
#define CUE_EULER_CHECK(...)                                                                                           \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(__VA_ARGS__))                                                                                            \
        {                                                                                                              \
            std::fprintf(stderr, "Euler test failed at line %d: %s\n", __LINE__, #__VA_ARGS__);                        \
            return __LINE__;                                                                                           \
        }                                                                                                              \
    } while (false)

/// @brief Euler の非一意性を許容し、回転した基底が往復後も一致することを検証する
template <cue::math::Real T> int test_roundtrip()
{
    using cue::math::Quaternion;
    using cue::math::Vector3;
    constexpr T halfPi = cue::math::k_pi<T> / T{2};
    constexpr T tolerance = std::same_as<T, float> ? T{0.0001} : T{0.00000001};
    const std::array rotations = {Vector3<T>{},
                                  Vector3<T>{T{0.3}, T{0.6}, T{-1.2}},
                                  Vector3<T>{T{-2.7}, T{-0.6}, T{2.1}},
                                  Vector3<T>{T{0.7}, halfPi, T{-0.8}},
                                  Vector3<T>{T{-0.7}, -halfPi, T{0.8}},
                                  Vector3<T>{T{0.7}, halfPi - T{0.0001}, T{-0.8}},
                                  Vector3<T>{T{-0.7}, -halfPi + T{0.0001}, T{0.8}},
                                  Vector3<T>{T{0.4}, T{2.2}, T{1.7}}};
    const std::array basis = {Vector3<T>{T{1}, T{0}, T{0}}, Vector3<T>{T{0}, T{1}, T{0}}, Vector3<T>{T{0}, T{0}, T{1}}};
    for (const auto &angles : rotations)
    {
        auto original = cue::math::rotation_euler_xyz(angles);
        CUE_EULER_CHECK(original.has_value());
        for (const auto q : {*original.try_value(), -*original.try_value()})
        {
            const Quaternion<T> scaled{q.x * T{3}, q.y * T{3}, q.z * T{3}, q.w * T{3}};
            auto decomposed = cue::math::euler_xyz(scaled);
            CUE_EULER_CHECK(decomposed.has_value());
            CUE_EULER_CHECK(std::abs(decomposed.try_value()->y) <= halfPi);
            auto restored = cue::math::rotation_euler_xyz(*decomposed.try_value());
            CUE_EULER_CHECK(restored.has_value());
            for (const auto axis : basis)
            {
                auto before = cue::math::rotate(q, axis);
                auto after = cue::math::rotate(*restored.try_value(), axis);
                CUE_EULER_CHECK(before.has_value() && after.has_value());
                CUE_EULER_CHECK(cue::math::nearly_equal(*before.try_value(), *after.try_value(), tolerance));
            }
        }
    }
    const T nan = std::numeric_limits<T>::quiet_NaN();
    const T infinity = std::numeric_limits<T>::infinity();
    CUE_EULER_CHECK(!cue::math::euler_xyz(Quaternion<T>{T{0}, T{0}, T{0}, T{0}}).has_value());
    CUE_EULER_CHECK(!cue::math::euler_xyz(Quaternion<T>{nan, T{0}, T{0}, T{1}}).has_value());
    CUE_EULER_CHECK(!cue::math::euler_xyz(Quaternion<T>{T{0}, infinity, T{0}, T{1}}).has_value());
    return 0;
}
} // namespace

/// @brief float / double 両方の姿勢変換と特異点を確認する
int main()
{
    if (const int failed = test_roundtrip<float>())
        return failed;
    return test_roundtrip<double>();
}
