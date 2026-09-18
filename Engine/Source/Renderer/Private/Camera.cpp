#include <Cue/Renderer/Camera.h>

#include <Cue/Foundation/EmergencyHandler.h>
#include <Cue/Math/Angle.h>

#include <cmath>
#include <cstddef>
#include <utility>

namespace
{
/// @brief Quaternionを行Vector規約の回転Matrixへ展開する
[[nodiscard]] cue::math::Matrix4 make_rotation(cue::math::Quaternion a_rotation) noexcept
{
    const float xx = a_rotation.x * a_rotation.x;
    const float yy = a_rotation.y * a_rotation.y;
    const float zz = a_rotation.z * a_rotation.z;
    const float xy = a_rotation.x * a_rotation.y;
    const float xz = a_rotation.x * a_rotation.z;
    const float yz = a_rotation.y * a_rotation.z;
    const float xw = a_rotation.x * a_rotation.w;
    const float yw = a_rotation.y * a_rotation.w;
    const float zw = a_rotation.z * a_rotation.w;

    cue::math::Matrix4 result;
    result.values[0][0] = 1.0F - 2.0F * (yy + zz);
    result.values[0][1] = 2.0F * (xy + zw);
    result.values[0][2] = 2.0F * (xz - yw);
    result.values[1][0] = 2.0F * (xy - zw);
    result.values[1][1] = 1.0F - 2.0F * (xx + zz);
    result.values[1][2] = 2.0F * (yz + xw);
    result.values[2][0] = 2.0F * (xz + yw);
    result.values[2][1] = 2.0F * (yz - xw);
    result.values[2][2] = 1.0F - 2.0F * (xx + yy);
    return result;
}

/// @brief CameraのScaleを無視したRigid Transform逆Matrixを生成する
[[nodiscard]] cue::math::Matrix4 make_view(const cue::math::Transform &a_transform) noexcept
{
    const cue::math::Matrix4 rotation = make_rotation(a_transform.rotation());
    const cue::math::Vector3 position = a_transform.translation();
    cue::math::Matrix4 view;

    for (std::size_t row = 0U; row < 3U; ++row)
    {
        for (std::size_t column = 0U; column < 3U; ++column)
        {
            view.values[row][column] = rotation.values[column][row];
        }
    }
    view.values[3][0] =
        -(position.x * rotation.values[0][0] + position.y * rotation.values[0][1] + position.z * rotation.values[0][2]);
    view.values[3][1] =
        -(position.x * rotation.values[1][0] + position.y * rotation.values[1][1] + position.z * rotation.values[1][2]);
    view.values[3][2] =
        -(position.x * rotation.values[2][0] + position.y * rotation.values[2][1] + position.z * rotation.values[2][2]);
    return view;
}
} // namespace

namespace cue::renderer
{
Result<DebugCamera> DebugCamera::create_default(EmergencyHandler &a_emergencyHandler) noexcept
{
    Result<math::Tolerance> tolerance = math::Tolerance::create(a_emergencyHandler, 0.00001F, 0.00001F);
    if (!tolerance)
    {
        return Result<DebugCamera>::failure(std::move(*tolerance.try_error()));
    }

    const float halfAngle = math::to_radians(math::Degrees(18.0F)).value * 0.5F;
    const math::Quaternion rotation{std::sin(halfAngle), 0.0F, 0.0F, std::cos(halfAngle)};
    Result<math::Transform> transform = math::Transform::create(a_emergencyHandler, {0.0F, 3.0F, -6.0F}, rotation,
                                                                {1.0F, 1.0F, 1.0F}, *tolerance.try_value());
    if (!transform)
    {
        return Result<DebugCamera>::failure(std::move(*transform.try_error()));
    }

    PerspectiveCamera camera{std::move(*transform.try_value()), math::to_radians(math::Degrees(60.0F)).value, 0.1F,
                             1000.0F};
    return Result<DebugCamera>::success(DebugCamera(std::move(camera)));
}

DebugCamera::DebugCamera(PerspectiveCamera a_camera) noexcept : m_camera(std::move(a_camera))
{
}

const PerspectiveCamera &DebugCamera::camera() const noexcept
{
    return m_camera;
}

math::Matrix4 make_world_matrix(const math::Transform &a_transform) noexcept
{
    math::Matrix4 result = make_rotation(a_transform.rotation());
    const math::Vector3 scale = a_transform.scale();
    for (std::size_t column = 0U; column < 3U; ++column)
    {
        result.values[0][column] *= scale.x;
        result.values[1][column] *= scale.y;
        result.values[2][column] *= scale.z;
    }
    const math::Vector3 translation = a_transform.translation();
    result.values[3][0] = translation.x;
    result.values[3][1] = translation.y;
    result.values[3][2] = translation.z;
    return result;
}

math::Matrix4 make_view_projection(const PerspectiveCamera &a_camera, float a_aspectRatio) noexcept
{
    const float halfFov = a_camera.verticalFovRadians * 0.5F;
    const float yScale = 1.0F / std::tan(halfFov);
    const float xScale = yScale / a_aspectRatio;
    const float depthScale = a_camera.farPlane / (a_camera.farPlane - a_camera.nearPlane);

    math::Matrix4 projection = math::zero_matrix4();
    projection.values[0][0] = xScale;
    projection.values[1][1] = yScale;
    projection.values[2][2] = depthScale;
    projection.values[2][3] = 1.0F;
    projection.values[3][2] = -a_camera.nearPlane * depthScale;
    return make_view(a_camera.transform) * projection;
}

bool is_valid(const PerspectiveCamera &a_camera) noexcept
{
    return math::is_finite(a_camera.verticalFovRadians) && math::is_finite(a_camera.nearPlane) &&
           math::is_finite(a_camera.farPlane) && a_camera.verticalFovRadians > 0.0F &&
           a_camera.verticalFovRadians < math::pi() && a_camera.nearPlane > 0.0F &&
           a_camera.farPlane > a_camera.nearPlane;
}
} // namespace cue::renderer
