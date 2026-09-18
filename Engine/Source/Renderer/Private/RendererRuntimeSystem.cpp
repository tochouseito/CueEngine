#include <Cue/Renderer/RendererRuntimeSystem.h>

#include <Cue/EngineAssets/BuiltInMesh.h>
#include <Cue/Foundation/Assert.h>
#include <Cue/GameCore/World.h>
#include <Cue/Math/Angle.h>
#include <Cue/Renderer/Error.h>
#include <Cue/Renderer/RendererSchema.h>
#include <Cue/Scene/Instantiation.h>

#include <exception>
#include <memory>
#include <new>
#include <optional>
#include <utility>
#include <vector>

namespace
{
struct CameraComponent final
{
    bool isMain = false;
    float verticalFovRadians = 1.0471975512F;
    float nearPlane = 0.1F;
    float farPlane = 1000.0F;
};

struct MeshComponent final
{
    cue::renderer::RenderMesh mesh = cue::renderer::RenderMesh::Cube;
};

struct RuntimeBindings final
{
    std::optional<cue::game_core::ComponentType<cue::math::Transform>> transform;
    std::optional<cue::game_core::ComponentType<cue::scene::SceneObjectState>> sceneObjectState;
    std::optional<cue::game_core::ComponentType<CameraComponent>> camera;
    std::optional<cue::game_core::ComponentType<MeshComponent>> mesh;
};

/// @brief Camera Authoring DataをRuntime Componentへ変換する
[[nodiscard]] cue::Result<CameraComponent> parse_camera(const cue::scene::KnownComponentData &a_data,
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
    for (const cue::scene::KnownFieldData &field : a_data.known_fields())
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
    CameraComponent result{isMain, cue::math::to_radians(cue::math::Degrees(static_cast<float>(fovDegrees))).value,
                           static_cast<float>(nearPlane), static_cast<float>(farPlane)};
    const cue::renderer::PerspectiveCamera validationCamera{cue::math::Transform{}, result.verticalFovRadians,
                                                            result.nearPlane, result.farPlane};
    if (!hasMain || !hasFov || !hasNear || !hasFar || !cue::renderer::is_valid(validationCamera))
    {
        return cue::Result<CameraComponent>::failure(cue::renderer::make_renderer_error(
            a_assertContext, cue::renderer::RendererError::InvalidCamera,
            "Runtime camera component contains invalid or incomplete projection data"));
    }
    return cue::Result<CameraComponent>::success(std::move(result));
}

/// @brief Mesh Authoring DataをRuntime Componentへ変換する
[[nodiscard]] cue::Result<MeshComponent> parse_mesh(const cue::scene::KnownComponentData &a_data,
                                                    const cue::AssertContext &a_assertContext) noexcept
{
    for (const cue::scene::KnownFieldData &field : a_data.known_fields())
    {
        if (field.id().value() != 1U)
        {
            continue;
        }
        const cue::scene::AssetReferenceValue *asset = field.value().try_asset_reference();
        if (asset != nullptr && asset->token() == cue::engine_assets::k_cubeMeshAssetId)
        {
            return cue::Result<MeshComponent>::success({cue::renderer::RenderMesh::Cube});
        }
    }
    return cue::Result<MeshComponent>::failure(
        cue::renderer::make_renderer_error(a_assertContext, cue::renderer::RendererError::UnsupportedMesh,
                                           "Runtime mesh component does not reference a supported built-in mesh"));
}

class CameraBuilder final : public cue::scene::RuntimeComponentBuilder
{
  public:
    /// @brief Stable TypeとWorld-local Component Tokenを保持する
    CameraBuilder(cue::schema::TypeId a_typeId, cue::game_core::ComponentType<CameraComponent> a_componentType) noexcept
        : m_typeId(std::move(a_typeId)), m_componentType(a_componentType)
    {
    }

    /// @brief Camera Stable Type Identityを返す
    [[nodiscard]] cue::schema::TypeId type_id() const noexcept override
    {
        return m_typeId;
    }

    /// @brief Camera Tokenが指定Worldへ登録済みか返す
    [[nodiscard]] bool is_compatible(const cue::game_core::World &a_world) const noexcept override
    {
        return a_world.is_component_type_registered(m_componentType);
    }

