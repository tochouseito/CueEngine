#include <Scene/Scene.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <new>
#include <unordered_set>
#include <utility>

#include <Foundation/Utf8.h>
#include <Foundation/Uuid.h>
#include <Math/Projection.h>
#include <Platform/FileSystem.h>

namespace cue
{
namespace
{
constexpr std::array<std::string_view, 3> k_sceneKeys{"formatVersion", "sceneId", "name"};
constexpr std::array<std::string_view, 10> k_objectKeys{"name",
                                                        "active",
                                                        "transform.position",
                                                        "transform.rotation",
                                                        "transform.scale",
                                                        "camera.projection",
                                                        "camera.fovYRadians",
                                                        "camera.near",
                                                        "camera.far",
                                                        "mesh.assetId"};

/// @brief 一つの Script の必須項目を追跡し、不完全・重複項目を既定値で補完しない
struct ParsedScript final
{
    ScriptDescriptor descriptor;
    unsigned fields = 0;
};

/// @brief v2 の Script Key を所有値へ読む。型の解決は行わず、未知の Script 設定も保持する
Result<void> store_script(std::vector<ParsedScript> &a_scripts, std::string_view a_key, std::string_view a_value)
{
    a_key.remove_prefix(7);
    const auto separator = a_key.find('.');
    if (separator == std::string_view::npos || !is_valid_uuid_text(a_key.substr(0, separator)) || a_value.empty())
        return Result<void>::failure({ErrorCategory::InvalidArgument, "Scene.script_key"});
    const auto id = a_key.substr(0, separator);
    a_key.remove_prefix(separator + 1);
    auto found = std::find_if(a_scripts.begin(), a_scripts.end(),
                              [&](const auto &a_script) { return a_script.descriptor.instanceId == id; });
    if (found == a_scripts.end())
    {
        if (a_scripts.size() >= k_maxScriptsPerObject)
            return Result<void>::failure({ErrorCategory::InvalidArgument, "Scene.script_limit"});
        a_scripts.emplace_back();
        found = a_scripts.end() - 1;
        found->descriptor.instanceId = id;
    }
    auto &script = found->descriptor;
    unsigned field = 0;
    if (a_key == "typeId")
    {
        field = 1;
        script.typeId = a_value;
    }
    else if (a_key == "schemaVersion")
    {
        field = 2;
        const auto parsed = std::from_chars(a_value.data(), a_value.data() + a_value.size(), script.schemaVersion);
        if (parsed.ec != std::errc{} || parsed.ptr != a_value.data() + a_value.size())
            return Result<void>::failure({ErrorCategory::InvalidArgument, "Scene.script_schema"});
    }
    else if (a_key == "enabled")
    {
        field = 4;
        if (a_value != "true" && a_value != "false")
            return Result<void>::failure({ErrorCategory::InvalidArgument, "Scene.script_enabled"});
        script.isEnabled = a_value == "true";
    }
    else if (a_key.starts_with("property."))
    {
        ScriptPropertyValue property;
        property.propertyId = a_key.substr(9);
        if (script.properties.size() >= k_maxScriptProperties ||
            std::any_of(script.properties.begin(), script.properties.end(),
                        [&](const auto &a_property) { return a_property.propertyId == property.propertyId; }))
            return Result<void>::failure({ErrorCategory::InvalidArgument, "Scene.script_property_duplicate_or_limit"});
        if (a_value.starts_with("number:"))
        {
            property.kind = ScriptPropertyKind::Number;
            a_value.remove_prefix(7);
            const auto parsed = std::from_chars(a_value.data(), a_value.data() + a_value.size(), property.number,
                                                std::chars_format::general);
            if (parsed.ec != std::errc{} || parsed.ptr != a_value.data() + a_value.size() ||
                !std::isfinite(property.number))
                return Result<void>::failure({ErrorCategory::InvalidArgument, "Scene.script_number"});
        }
        else if (a_value == "boolean:true" || a_value == "boolean:false")
        {
            property.kind = ScriptPropertyKind::Boolean;
            property.boolean = a_value == "boolean:true";
        }
        else
            return Result<void>::failure({ErrorCategory::InvalidArgument, "Scene.script_property_type"});
        script.properties.push_back(std::move(property));
        return Result<void>::success();
    }
    else
        return Result<void>::failure({ErrorCategory::InvalidArgument, "Scene.script_field"});
    if (found->fields & field)
        return Result<void>::failure({ErrorCategory::InvalidArgument, "Scene.script_duplicate"});
    found->fields |= field;
    return Result<void>::success();
}

/// @brief 区切りの Space / Tab / CR を除く。表示名自身の前後空白は Schema でも拒否する
std::string_view trim(std::string_view a_value) noexcept
{
    const auto first = a_value.find_first_not_of(" \t\r");
    return first == std::string_view::npos ? std::string_view{}
                                           : a_value.substr(first, a_value.find_last_not_of(" \t\r") - first + 1);
}

/// @brief File と所有 Descriptor で同じ表示名の制約を使う
bool valid_name(std::string_view a_name) noexcept
{
    return !a_name.empty() && a_name.size() <= 256 && trim(a_name) == a_name && is_valid_utf8_text(a_name) &&
           std::none_of(a_name.begin(), a_name.end(),
                        [](unsigned char a_value) { return a_value < 0x20 || a_value == 0x7f; });
}

/// @brief 既知 Key の重複・欠損値・未知項目を拒否して値を借用する
template <std::size_t Count>
Result<void> store_value(const std::array<std::string_view, Count> &a_keys,
                         std::array<std::string_view, Count> &a_values, std::string_view a_key,
                         std::string_view a_value)
{
    const auto found = std::find(a_keys.begin(), a_keys.end(), a_key);
    if (found == a_keys.end() || a_value.empty())
        return Result<void>::failure({ErrorCategory::InvalidArgument, "Scene.key:" + std::string(a_key)});
    const auto index = static_cast<std::size_t>(found - a_keys.begin());
    if (!a_values[index].empty())
        return Result<void>::failure({ErrorCategory::InvalidArgument, "Scene.duplicate_key:" + std::string(a_key)});
    a_values[index] = a_value;
    return Result<void>::success();
}

/// @brief 数値を Locale 非依存で完全に読み、NaN / Infinity / 範囲外・余剰成分を拒否する
template <std::size_t Count> Result<std::array<float, Count>> parse_numbers(std::string_view a_text)
{
    std::array<float, Count> values{};
    for (std::size_t index = 0; index < Count; ++index)
    {
        const auto comma = a_text.find(',');
        if ((index + 1 < Count && comma == std::string_view::npos) ||
            (index + 1 == Count && comma != std::string_view::npos))
            return Result<std::array<float, Count>>::failure({ErrorCategory::InvalidArgument, "Scene.numeric_count"});
        const auto part = trim(a_text.substr(0, comma));
        if (part.empty())
            return Result<std::array<float, Count>>::failure({ErrorCategory::InvalidArgument, "Scene.numeric_value"});
        const auto parsed =
            std::from_chars(part.data(), part.data() + part.size(), values[index], std::chars_format::general);
        if (parsed.ec != std::errc{} || parsed.ptr != part.data() + part.size() || !std::isfinite(values[index]))
            return Result<std::array<float, Count>>::failure({ErrorCategory::InvalidArgument, "Scene.numeric_value"});
        if (comma != std::string_view::npos)
            a_text.remove_prefix(comma + 1);
    }
    return Result<std::array<float, Count>>::success(values);
}

/// @brief 一つの Object を完成させる。Transform の一部だけを既定値で補完しない
Result<void> finish_object(SceneDescriptor &a_scene, std::string_view a_id,
                           const std::array<std::string_view, k_objectKeys.size()> &a_values,
                           const std::vector<ParsedScript> &a_scripts)
{
    if (a_id.empty())
        return Result<void>::success();
    if (a_values[0].empty() || (!a_values[1].empty() && a_values[1] != "true" && a_values[1] != "false"))
        return Result<void>::failure({ErrorCategory::InvalidArgument, "Scene.object_fields"});
    SceneObjectDescriptor object{std::string(a_id), std::string(a_values[0]), a_values[1] != "false", {}};
    if (!a_values[2].empty() || !a_values[3].empty() || !a_values[4].empty())
    {
        auto position = parse_numbers<3>(a_values[2]);
        auto rotation = parse_numbers<4>(a_values[3]);
        auto scale = parse_numbers<3>(a_values[4]);
        if (!position.has_value() || !rotation.has_value() || !scale.has_value())
            return Result<void>::failure({ErrorCategory::InvalidArgument, "Scene.transform_fields"});
        const auto &p = *position.try_value();
        const auto &q = *rotation.try_value();
        const auto &s = *scale.try_value();
        object.transform = math::floatTransform{{p[0], p[1], p[2]}, {q[0], q[1], q[2], q[3]}, {s[0], s[1], s[2]}};
    }
    if (!a_values[5].empty() || !a_values[6].empty() || !a_values[7].empty() || !a_values[8].empty())
    {
        // Component の一部だけを既定値で補完せず、未知の投影方式も拒否する
        auto fov = parse_numbers<1>(a_values[6]);
        auto nearClip = parse_numbers<1>(a_values[7]);
        auto farClip = parse_numbers<1>(a_values[8]);
        if (a_values[5] != "perspective" || !fov.has_value() || !nearClip.has_value() || !farClip.has_value())
            return Result<void>::failure({ErrorCategory::InvalidArgument, "Scene.camera_fields"});
        object.camera =
            SceneCameraDescriptor{(*fov.try_value())[0], (*nearClip.try_value())[0], (*farClip.try_value())[0]};
    }
    if (!a_values[9].empty())
        object.mesh = SceneMeshDescriptor{std::string(a_values[9])};
    for (const auto &script : a_scripts)
    {
        if (script.fields != 7)
            return Result<void>::failure({ErrorCategory::InvalidArgument, "Scene.script_required_fields"});
        object.scripts.push_back(script.descriptor);
    }
    if (a_scene.objects.size() >= Scene::k_maxObjects)
        return Result<void>::failure({ErrorCategory::InvalidArgument, "Scene.object_limit"});
    a_scene.objects.push_back(std::move(object));
    return Result<void>::success();
}

/// @brief float の round-trip 桁数で出力する。Scene の検証済み有限値だけを受け取る
void append_number(std::string &a_text, float a_value)
{
    std::array<char, 64> buffer{};
    const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), a_value,
                                         std::chars_format::general, std::numeric_limits<float>::max_digits10);
    a_text.append(buffer.data(), converted.ptr);
}

