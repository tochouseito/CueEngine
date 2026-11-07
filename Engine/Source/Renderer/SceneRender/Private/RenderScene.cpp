#include <SceneRender/RenderScene.h>

#include <new>
#include <utility>

#include <Math/Projection.h>
#include <Runtime/Components/CameraComponent.h>
#include <Runtime/Components/MeshRendererComponent.h>
#include <Runtime/Components/TransformComponent.h>
#include <Runtime/World.h>

namespace cue
{
Result<SceneRenderSnapshot> extract_render_scene(World &a_world)
{
    try
    {
        SceneRenderSnapshot snapshot;
        std::size_t cameraCount = 0;
        std::optional<Error> validationError;
        bool hasAllocationFailure = false;
        bool hasDirtyTransform = false;
        auto cameras = a_world.for_each<TransformComponent, CameraComponent>(
            [&](EntityHandle, TransformComponent &a_transform, CameraComponent &a_camera) -> Result<void>
            {
                ++cameraCount;
                if (a_transform.is_dirty())
                {
                    hasDirtyTransform = true;
                    return Result<void>::success();
                }
                try
                {
                    auto view = math::view_from_transform(a_transform.local());
                    if (!view.has_value())
                        validationError = *view.try_error();
                    else
                    {
                        const auto &projection = a_camera.projection();
                        snapshot.camera = RenderCameraData{view.take_value(), projection.verticalFovRadians,
                                                           projection.nearClip, projection.farClip};
                    }
                }
                catch (const std::bad_alloc &)
                {
                    hasAllocationFailure = true;
                }
                // 描画入力の不正で World の System 失敗状態へ遷移させない
                return Result<void>::success();
            });
        if (!cameras.has_value())
            return Result<SceneRenderSnapshot>::failure(*cameras.try_error());
        auto meshes = a_world.for_each<TransformComponent, MeshRendererComponent>(
            [&](EntityHandle, TransformComponent &a_transform, MeshRendererComponent &a_mesh) -> Result<void>
            {
                if (a_transform.is_dirty())
                    hasDirtyTransform = true;
                else
                {
                    try
                    {
                        snapshot.objects.push_back({a_transform.world_matrix(), std::string(a_mesh.mesh_id())});
                    }
                    catch (const std::bad_alloc &)
                    {
                        // 読取り Callback の Allocation 失敗を World の System 失敗へ持ち込まない
                        hasAllocationFailure = true;
                    }
                }
                return Result<void>::success();
            });
        if (!meshes.has_value())
            return Result<SceneRenderSnapshot>::failure(*meshes.try_error());
        if (hasAllocationFailure)
            return Result<SceneRenderSnapshot>::failure({ErrorCategory::PlatformFailure, "RenderScene.allocation"});
        if (hasDirtyTransform)
            return Result<SceneRenderSnapshot>::failure(
                {ErrorCategory::InvalidState, "RenderScene.transform_not_synchronized"});
        if (validationError)
            return Result<SceneRenderSnapshot>::failure(std::move(*validationError));
        if ((!snapshot.objects.empty() && cameraCount != 1) || cameraCount > 1)
            return Result<SceneRenderSnapshot>::failure({ErrorCategory::InvalidArgument, "RenderScene.camera_count"});
        return Result<SceneRenderSnapshot>::success(std::move(snapshot));
    }
    catch (const std::bad_alloc &)
    {
        return Result<SceneRenderSnapshot>::failure({ErrorCategory::PlatformFailure, "RenderScene.allocation"});
    }
}
} // namespace cue