    /// @brief Camera FieldをWorld変更なしで検証する
    [[nodiscard]] cue::Result<void> validate(const cue::scene::KnownComponentData &a_data,
                                             const cue::AssertContext &a_assertContext) const noexcept override
    {
        cue::Result<CameraComponent> parsed = parse_camera(a_data, a_assertContext);
        if (!parsed)
        {
            return cue::Result<void>::failure(std::move(*parsed.try_error()));
        }
        return cue::Result<void>::success();
    }

    /// @brief 検証済みCameraをEntityへ追加する
    [[nodiscard]] cue::Result<void> build(const cue::scene::KnownComponentData &a_data, cue::game_core::World &a_world,
                                          cue::game_core::EntityHandle a_entity,
                                          const cue::AssertContext &a_assertContext) noexcept override
    {
        cue::Result<CameraComponent> parsed = parse_camera(a_data, a_assertContext);
        if (!parsed)
        {
            return cue::Result<void>::failure(std::move(*parsed.try_error()));
        }
        cue::Result<CameraComponent *> added =
            a_world.add_component(m_componentType, a_entity, std::move(*parsed.try_value()));
        if (!added)
        {
            return cue::Result<void>::failure(std::move(*added.try_error()));
        }
        return cue::Result<void>::success();
    }

  private:
    cue::schema::TypeId m_typeId;
    cue::game_core::ComponentType<CameraComponent> m_componentType;
};

class MeshBuilder final : public cue::scene::RuntimeComponentBuilder
{
  public:
    /// @brief Stable TypeとWorld-local Component Tokenを保持する
    MeshBuilder(cue::schema::TypeId a_typeId, cue::game_core::ComponentType<MeshComponent> a_componentType) noexcept
        : m_typeId(std::move(a_typeId)), m_componentType(a_componentType)
    {
    }

    /// @brief Mesh Stable Type Identityを返す
    [[nodiscard]] cue::schema::TypeId type_id() const noexcept override
    {
        return m_typeId;
    }

    /// @brief Mesh Tokenが指定Worldへ登録済みか返す
    [[nodiscard]] bool is_compatible(const cue::game_core::World &a_world) const noexcept override
    {
        return a_world.is_component_type_registered(m_componentType);
    }

    /// @brief Mesh Asset参照をWorld変更なしで検証する
    [[nodiscard]] cue::Result<void> validate(const cue::scene::KnownComponentData &a_data,
                                             const cue::AssertContext &a_assertContext) const noexcept override
    {
        cue::Result<MeshComponent> parsed = parse_mesh(a_data, a_assertContext);
        if (!parsed)
        {
            return cue::Result<void>::failure(std::move(*parsed.try_error()));
        }
        return cue::Result<void>::success();
    }

    /// @brief 検証済みMeshをEntityへ追加する
    [[nodiscard]] cue::Result<void> build(const cue::scene::KnownComponentData &a_data, cue::game_core::World &a_world,
                                          cue::game_core::EntityHandle a_entity,
                                          const cue::AssertContext &a_assertContext) noexcept override
    {
        cue::Result<MeshComponent> parsed = parse_mesh(a_data, a_assertContext);
        if (!parsed)
        {
            return cue::Result<void>::failure(std::move(*parsed.try_error()));
        }
        cue::Result<MeshComponent *> added =
            a_world.add_component(m_componentType, a_entity, std::move(*parsed.try_value()));
        if (!added)
        {
            return cue::Result<void>::failure(std::move(*added.try_error()));
        }
        return cue::Result<void>::success();
    }

  private:
    cue::schema::TypeId m_typeId;
    cue::game_core::ComponentType<MeshComponent> m_componentType;
};

class CameraBuilderFactory final : public cue::scene::RuntimeComponentBuilderFactory
{
  public:
    /// @brief Camera TypeとSession共有Bindingを保持する
    CameraBuilderFactory(cue::schema::TypeId a_typeId, std::shared_ptr<RuntimeBindings> a_bindings) noexcept
        : m_typeId(std::move(a_typeId)), m_bindings(std::move(a_bindings))
    {
    }

