#include "EditorUiPanels.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace cue
{
namespace
{
/// @brief 借用中の Script / Property の ID を ImGui の Scope に対応させる
class ScriptIdScope final
{
  public:
    /// @brief 正本の安定した識別子から Widget ID を作る
    explicit ScriptIdScope(const char *a_id)
    {
        ImGui::PushID(a_id);
    }
    /// @brief 例外でも ID の Stack を解放する
    ~ScriptIdScope()
    {
        ImGui::PopID();
    }
};

/// @brief 操作要求の所有文字列の確保失敗でも Combo の Popup を閉じる
class ScriptComboScope final
{
  public:
    /// @brief BeginCombo の成功後だけ構築する
    ScriptComboScope() = default;
    /// @brief Combo と Popup の Stack を対応させる
    ~ScriptComboScope()
    {
        ImGui::EndCombo();
    }
};

/// @brief 設定値を明示登録された Property 名へ解決する。未知の Property も値を保持して表示する
const ScriptPropertyDescriptor *find_property(const ScriptTypeDescriptor *a_type, std::string_view a_id)
{
    if (!a_type)
        return nullptr;
    const auto found = std::find_if(a_type->properties.begin(), a_type->properties.end(),
                                    [&](const auto &a_property) { return a_property.propertyId == a_id; });
    return found == a_type->properties.end() ? nullptr : &*found;
}
} // namespace

void draw_script_inspector(const SceneObjectDescriptor &a_object, const EditorUiContext &a_context,
                           std::vector<EditorAction> &a_actions)
{
    // 子の終了・再起動で未確定の入力 Buffer を編集正本や別 Session へ持ち越さない
    ScriptIdScope modeId(a_context.isPlaying ? "PlayScripts" : "AuthoringScripts");
    const auto sessionId = std::to_string(a_context.isPlaying ? a_context.playSessionId : 0);
    ScriptIdScope sessionScope(sessionId.c_str());
    const SceneObjectDescriptor *object = &a_object;
    if (a_context.isPlaying && a_context.playScene)
    {
        for (const auto &candidate : a_context.playScene->objects)
            if (candidate.objectId == a_object.objectId)
            {
                object = &candidate;
                break;
            }
    }
    ImGui::Separator();
    ImGui::TextUnformatted(a_context.isPlaying ? "Scripts (Play instance)" : "Scripts");
    if (a_context.isPlaying)
        ImGui::TextWrapped("Changes affect this Play session only. Values update after confirmation.");
    for (const auto &script : object->scripts)
    {
        ScriptIdScope scriptId(script.instanceId.c_str());
        const auto found = std::find_if(a_context.scriptTypes.begin(), a_context.scriptTypes.end(),
                                        [&](const auto &a_type) { return a_type.typeId == script.typeId; });
        const auto *type = found == a_context.scriptTypes.end() ? nullptr : &*found;
        ImGui::TextUnformatted(type ? type->displayName.c_str() : script.typeId.c_str());
        {
            EditorDisabledScope disabled(!a_context.script.canEditSource);
            if (ImGui::SmallButton("Edit Source"))
            {
                EditorAction action;
                action.kind = EditorActionKind::OpenScriptSource;
                action.scriptTypeId = script.typeId;
                a_actions.push_back(std::move(action));
            }
        }
        {
            EditorDisabledScope disabled(a_context.isPlaying);
            bool isEnabled = script.isEnabled;
            if (ImGui::Checkbox("Enabled", &isEnabled))
            {
                EditorAction action;
                action.kind = EditorActionKind::SetScriptEnabled;
                action.objectId = object->objectId;
                action.scriptInstanceId = script.instanceId;
                action.isEnabled = isEnabled;
                a_actions.push_back(std::move(action));
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Remove"))
            {
                EditorAction action;
                action.kind = EditorActionKind::RemoveScript;
                action.objectId = object->objectId;
                action.scriptInstanceId = script.instanceId;
                a_actions.push_back(std::move(action));
            }
        }
        if (!type)
            ImGui::TextWrapped("Script type unavailable. Saved settings are preserved.");
        else if (type->schemaVersion != script.schemaVersion)
            ImGui::TextWrapped("Script schema changed. Saved settings are preserved; reassign the script to migrate.");
        // 省略された Property も型の既定値から表示する。編集するまでは保存正本へ追加しない
        auto properties = script.properties;
        if (type)
            for (const auto &metadata : type->properties)
                if (std::none_of(properties.begin(), properties.end(),
                                 [&](const auto &a_value) { return a_value.propertyId == metadata.propertyId; }))
                    properties.push_back(
                        {metadata.propertyId, metadata.kind, metadata.defaultNumber, metadata.defaultBoolean});
        // 未解決型の設定は破壊せず、登録型と一致する Property だけを編集する
        for (const auto &property : properties)
        {
            const auto *metadata = find_property(type, property.propertyId);
            ScriptIdScope propertyId(property.propertyId.c_str());
            EditorDisabledScope disabled(!metadata || metadata->kind != property.kind ||
                                         (type && type->schemaVersion != script.schemaVersion) || a_context.isPlayBusy);
            ImGui::TextUnformatted(metadata ? metadata->displayName.c_str() : property.propertyId.c_str());
            auto value = property;
            bool changed = false;
            ImGui::SetNextItemWidth(-1);
            if (value.kind == ScriptPropertyKind::Number)
                changed =
                    ImGui::InputDouble("##Value", &value.number, 0, 0, "%.3f", ImGuiInputTextFlags_EnterReturnsTrue);
            else if (value.kind == ScriptPropertyKind::Boolean)
                changed = ImGui::Checkbox("##Value", &value.boolean);
            if (changed && (value.kind != ScriptPropertyKind::Number || std::isfinite(value.number)))
            {
                EditorAction action;
                action.kind = EditorActionKind::SetScriptProperty;
                action.objectId = object->objectId;
                action.scriptInstanceId = script.instanceId;
                action.property = std::move(value);
                a_actions.push_back(std::move(action));
            }
        }
        ImGui::Spacing();
    }
    {
        EditorDisabledScope disabled(a_context.isPlaying || a_context.isPlayBusy ||
                                     object->scripts.size() >= k_maxScriptsPerObject);
        if (ImGui::BeginCombo("Add Script", "Select type..."))
        {
            ScriptComboScope combo;
            for (const auto &type : a_context.scriptTypes)
            {
                ScriptIdScope typeId(type.typeId.c_str());
                if (ImGui::Selectable(type.displayName.c_str()))
                {
                    EditorAction action;
                    action.kind = EditorActionKind::AddScript;
                    action.objectId = object->objectId;
                    action.scriptTypeId = type.typeId;
                    a_actions.push_back(std::move(action));
                }
            }
        }
    }
    if (a_context.scriptTypes.empty())
        ImGui::TextWrapped("No script types loaded. Use Script > Reload Script Types.");
    else
        ImGui::TextDisabled("Press Enter to apply a numeric value.");
}
} // namespace cue
