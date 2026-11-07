#include <Math/Math.h>

#include <cstdint>
#include <type_traits>

namespace
{
template <typename T>
concept HasVector = requires { typename cue::math::Vector3<T>; };
template <typename T>
concept HasMatrix = requires { typename cue::math::Matrix4x4<T>; };
template <typename T>
concept HasQuaternion = requires { typename cue::math::Quaternion<T>; };
template <typename T>
concept HasTransform = requires { typename cue::math::Transform<T>; };
template <typename V>
concept CanNormalize = requires(V a_value) { cue::math::normalize(a_value); };
template <typename V, typename S>
concept CanScale = requires(V a_value, S a_scalar) {
    a_value * a_scalar;
    a_scalar * a_value;
};
template <typename V, typename S>
concept CanDivide = requires(V a_value, S a_scalar) { cue::math::divide(a_value, a_scalar); };

static_assert(HasVector<float> && HasVector<double> && HasVector<std::int32_t> && HasVector<std::uint32_t>);
static_assert(!HasVector<bool> && !HasVector<std::int16_t> && !HasVector<long double>);
static_assert(HasMatrix<float> && HasMatrix<double> && !HasMatrix<std::int32_t>);
static_assert(HasQuaternion<float> && HasQuaternion<double> && !HasQuaternion<std::uint32_t>);
static_assert(HasTransform<float> && HasTransform<double> && !HasTransform<int>);
static_assert(CanNormalize<cue::math::float3> && !CanNormalize<cue::math::int3>);
static_assert(CanScale<cue::math::int3, std::int32_t> && !CanScale<cue::math::int3, double>);
static_assert(CanScale<cue::math::float3, float> && !CanScale<cue::math::float3, double>);
static_assert(CanDivide<cue::math::int3, std::int32_t> && !CanDivide<cue::math::int3, double>);
static_assert(std::is_standard_layout_v<cue::math::float3> && std::is_trivially_copyable_v<cue::math::float3>);
static_assert(std::is_trivially_copyable_v<cue::math::doubleTransform>);
static_assert(sizeof(cue::math::float3) == sizeof(float) * 3);
static_assert(sizeof(cue::math::double4x4) == sizeof(double) * 16);
static_assert(cue::math::int3{1, 2, 3} + cue::math::int3{4, 5, 6} == cue::math::int3{5, 7, 9});
static_assert(cue::math::float4{1, 2, 3, 1} * cue::math::float4x4::identity() == cue::math::float4{1, 2, 3, 1});
static_assert(cue::math::Transform<double>{}.scale == cue::math::double3{1, 1, 1});
} // namespace

/// @brief Platform / ECS / Renderer なしで数学型を利用できることを Compile 時に確認する
int main()
{
    return 0;
}
