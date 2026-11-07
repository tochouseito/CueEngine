#include <Math/Projection.h>

#include <cmath>
#include <cstdio>
#include <limits>

namespace
{
#define CHECK(...)                                                                                                     \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(__VA_ARGS__))                                                                                            \
        {                                                                                                              \
            std::fprintf(stderr, "Projection test line %d\n", __LINE__);                                               \
            return false;                                                                                              \
        }                                                                                                              \
    } while (false)

/// @brief float / double の Frustum 境界、Depth 範囲、Camera 姿勢を既知の値で検証する
template <cue::math::Real T> bool verify_projection()
{
    using namespace cue::math;
    const T fov = degrees_to_radians(T{60});
    const T aspect = T{2};
    const T nearClip = T{0.25};
    const T farClip = T{100};
    const T tolerance = static_cast<T>(1e-5);
    auto projection = perspective_rh_zo(fov, aspect, nearClip, farClip);
    CHECK(projection.has_value());
    auto nearPoint = transform_point(Vector3<T>{T{0}, T{0}, -nearClip}, *projection.try_value());
    auto farPoint = transform_point(Vector3<T>{T{0}, T{0}, -farClip}, *projection.try_value());
    CHECK(nearPoint.has_value() && farPoint.has_value());
    CHECK(nearly_equal(nearPoint.try_value()->z, T{0}, tolerance, tolerance));
    CHECK(nearly_equal(farPoint.try_value()->z, T{1}, tolerance, tolerance));
    const T top = nearClip * std::tan(fov / T{2});
    auto corner = transform_point(Vector3<T>{top * aspect, top, -nearClip}, *projection.try_value());
    CHECK(corner.has_value() && nearly_equal(corner.try_value()->x, T{1}, tolerance, tolerance) &&
          nearly_equal(corner.try_value()->y, T{1}, tolerance, tolerance));

    Transform<T> camera;
    camera.position = {T{0}, T{0}, T{3}};
    auto view = view_from_transform(camera);
    CHECK(view.has_value());
    auto origin = transform_point(Vector3<T>{}, *view.try_value());
    CHECK(origin.has_value() && nearly_equal(*origin.try_value(), Vector3<T>{T{0}, T{0}, T{-3}}, tolerance));
    camera.scale = {T{2}, T{0}, T{-4}};
    auto scaled = view_from_transform(camera);
    CHECK(scaled.has_value() && *scaled.try_value() == *view.try_value());
    camera.position.z = T{-3};
    auto rotation = rotation_axis_angle(Vector3<T>{T{0}, T{1}, T{0}}, k_pi<T>);
    CHECK(rotation.has_value());
    camera.rotation = rotation.take_value();
    auto turned = view_from_transform(camera);
    CHECK(turned.has_value());
    origin = transform_point(Vector3<T>{}, *turned.try_value());
    CHECK(origin.has_value() && nearly_equal(*origin.try_value(), Vector3<T>{T{0}, T{0}, T{-3}}, tolerance));
    camera.rotation = {T{0}, T{0}, T{0}, T{0}};
    CHECK(!view_from_transform(camera).has_value());
    camera.rotation = {};
    camera.position.x = std::numeric_limits<T>::infinity();
    CHECK(!view_from_transform(camera).has_value());

    const T nan = std::numeric_limits<T>::quiet_NaN();
    const T infinity = std::numeric_limits<T>::infinity();
    for (const T invalid : {T{0}, T{-1}, k_pi<T>, nan, infinity})
        CHECK(!perspective_rh_zo(invalid, aspect, nearClip, farClip).has_value());
    for (const T invalid : {T{0}, T{-1}, nan, infinity})
    {
        CHECK(!perspective_rh_zo(fov, invalid, nearClip, farClip).has_value());
        CHECK(!perspective_rh_zo(fov, aspect, invalid, farClip).has_value());
    }
    for (const T invalid : {nearClip, T{0}, T{-1}, nan, infinity})
        CHECK(!perspective_rh_zo(fov, aspect, nearClip, invalid).has_value());
    return true;
}
} // namespace

/// @brief Camera の投影と View を Platform / GPU なしで検証する
int main()
{
    return verify_projection<float>() && verify_projection<double>() ? 0 : 1;
}