    /// @brief Camera Stable Type Identityを返す
    [[nodiscard]] cue::schema::TypeId type_id() const noexcept override
    {
        return m_typeId;
    }

    /// @brief Camera TypeをWorldへ登録してBuilderとSystemのBindingを準備する
    [[nodiscard]] cue::Result<std::unique_ptr<cue::scene::RuntimeComponentBuilder>> create(
        cue::game_core::World &a_world, cue::game_core::ComponentType<cue::math::Transform> a_transformType,
        cue::game_core::ComponentType<cue::scene::SceneObjectState> a_sceneObjectStateType,
        const cue::AssertContext &a_assertContext) const noexcept override
    {
        cue::Result<cue::game_core::ComponentType<CameraComponent>> registered =
            a_world.register_component<CameraComponent>(m_typeId);
        if (!registered)
        {
            return cue::Result<std::unique_ptr<cue::scene::RuntimeComponentBuilder>>::failure(
                std::move(*registered.try_error()));
        }
        m_bindings->transform = a_transformType;
        m_bindings->sceneObjectState = a_sceneObjectStateType;
        m_bindings->camera = *registered.try_value();
        try
        {
            std::unique_ptr<cue::scene::RuntimeComponentBuilder> builder =
                std::make_unique<CameraBuilder>(m_typeId, *registered.try_value());
            return cue::Result<std::unique_ptr<cue::scene::RuntimeComponentBuilder>>::success(std::move(builder));
        }
        catch (const std::bad_alloc &)
        {
            a_assertContext.fatal_handler().terminate("Cue.Renderer camera builder allocation failed");
        }
        catch (...)
        {
            a_assertContext.fatal_handler().terminate(
                "Cue.Renderer camera builder creation caught an unexpected exception");
        }
        std::terminate();
    }

  private:
    cue::schema::TypeId m_typeId;
    std::shared_ptr<RuntimeBindings> m_bindings;
};

class MeshBuilderFactory final : public cue::scene::RuntimeComponentBuilderFactory
{
  public:
    /// @brief Mesh TypeとSession共有Bindingを保持する
    MeshBuilderFactory(cue::schema::TypeId a_typeId, std::shared_ptr<RuntimeBindings> a_bindings) noexcept
        : m_typeId(std::move(a_typeId)), m_bindings(std::move(a_bindings))
    {
    }

    /// @brief Mesh Stable Type Identityを返す
    [[nodiscard]] cue::schema::TypeId type_id() const noexcept override
    {
        return m_typeId;
    }

    /// @brief Mesh TypeをWorldへ登録してBuilderとSystemのBindingを準備する
    [[nodiscard]] cue::Result<std::unique_ptr<cue::scene::RuntimeComponentBuilder>> create(
        cue::game_core::World &a_world, cue::game_core::ComponentType<cue::math::Transform> a_transformType,
        cue::game_core::ComponentType<cue::scene::SceneObjectState> a_sceneObjectStateType,
        const cue::AssertContext &a_assertContext) const noexcept override
    {
        cue::Result<cue::game_core::ComponentType<MeshComponent>> registered =
            a_world.register_component<MeshComponent>(m_typeId);
        if (!registered)
        {
            return cue::Result<std::unique_ptr<cue::scene::RuntimeComponentBuilder>>::failure(
                std::move(*registered.try_error()));
        }
        m_bindings->transform = a_transformType;
        m_bindings->sceneObjectState = a_sceneObjectStateType;
        m_bindings->mesh = *registered.try_value();
        try
        {
            std::unique_ptr<cue::scene::RuntimeComponentBuilder> builder =
                std::make_unique<MeshBuilder>(m_typeId, *registered.try_value());
            return cue::Result<std::unique_ptr<cue::scene::RuntimeComponentBuilder>>::success(std::move(builder));
        }
        catch (const std::bad_alloc &)
        {
            a_assertContext.fatal_handler().terminate("Cue.Renderer mesh builder allocation failed");
        }
        catch (...)
        {
            a_assertContext.fatal_handler().terminate(
                "Cue.Renderer mesh builder creation caught an unexpected exception");
        }
        std::terminate();
    }