/// @brief Component の既知成分を comma 区切りで Serialize する
template <typename... T> void append_values(std::string &a_text, const char *a_key, T... a_values)
{
    a_text += a_key;
    const std::array values{a_values...};
    for (std::size_t index = 0; index < values.size(); ++index)
    {
        if (index)
            a_text += ',';
        append_number(a_text, values[index]);
    }
    a_text += '\n';
}
} // namespace

Scene::Scene(SceneDescriptor a_descriptor) : m_descriptor(std::move(a_descriptor))
{
}

Result<Scene> Scene::create(SceneDescriptor a_descriptor)
{
    try
    {
        if ((a_descriptor.formatVersion != 1 && a_descriptor.formatVersion != 2) ||
            !is_valid_uuid_text(a_descriptor.sceneId) || !valid_name(a_descriptor.name) ||
            a_descriptor.objects.size() > k_maxObjects)
            return Result<Scene>::failure({ErrorCategory::InvalidArgument, "Scene.descriptor"});
        std::unordered_set<std::string_view> ids;
        for (const auto &object : a_descriptor.objects)
        {
            if (!is_valid_uuid_text(object.objectId) || !valid_name(object.name) || !ids.insert(object.objectId).second)
                return Result<Scene>::failure({ErrorCategory::InvalidArgument, "Scene.object_identity"});
            if (object.transform)
            {
                auto matrix = object.transform->to_matrix();
                if (!matrix.has_value())
                    return Result<Scene>::failure(*matrix.try_error());
            }
            if ((object.camera || object.mesh) && !object.transform)
                return Result<Scene>::failure({ErrorCategory::InvalidArgument, "Scene.render_transform_required"});
            if (object.camera)
            {
                const auto &camera = *object.camera;
                auto projection =
                    math::perspective_rh_zo(camera.verticalFovRadians, 1.0f, camera.nearClip, camera.farClip);
                if (!projection.has_value())
                    return Result<Scene>::failure(*projection.try_error());
                auto view = math::view_from_transform(*object.transform);
                if (!view.has_value())
                    return Result<Scene>::failure(*view.try_error());
            }
            if (object.mesh && object.mesh->assetId != k_cubeMeshId)
                return Result<Scene>::failure({ErrorCategory::InvalidArgument, "Scene.mesh_asset"});
            if (object.scripts.size() > k_maxScriptsPerObject)
                return Result<Scene>::failure({ErrorCategory::InvalidArgument, "Scene.script_limit"});
            std::unordered_set<std::string_view> scriptIds;
            for (const auto &script : object.scripts)
            {
                auto valid = validate_script(script);
                if (!valid.has_value())
                    return Result<Scene>::failure(*valid.try_error());
                if (!is_valid_uuid_text(script.instanceId) || !scriptIds.insert(script.instanceId).second)
                    return Result<Scene>::failure({ErrorCategory::InvalidArgument, "Scene.script_identity"});
            }
            // 新しい項目を旧 Reader が無視することを避け、Script を持つ形式を明示する
            if (!object.scripts.empty())
                a_descriptor.formatVersion = 2;
        }
        return Result<Scene>::success(Scene(std::move(a_descriptor)));
    }
    catch (const std::bad_alloc &)
    {
        return Result<Scene>::failure({ErrorCategory::PlatformFailure, "Scene.create.allocation"});
    }
}

