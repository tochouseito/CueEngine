#include <Cue/Renderer/RenderExtraction.h>

#include <Cue/EngineAssets/BuiltInMesh.h>
#include <Cue/Foundation/Assert.h>
#include <Cue/Math/Angle.h>
#include <Cue/Renderer/Error.h>
#include <Cue/Scene/SceneDocument.h>

#include <exception>
#include <map>
#include <new>
#include <utility>

namespace
{
/// @brief Object自身とAncestorのActive状態をAuthoring Sceneから計算する
[[nodiscard]] bool is_effectively_active(
    const cue::scene::SceneObject &a_object,
    const std::map<cue::scene::ObjectId, const cue::scene::SceneObject *> &a_objects) noexcept
{
    bool isActive = a_object.is_active();
    const cue::scene::ObjectId *parentId = a_object.try_parent_id();
    while (parentId != nullptr)
    {
        const auto parent = a_objects.find(*parentId);
        if (parent == a_objects.end())
        {
            return false;
        }
        isActive = isActive && parent->second->is_active();
        parentId = parent->second->try_parent_id();
    }
    return isActive;
}

/// @brief Camera Authoring FieldをPortable Camera値へ変換する
[[nodiscard]] cue::Result<std::pair<bool, cue::renderer::PerspectiveCamera>> read_camera(
    const cue::scene::KnownComponentData &a_component, const cue::math::Transform &a_transform,
    const cue::AssertContext &a_assertContext) noexcept
{
    bool isMain = false;
    double fovDegrees = 60.0;
    double nearPlane = 0.1;
    double farPlane = 1000.0;
    bool hasMain = false;
    bool hasFov = false;
    bool hasNear = false;
    bool hasFar = false;
    for (const cue::scene::KnownFieldData &field : a_component.known_fields())
    {
        switch (field.id().value())
        {
        case 1U:
            if (const bool *value = field.value().try_boolean())
            {
                isMain = *value;
                hasMain = true;
            }
            break;
        case 2U:
            if (const double *value = field.value().try_floating_point())
            {
                fovDegrees = *value;
                hasFov = true;
            }
            break;
        case 3U:
            if (const double *value = field.value().try_floating_point())
            {
                nearPlane = *value;
                hasNear = true;
            }
            break;
        case 4U:
            if (const double *value = field.value().try_floating_point())
            {
                farPlane = *value;
                hasFar = true;
            }
            break;
        default:
            break;
        }
    }
    if (!hasMain || !hasFov || !hasNear || !hasFar)
    {
        return cue::Result<std::pair<bool, cue::renderer::PerspectiveCamera>>::failure(
            cue::renderer::make_renderer_error(a_assertContext, cue::renderer::RendererError::InvalidCamera,
                                               "Camera component is missing a required field"));
    }
    cue::renderer::PerspectiveCamera camera{
        a_transform, cue::math::to_radians(cue::math::Degrees(static_cast<float>(fovDegrees))).value,
        static_cast<float>(nearPlane), static_cast<float>(farPlane)};
    if (!cue::renderer::is_valid(camera))
    {
        return cue::Result<std::pair<bool, cue::renderer::PerspectiveCamera>>::failure(
            cue::renderer::make_renderer_error(a_assertContext, cue::renderer::RendererError::InvalidCamera,
                                               "Camera projection values are outside the supported perspective range"));
    }
    return cue::Result<std::pair<bool, cue::renderer::PerspectiveCamera>>::success({isMain, std::move(camera)});
}

/// @brief Mesh Authoring FieldをM22のBuilt-in Meshへ解決する
[[nodiscard]] cue::Result<cue::renderer::RenderMesh> read_mesh(const cue::scene::KnownComponentData &a_component,
                                                               const cue::AssertContext &a_assertContext) noexcept
{
    for (const cue::scene::KnownFieldData &field : a_component.known_fields())
    {
        if (field.id().value() != 1U)
        {
            continue;
        }
        const cue::scene::AssetReferenceValue *asset = field.value().try_asset_reference();
        if (asset != nullptr && asset->token() == cue::engine_assets::k_cubeMeshAssetId)
        {
            return cue::Result<cue::renderer::RenderMesh>::success(cue::renderer::RenderMesh::Cube);
        }
    }
    return cue::Result<cue::renderer::RenderMesh>::failure(
        cue::renderer::make_renderer_error(a_assertContext, cue::renderer::RendererError::UnsupportedMesh,
                                           "Mesh component does not reference a supported built-in mesh"));
}
} // namespace

namespace cue::renderer
{
Result<RenderSnapshot> extract_render_snapshot(const scene::SceneDocument &a_document,
                                               const RendererSchemaTypeIds &a_typeIds, std::uint64_t a_generation,
                                               const AssertContext &a_assertContext) noexcept
{
    try
    {
        std::map<scene::ObjectId, const scene::SceneObject *> objects;
        for (const scene::SceneObject &object : a_document.objects())
        {
            objects.emplace(object.id(), &object);
        }

        std::optional<PerspectiveCamera> mainCamera;
        std::size_t mainCameraCount = 0U;
        std::vector<RenderMeshInstance> meshes;
        meshes.reserve(a_document.object_count());

        for (const scene::SceneObject &object : a_document.objects())
        {
            if (!is_effectively_active(object, objects))
            {
                continue;
            }
            for (const scene::SceneComponent &component : object.components())
            {
                const scene::KnownComponentData *known = component.try_known();
                if (known == nullptr)
                {
                    continue;
                }
                if (known->type_id() == a_typeIds.camera)
                {
                    Result<std::pair<bool, PerspectiveCamera>> camera =
                        read_camera(*known, object.transform(), a_assertContext);
                    if (!camera)
                    {
                        return Result<RenderSnapshot>::failure(std::move(*camera.try_error()));
                    }
                    if (camera.try_value()->first)
                    {
                        ++mainCameraCount;
                        if (mainCameraCount == 1U)
                        {
                            mainCamera.emplace(std::move(camera.try_value()->second));
                        }
                    }
                }
                else if (known->type_id() == a_typeIds.mesh)
                {
                    Result<RenderMesh> mesh = read_mesh(*known, a_assertContext);
                    if (!mesh)
                    {
                        return Result<RenderSnapshot>::failure(std::move(*mesh.try_error()));
                    }
                    meshes.push_back({object.transform(), *mesh.try_value()});
                }
            }
        }

        MainCameraStatus status = MainCameraStatus::Ready;
        if (mainCameraCount == 0U)
        {
            status = MainCameraStatus::Missing;
            mainCamera.reset();
        }
        else if (mainCameraCount > 1U)
        {
            status = MainCameraStatus::Multiple;
            mainCamera.reset();
        }
        return Result<RenderSnapshot>::success(
            RenderSnapshot(status, std::move(mainCamera), std::move(meshes), a_generation));
    }
    catch (const std::bad_alloc &)
    {
        a_assertContext.fatal_handler().terminate("Cue.Renderer authoring snapshot allocation failed");
    }
    catch (...)
    {
        a_assertContext.fatal_handler().terminate(
            "Cue.Renderer authoring snapshot extraction caught an unexpected exception");
    }
    std::terminate();
}
} // namespace cue::renderer