  private:
    cue::schema::TypeId m_typeId;
    std::shared_ptr<RuntimeBindings> m_bindings;
};

class RendererRuntimeSystem final : public cue::game_core::RuntimeSystem
{
  public:
    /// @brief Session BindingとSnapshot Storeを非所有接続する
    RendererRuntimeSystem(std::shared_ptr<RuntimeBindings> a_bindings,
                          cue::renderer::RenderSnapshotStore &a_snapshotStore) noexcept
        : m_bindings(std::move(a_bindings)), m_snapshotStore(&a_snapshotStore)
    {
    }

    /// @brief Scene実体化済みWorldから最初のRender Snapshotを生成する
    [[nodiscard]] cue::Result<void> start(cue::game_core::RuntimeSystemContext &a_context) noexcept override
    {
        if (!bindings_ready())
        {
            return cue::Result<void>::failure(cue::renderer::make_renderer_error(
                *m_assertContext, cue::renderer::RendererError::InvalidRuntimeBinding,
                "Renderer runtime component bindings were not prepared before system start"));
        }
        return extract(a_context.world);
    }

    /// @brief Runtime更新後のWorldから最新Render Snapshotを生成する
    [[nodiscard]] cue::Result<void> update(
        const cue::game_core::RuntimeSystemUpdateContext &a_context) noexcept override
    {
        return extract(a_context.world);
    }

    /// @brief Runtime Snapshotを公開停止して再試行不要な停止状態へ移る
    [[nodiscard]] cue::Result<void> stop(cue::game_core::RuntimeSystemContext &) noexcept override
    {
        m_snapshotStore->clear();
        return cue::Result<void>::success();
    }

    /// @brief Error生成に使用するSession外診断Contextを接続する
    void set_assert_context(const cue::AssertContext &a_assertContext) noexcept
    {
        m_assertContext = &a_assertContext;
    }

  private:
    /// @brief 全Core／Renderer Component Tokenが同じSession用に準備済みか返す
    [[nodiscard]] bool bindings_ready() const noexcept
    {
        return m_bindings->transform.has_value() && m_bindings->sceneObjectState.has_value() &&
               m_bindings->camera.has_value() && m_bindings->mesh.has_value();
    }