Result<Scene> Scene::parse(std::string_view a_text)
{
    try
    {
        if (a_text.size() > k_maxFileBytes || !is_valid_utf8_text(a_text))
            return Result<Scene>::failure({ErrorCategory::InvalidArgument, "Scene.text"});
        if (a_text.starts_with("\xef\xbb\xbf"))
            a_text.remove_prefix(3);
        SceneDescriptor descriptor;
        std::array<std::string_view, k_sceneKeys.size()> header{};
        std::array<std::string_view, k_objectKeys.size()> object{};
        std::vector<ParsedScript> scripts;
        std::string_view objectId;
        bool hasScripts = false;
        for (std::size_t offset = 0; offset < a_text.size();)
        {
            const auto newline = a_text.find('\n', offset);
            const auto end = newline == std::string_view::npos ? a_text.size() : newline;
            const auto line = trim(a_text.substr(offset, end - offset));
            offset = end == a_text.size() ? end : end + 1;
            if (line.empty() || line.starts_with('#'))
                continue;
            if (line.starts_with('['))
            {
                if (!line.starts_with("[object ") || !line.ends_with(']'))
                    return Result<Scene>::failure({ErrorCategory::InvalidArgument, "Scene.object_section"});
                auto finished = finish_object(descriptor, objectId, object, scripts);
                if (!finished.has_value())
                    return Result<Scene>::failure(*finished.try_error());
                objectId = trim(line.substr(8, line.size() - 9));
                if (!is_valid_uuid_text(objectId))
                    return Result<Scene>::failure({ErrorCategory::InvalidArgument, "Scene.object_id"});
                object = {};
                scripts.clear();
                continue;
            }
            const auto separator = line.find('=');
            if (separator == std::string_view::npos)
                return Result<Scene>::failure({ErrorCategory::InvalidArgument, "Scene.syntax"});
            const auto key = trim(line.substr(0, separator));
            const auto value = trim(line.substr(separator + 1));
            if (!objectId.empty() && key.starts_with("script."))
            {
                auto stored = store_script(scripts, key, value);
                if (!stored.has_value())
                    return Result<Scene>::failure(*stored.try_error());
                hasScripts = true;
                continue;
            }
            auto stored = objectId.empty() ? store_value(k_sceneKeys, header, key, value)
                                           : store_value(k_objectKeys, object, key, value);
            if (!stored.has_value())
                return Result<Scene>::failure(*stored.try_error());
        }
        auto finished = finish_object(descriptor, objectId, object, scripts);
        if (!finished.has_value())
            return Result<Scene>::failure(*finished.try_error());
        for (std::size_t index = 0; index < header.size(); ++index)
            if (header[index].empty())
                return Result<Scene>::failure(
                    {ErrorCategory::InvalidArgument, "Scene.missing:" + std::string(k_sceneKeys[index])});
        if ((header[0] != "1" && header[0] != "2") || (header[0] == "1" && hasScripts))
            return Result<Scene>::failure({ErrorCategory::InvalidArgument, "Scene.formatVersion"});
        descriptor.formatVersion = header[0] == "1" ? 1 : 2;
        descriptor.sceneId = header[1];
        descriptor.name = header[2];
        return create(std::move(descriptor));
    }
    catch (const std::bad_alloc &)
    {
        return Result<Scene>::failure({ErrorCategory::PlatformFailure, "Scene.parse.allocation"});
    }
}

