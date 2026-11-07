#include <SceneRuntime/SceneInstantiation.h>

#include <Runtime/Components/CameraComponent.h>
#include <Runtime/Components/MeshRendererComponent.h>
#include <Runtime/Components/TransformComponent.h>
#include <Runtime/World.h>

namespace cue
{
namespace
{
/// @brief 実体化を中断した際、健康な World の作りかけ Instance を回収する
class InstanceRollback final
{
  public:
    /// @brief 呼出中だけ World を借用する
    InstanceRollback(World &a_world, SceneInstanceHandle a_instance) : m_world(a_world), m_instance(a_instance)
    {
    }
    /// @brief 終端失敗の World は外部へ公開できないため、呼出側による World 停止へ委ねる
    ~InstanceRollback()
    {
        if (m_released)
            return;
        try
        {
            auto alive = m_world.is_alive(m_instance);
            if (alive.has_value() && *alive.try_value())
            {
                auto removed = m_world.destroy_scene_instance(m_instance);
                (void)removed;
            }
        }
        catch (...)
        {
        }
    }
    /// @brief 全 Object の生成が完了した Instance を公開する
    void release() noexcept
    {
        m_released = true;
    }

  private:
    World &m_world;
    SceneInstanceHandle m_instance;
    bool m_released = false;
};
} // namespace

Result<SceneInstanceHandle> instantiate_scene(const Scene &a_scene, World &a_world)
{
    try
    {
        const auto &descriptor = a_scene.descriptor();
        // 型容量の検証は Instance を生成する前に行い、非対応を途中で発見しない
        bool needsTransform = false;
        bool needsCamera = false;
        bool needsMesh = false;
        for (const auto &object : descriptor.objects)
        {
            needsTransform = needsTransform || object.transform.has_value();
            needsCamera = needsCamera || object.camera.has_value();
            needsMesh = needsMesh || object.mesh.has_value();
        }
        if ((needsTransform && ecs::ComponentID<TransformComponent>() >= 256) ||
            (needsCamera && ecs::ComponentID<CameraComponent>() >= 256) ||
            (needsMesh && ecs::ComponentID<MeshRendererComponent>() >= 256))
            return Result<SceneInstanceHandle>::failure(
                {ErrorCategory::InvalidArgument, "SceneInstantiation.component_capacity"});
        auto instance = a_world.create_scene_instance(descriptor.sceneId);
        if (!instance.has_value())
            return instance;
        InstanceRollback rollback(a_world, *instance.try_value());
        for (const auto &object : descriptor.objects)
        {
            auto entity = a_world.create_entity(*instance.try_value(), object.objectId, object.name);
            if (!entity.has_value())
                return Result<SceneInstanceHandle>::failure(*entity.try_error());
            if (object.transform)
            {
                auto component = a_world.add_component<TransformComponent>(*entity.try_value());
                if (!component.has_value())
                    return Result<SceneInstanceHandle>::failure(*component.try_error());
                auto assigned = (*component.try_value())->set_local(*object.transform);
                if (!assigned.has_value())
                    return Result<SceneInstanceHandle>::failure(*assigned.try_error());
            }
            if (object.camera)
            {
                auto component = a_world.add_component<CameraComponent>(*entity.try_value());
                if (!component.has_value())
                    return Result<SceneInstanceHandle>::failure(*component.try_error());
                const auto &camera = *object.camera;
                auto assigned = (*component.try_value())
                                    ->set_projection({camera.verticalFovRadians, camera.nearClip, camera.farClip});
                if (!assigned.has_value())
                    return Result<SceneInstanceHandle>::failure(*assigned.try_error());
            }
            if (object.mesh)
            {
                auto component = a_world.add_component<MeshRendererComponent>(*entity.try_value());
                if (!component.has_value())
                    return Result<SceneInstanceHandle>::failure(*component.try_error());
                auto assigned = (*component.try_value())->set_mesh_id(object.mesh->assetId);
                if (!assigned.has_value())
                    return Result<SceneInstanceHandle>::failure(*assigned.try_error());
            }
            auto active = a_world.set_active(*entity.try_value(), object.isActive);
            if (!active.has_value())
                return Result<SceneInstanceHandle>::failure(*active.try_error());
        }
        rollback.release();
        return instance;
    }
    catch (...)
    {
        return Result<SceneInstanceHandle>::failure({ErrorCategory::Fatal, "SceneInstantiation.exception"});
    }
}
} // namespace cue