    /// @brief GameCore WorldをPointer非保持Render Snapshotへ変換する
    [[nodiscard]] cue::Result<void> extract(cue::game_core::World &a_world) noexcept
    {
        try
        {
            std::optional<cue::renderer::PerspectiveCamera> mainCamera;
            std::optional<cue::Error> stateFailure;
            std::size_t mainCameraCount = 0U;
            std::vector<cue::renderer::RenderMeshInstance> meshes;

            /// @brief ActiveなMain Camera候補をWorldから抽出する
            const auto collectCamera = [&](cue::game_core::EntityHandle a_entity,
                                           const cue::math::Transform &a_transform, const CameraComponent &a_camera)
            {
                cue::Result<cue::scene::SceneObjectState *> state =
                    a_world.get_component(*m_bindings->sceneObjectState, a_entity);
                if (!state)
                {
                    if (!stateFailure.has_value())
                    {
                        stateFailure.emplace(std::move(*state.try_error()));
                    }
                    return;
                }
                if (!(*state.try_value())->isEffectiveActive || !a_camera.isMain)
                {
                    return;
                }
                ++mainCameraCount;
                if (mainCameraCount == 1U)
                {
                    mainCamera.emplace(cue::renderer::PerspectiveCamera{a_transform, a_camera.verticalFovRadians,
                                                                        a_camera.nearPlane, a_camera.farPlane});
                }
            };
            cue::Result<std::size_t> cameraCount =
                a_world.query_read(*m_bindings->transform, *m_bindings->camera, collectCamera);
            if (!cameraCount)
            {
                return cue::Result<void>::failure(std::move(*cameraCount.try_error()));
            }
            if (stateFailure.has_value())
            {
                return cue::Result<void>::failure(std::move(*stateFailure));
            }

            /// @brief ActiveなMesh InstanceをWorldから抽出する
            const auto collectMesh = [&](cue::game_core::EntityHandle a_entity, const cue::math::Transform &a_transform,
                                         const MeshComponent &a_mesh)
            {
                cue::Result<cue::scene::SceneObjectState *> state =
                    a_world.get_component(*m_bindings->sceneObjectState, a_entity);
                if (!state)
                {
                    if (!stateFailure.has_value())
                    {
                        stateFailure.emplace(std::move(*state.try_error()));
                    }
                    return;
                }
                if ((*state.try_value())->isEffectiveActive)
                {
                    meshes.push_back({a_transform, a_mesh.mesh});
                }
            };
            cue::Result<std::size_t> meshCount =
                a_world.query_read(*m_bindings->transform, *m_bindings->mesh, collectMesh);
            if (!meshCount)
            {
                return cue::Result<void>::failure(std::move(*meshCount.try_error()));
            }
            if (stateFailure.has_value())
            {
                return cue::Result<void>::failure(std::move(*stateFailure));
            }

            cue::renderer::MainCameraStatus status = cue::renderer::MainCameraStatus::Ready;
            if (mainCameraCount == 0U)
            {
                status = cue::renderer::MainCameraStatus::Missing;
            }
            else if (mainCameraCount > 1U)
            {
                status = cue::renderer::MainCameraStatus::Multiple;
                mainCamera.reset();
            }
            ++m_generation;
            m_snapshotStore->publish(
                cue::renderer::RenderSnapshot(status, std::move(mainCamera), std::move(meshes), m_generation));
            return cue::Result<void>::success();
        }
        catch (const std::bad_alloc &)
        {
            m_assertContext->fatal_handler().terminate("Cue.Renderer runtime snapshot allocation failed");
        }
        catch (...)
        {
            m_assertContext->fatal_handler().terminate(
                "Cue.Renderer runtime snapshot extraction caught an unexpected exception");
        }
        std::terminate();
    }

    std::shared_ptr<RuntimeBindings> m_bindings;
    cue::renderer::RenderSnapshotStore *m_snapshotStore;
    const cue::AssertContext *m_assertContext = nullptr;
    std::uint64_t m_generation = 0U;
};
} // namespace

namespace cue::renderer
{
RendererRuntimeSystemFactory::RendererRuntimeSystemFactory(RenderSnapshotStore &a_snapshotStore) noexcept
    : m_snapshotStore(&a_snapshotStore)
{
}

Result<runtime::RuntimeSystemRegistration> RendererRuntimeSystemFactory::create_system(
    const AssertContext &a_assertContext) const noexcept
{
    Result<RendererSchemaTypeIds> typeIds = make_renderer_schema_type_ids(a_assertContext);
    if (!typeIds)
    {
        return Result<runtime::RuntimeSystemRegistration>::failure(std::move(*typeIds.try_error()));
    }

    try
    {
        std::shared_ptr<RuntimeBindings> bindings = std::make_shared<RuntimeBindings>();
        auto system = std::make_unique<RendererRuntimeSystem>(bindings, *m_snapshotStore);
        system->set_assert_context(a_assertContext);

        runtime::RuntimeSystemRegistration registration;
        registration.descriptor = {"Cue.Renderer.Extraction", game_core::RuntimeUpdatePhase::PostUpdate, 1000, {}};
        registration.system = std::move(system);
        registration.componentBuilderFactories.reserve(2U);
        registration.componentBuilderFactories.push_back(
            std::make_unique<CameraBuilderFactory>(typeIds.try_value()->camera, bindings));
        registration.componentBuilderFactories.push_back(
            std::make_unique<MeshBuilderFactory>(typeIds.try_value()->mesh, std::move(bindings)));
        return Result<runtime::RuntimeSystemRegistration>::success(std::move(registration));
    }
    catch (const std::bad_alloc &)
    {
        a_assertContext.fatal_handler().terminate("Cue.Renderer runtime system allocation failed");
    }
    catch (...)
    {
        a_assertContext.fatal_handler().terminate(
            "Cue.Renderer runtime system creation caught an unexpected exception");
    }
    std::terminate();
}
} // namespace cue::renderer