Result<Scene> Scene::load(IFileSystem &a_files, const Path &a_file)
{
    try
    {
        auto file = a_files.absolute_path(a_file);
        if (!file.has_value())
            return Result<Scene>::failure(*file.try_error());
        if (!file.try_value()->is_absolute() || file.try_value()->extension() != ".cuescene")
            return Result<Scene>::failure({ErrorCategory::InvalidArgument, "Scene.extension"});
        auto bytes = a_files.read_all(*file.try_value(), k_maxFileBytes);
        if (!bytes.has_value())
            return Result<Scene>::failure(*bytes.try_error());
        const auto &data = *bytes.try_value();
        return parse(std::string_view(data.empty() ? "" : reinterpret_cast<const char *>(data.data()), data.size()));
    }
    catch (const std::bad_alloc &)
    {
        return Result<Scene>::failure({ErrorCategory::PlatformFailure, "Scene.load.allocation"});
    }
}

Result<std::string> Scene::serialize() const
{
    try
    {
        std::string text = "formatVersion=" + std::to_string(m_descriptor.formatVersion) +
                           "\nsceneId=" + m_descriptor.sceneId + "\nname=" + m_descriptor.name + "\n";
        for (const auto &object : m_descriptor.objects)
        {
            text += "\n[object " + object.objectId + "]\nname=" + object.name +
                    "\nactive=" + (object.isActive ? "true\n" : "false\n");
            if (object.transform)
            {
                const auto &value = *object.transform;
                append_values(text, "transform.position=", value.position.x, value.position.y, value.position.z);
                append_values(text, "transform.rotation=", value.rotation.x, value.rotation.y, value.rotation.z,
                              value.rotation.w);
                append_values(text, "transform.scale=", value.scale.x, value.scale.y, value.scale.z);
            }
            if (object.camera)
            {
                text += "camera.projection=perspective\n";
                append_values(text, "camera.fovYRadians=", object.camera->verticalFovRadians);
                append_values(text, "camera.near=", object.camera->nearClip);
                append_values(text, "camera.far=", object.camera->farClip);
            }
            if (object.mesh)
                text += "mesh.assetId=" + object.mesh->assetId + '\n';
            for (const auto &script : object.scripts)
            {
                const std::string prefix = "script." + script.instanceId + '.';
                text += prefix + "typeId=" + script.typeId + '\n';
                text += prefix + "schemaVersion=" + std::to_string(script.schemaVersion) + '\n';
                text += prefix + "enabled=" + (script.isEnabled ? "true\n" : "false\n");
                for (const auto &property : script.properties)
                {
                    text += prefix + "property." + property.propertyId + '=';
                    if (property.kind == ScriptPropertyKind::Boolean)
                        text += property.boolean ? "boolean:true\n" : "boolean:false\n";
                    else
                    {
                        // float へ丸めず、公開 Property の double を round-trip 可能な精度で保存する
                        std::array<char, 64> buffer{};
                        const auto converted =
                            std::to_chars(buffer.data(), buffer.data() + buffer.size(), property.number,
                                          std::chars_format::general, std::numeric_limits<double>::max_digits10);
                        text += "number:";
                        text.append(buffer.data(), converted.ptr);
                        text += '\n';
                    }
                }
            }
        }
        if (text.size() > k_maxFileBytes)
            return Result<std::string>::failure({ErrorCategory::InvalidArgument, "Scene.serialize.size"});
        return Result<std::string>::success(std::move(text));
    }
    catch (const std::bad_alloc &)
    {
        return Result<std::string>::failure({ErrorCategory::PlatformFailure, "Scene.serialize.allocation"});
    }
}

const SceneDescriptor &Scene::descriptor() const noexcept
{
    return m_descriptor;
}
} // namespace cue
