#include <Script/GameObject.h>

#include <algorithm>
#include <cstring>
#include <limits>

namespace cue
{
namespace
{
/// @brief 呼出中の文字列だけを借用する。ABI の長さに収まらない入力は呼出前に拒否する
CueScriptString borrow(std::string_view a_text) noexcept
{
    return {a_text.data(), static_cast<std::uint32_t>(a_text.size())};
}
} // namespace

Result<void> script_result(CueScriptStatus a_status)
{
    if (a_status.code == 0)
        return Result<void>::success();
    const auto end = std::find(std::begin(a_status.message), std::end(a_status.message), '\0');
    const auto category = a_status.code == 1   ? ErrorCategory::InvalidArgument
                          : a_status.code == 2 ? ErrorCategory::PlatformFailure
                                               : ErrorCategory::Fatal;
    return Result<void>::failure({category, std::string(a_status.message, end)});
}

CueScriptStatus script_status(const Error &a_error) noexcept
{
    CueScriptStatus result{};
    result.code = a_error.category == ErrorCategory::Fatal             ? 3
                  : a_error.category == ErrorCategory::PlatformFailure ? 2
                                                                       : 1;
    const auto size = (std::min)(a_error.operation.size(), sizeof(result.message) - 1);
    std::memcpy(result.message, a_error.operation.data(), size);
    return result;
}

CueScriptStatus script_exception_status() noexcept
{
    CueScriptStatus status{};
    status.code = 3;
    constexpr char message[] = "Script.unhandled_exception";
    std::memcpy(status.message, message, sizeof(message));
    return status;
}

GameObject::GameObject(const CueScriptEngineApi &a_api, CueScriptEntity a_entity) noexcept
    : GameObject(a_api, a_entity, nullptr)
{
}

GameObject::GameObject(const CueScriptEngineApi &a_api, CueScriptEntity a_entity, const ScriptAccess *a_access) noexcept
    : m_api(a_api), m_entity(a_entity), m_access(a_access)
{
}

CueScriptEntity GameObject::handle() const noexcept
{
    return m_entity;
}

Result<bool> GameObject::is_alive() const
{
    if (!m_api.is_alive)
        return Result<bool>::failure({ErrorCategory::InvalidState, "GameObject.is_alive.api"});
    std::uint32_t alive = 0;
    auto result = script_result(m_api.is_alive(m_api.context, m_entity, &alive));
    if (!result.has_value())
        return Result<bool>::failure(*result.try_error());
    if (alive > 1)
        return Result<bool>::failure({ErrorCategory::InvalidArgument, "GameObject.is_alive.value"});
    return Result<bool>::success(alive != 0);
}

Result<math::floatTransform> GameObject::transform() const
{
    if (!m_api.get_transform)
        return Result<math::floatTransform>::failure({ErrorCategory::InvalidState, "GameObject.transform.api"});
    CueScriptTransform value{};
    auto result = script_result(m_api.get_transform(m_api.context, m_entity, &value));
    if (!result.has_value())
        return Result<math::floatTransform>::failure(*result.try_error());
    math::floatTransform transform{{value.position[0], value.position[1], value.position[2]},
                                   {value.rotation[0], value.rotation[1], value.rotation[2], value.rotation[3]},
                                   {value.scale[0], value.scale[1], value.scale[2]}};
    auto matrix = transform.to_matrix();
    if (!matrix.has_value())
        return Result<math::floatTransform>::failure(*matrix.try_error());
    return Result<math::floatTransform>::success(transform);
}

Result<void> GameObject::set_transform(const math::floatTransform &a_transform) const
{
    if (!m_api.set_transform)
        return Result<void>::failure({ErrorCategory::InvalidState, "GameObject.set_transform.api"});
    auto matrix = a_transform.to_matrix();
    if (!matrix.has_value())
        return Result<void>::failure(*matrix.try_error());
    const CueScriptTransform value{
        {a_transform.position.x, a_transform.position.y, a_transform.position.z},
        {a_transform.rotation.x, a_transform.rotation.y, a_transform.rotation.z, a_transform.rotation.w},
        {a_transform.scale.x, a_transform.scale.y, a_transform.scale.z}};
    return script_result(m_api.set_transform(m_api.context, m_entity, &value));
}

Result<ScriptCamera> GameObject::camera() const
{
    if (!m_api.get_camera)
    {
        return Result<ScriptCamera>::failure({ErrorCategory::InvalidState, "GameObject.camera.api"});
    }
    CueScriptCamera value{};
    auto result = script_result(m_api.get_camera(m_api.context, m_entity, &value));
    if (!result.has_value())
    {
        return Result<ScriptCamera>::failure(*result.try_error());
    }
    return Result<ScriptCamera>::success({value.verticalFovRadians, value.nearClip, value.farClip});
}

Result<ScriptMeshRenderer> GameObject::mesh_renderer() const
{
    if (!m_api.get_mesh_renderer)
    {
        return Result<ScriptMeshRenderer>::failure({ErrorCategory::InvalidState, "GameObject.mesh_renderer.api"});
    }
    CueScriptMeshRenderer value{};
    auto result = script_result(m_api.get_mesh_renderer(m_api.context, m_entity, &value));
    if (!result.has_value())
    {
        return Result<ScriptMeshRenderer>::failure(*result.try_error());
    }
    if (!value.assetIdSize || value.assetIdSize > sizeof(value.assetId))
    {
        return Result<ScriptMeshRenderer>::failure({ErrorCategory::InvalidArgument, "GameObject.mesh_renderer.asset"});
    }
    return Result<ScriptMeshRenderer>::success({std::string(value.assetId, value.assetIdSize)});
}

Result<Behaviour *> GameObject::find_script(const std::type_info &a_type, std::string_view a_instanceId) const
{
    constexpr std::size_t k_maxCallDepth = 32;
    if (!m_access || !m_api.find_script || *m_access->callDepth >= k_maxCallDepth)
    {
        return Result<Behaviour *>::failure({ErrorCategory::InvalidState, "GameObject.with_script.context_or_depth"});
    }
    return m_access->find(m_access->context, *this, a_type, a_instanceId);
}

Result<GameObject> GameObject::find_by_name(std::string_view a_name) const
{
    if (!m_api.get_scene_instance)
    {
        return Result<GameObject>::failure({ErrorCategory::InvalidState, "GameObject.find_by_name.scene_api"});
    }
    CueScriptSceneInstance scene{};
    auto result = script_result(m_api.get_scene_instance(m_api.context, m_entity, &scene));
    if (!result.has_value())
    {
        return Result<GameObject>::failure(*result.try_error());
    }
    return find_by_name(scene, a_name);
}

Result<GameObject> GameObject::find_by_name(CueScriptSceneInstance a_instance, std::string_view a_name) const
{
    if (!m_api.find_by_name || a_name.empty() || a_name.size() > (std::numeric_limits<std::uint32_t>::max)())
        return Result<GameObject>::failure({ErrorCategory::InvalidArgument, "GameObject.find_by_name.arguments"});
    CueScriptEntity found{};
    auto result = script_result(m_api.find_by_name(m_api.context, a_instance, borrow(a_name), &found));
    if (!result.has_value())
        return Result<GameObject>::failure(*result.try_error());
    return Result<GameObject>::success(GameObject(m_api, found, m_access));
}

Result<void> GameObject::report(std::uint32_t a_severity, std::string_view a_text) const
{
    if (!m_api.report || a_text.size() > (std::numeric_limits<std::uint32_t>::max)())
        return Result<void>::failure({ErrorCategory::InvalidArgument, "GameObject.report.arguments"});
    return script_result(m_api.report(m_api.context, a_severity, borrow(a_text)));
}
} // namespace cue
