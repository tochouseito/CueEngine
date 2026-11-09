#include "EditorUiPanels.h"

#include <algorithm>

#include <imgui_internal.h>

namespace cue
{
namespace
{
/// @brief 選択と Revision が変わった時だけ正本から表示 Buffer を作り直す
void synchronize_input(const SceneDocument &a_document, EditorUiState &a_state, const SceneObjectDescriptor *a_object)
{
    const auto selected = a_document.selected_object_id();
    if (!a_state.needsSync && a_state.selectedId == selected &&
        (a_state.hasEdit || a_state.sourceRevision == a_document.revision()))
        return;
    if (a_state.selectedId != selected)
        a_state.hasInvalidRotationInput = false;
    a_state.selectedId = selected;
    a_state.sourceRevision = a_document.revision();
    a_state.hasTransform = a_object && a_object->transform.has_value();
    a_state.needsSync = false;
    if (!a_state.hasTransform)
        return;
    a_state.transform = *a_object->transform;
    auto euler = math::euler_xyz(a_state.transform.rotation);
    if (euler.has_value())
    {
        const auto &radians = *euler.try_value();
        a_state.rotationDegrees = {math::radians_to_degrees(radians.x), math::radians_to_degrees(radians.y),
                                   math::radians_to_degrees(radians.z)};
    }
}

/// @brief 1 回の Drag / 数値入力を Begin → Preview → Commit の要求へ変換する
Result<void> edit_transform_field(EditorUiState &a_state, std::vector<EditorAction> &a_actions, const char *a_label,
                                  int a_field, math::float3 &a_values, float a_speed, bool a_isCancelling)
{
    float values[] = {a_values.x, a_values.y, a_values.z};
    ImGui::PushID(a_field);
    ImGui::TextUnformatted(a_label);
    ImGui::SetNextItemWidth(-1);
    // 数値入力中の有効値も Draft へ反映し、Enter 前の Close でも未保存状態を検出できるようにする
    ImGui::PushItemFlag(ImGuiItemFlags_LiveEditOnInputScalar, true);
    const bool changed = ImGui::DragFloat3("##Value", values, a_speed, 0, 0, "%.3f");
    ImGui::PopItemFlag();
    const bool activated = ImGui::IsItemActivated();
    const bool deactivated = ImGui::IsItemDeactivated();
    const ImGuiID inputId = ImGui::GetItemID();
    ImGui::PopID();
    // Widget 自身には Escape の復元と Deactivate を完了させ、外部操作は Cancel だけにまとめる
    if (a_isCancelling)
        return Result<void>::success();
    if (activated || (changed && !a_state.hasEdit))
    {
        finish_transform_input(a_state, a_actions);
        emit_editor_action(a_actions, EditorActionKind::BeginTransformEdit, a_state.selectedId);
        a_state.hasEdit = true;
        a_state.activeField = a_field;
        a_state.activeInputId = inputId;
        a_state.needsSync = false;
    }
    if (changed)
    {
        const auto previous = a_values;
        a_values = {values[0], values[1], values[2]};
        bool canPreview = true;
        if (a_field == 1)
        {
            const math::float3 radians = {math::degrees_to_radians(a_values.x), math::degrees_to_radians(a_values.y),
                                          math::degrees_to_radians(a_values.z)};
            auto rotation = math::rotation_euler_xyz(radians);
            a_state.hasInvalidRotationInput = !rotation.has_value();
            if (rotation.has_value())
                a_state.transform.rotation = rotation.take_value();
            else
            {
                // 不正な回転入力は直前の有効な姿勢を保持する。Editor 全体の停止理由にしない
                a_values = previous;
                canPreview = false;
            }
        }
        if (canPreview)
            emit_editor_action(a_actions, EditorActionKind::PreviewTransform, a_state.selectedId, a_state.transform);
    }
    if (a_state.hasEdit && a_state.activeField == a_field && deactivated)
        finish_transform_input(a_state, a_actions);
    return Result<void>::success();
}

/// @brief Play の入力だけを更新し、Authoring Transaction を開始せず確定境界への要求を返す
void edit_play_transform_field(EditorUiState::PlayTransformInput &a_input, std::vector<EditorAction> &a_actions,
                               const char *a_label, int a_field, math::float3 &a_values, float a_speed,
                               bool a_isCancelling)
{
    float values[] = {a_values.x, a_values.y, a_values.z};
    ImGui::PushID(a_field);
    ImGui::TextUnformatted(a_label);
    ImGui::SetNextItemWidth(-1);
    ImGui::PushItemFlag(ImGuiItemFlags_LiveEditOnInputScalar, true);
    const bool changed = ImGui::DragFloat3("##PlayValue", values, a_speed, 0, 0, "%.3f");
    ImGui::PopItemFlag();
    const bool activated = ImGui::IsItemActivated();
    const bool deactivated = ImGui::IsItemDeactivated();
    const ImGuiID inputId = ImGui::GetItemID();
    ImGui::PopID();
    if (a_isCancelling)
        return;
    if (activated || (changed && !a_input.hasEdit))
    {
        a_input.initial = a_input.transform;
        a_input.hasEdit = true;
        a_input.activeField = a_field;
        a_input.activeInputId = inputId;
    }
    if (changed)
    {
        const auto previous = a_input.transform;
        const auto previousValues = a_values;
        a_values = {values[0], values[1], values[2]};
        bool isValid = true;
        if (a_field == 1)
        {
            auto rotation = math::rotation_euler_xyz(math::float3{math::degrees_to_radians(a_values.x),
                                                                  math::degrees_to_radians(a_values.y),
                                                                  math::degrees_to_radians(a_values.z)});
            isValid = rotation.has_value();
            if (isValid)
                a_input.transform.rotation = rotation.take_value();
        }
        if (isValid && a_input.transform.to_matrix().has_value())
            emit_editor_action(a_actions, EditorActionKind::SetPlayTransform, a_input.objectId, a_input.transform);
        else
        {
            a_input.transform = previous;
            a_values = previousValues;
        }
    }
    if (a_input.hasEdit && a_input.activeField == a_field && deactivated)
    {
        a_input.hasEdit = false;
        a_input.activeField = -1;
        a_input.activeInputId = 0;
    }
}

/// @brief 子 World の所有 Snapshot を表示し、入力中だけ値の再同期を抑止する
void draw_play_transform(const SceneObjectDescriptor &a_object, const EditorUiContext &a_context,
                         EditorUiState &a_state, std::vector<EditorAction> &a_actions)
{
    auto &input = a_state.playTransform;
    if (input.session != a_context.playSessionId || input.objectId != a_object.objectId)
    {
        // 旧 Session / 選択の Scalar を新しい Object へ持ち越さない
        if (input.activeInputId && ImGui::GetActiveID() == input.activeInputId)
            ImGui::ClearActiveID();
        input = {};
        input.session = a_context.playSessionId;
        input.objectId = a_object.objectId;
    }
    if (!a_object.transform)
        return;
    if (!input.hasEdit)
    {
        input.transform = *a_object.transform;
        auto euler = math::euler_xyz(input.transform.rotation);
        if (euler.has_value())
        {
            const auto &radians = *euler.try_value();
            input.rotationDegrees = {math::radians_to_degrees(radians.x), math::radians_to_degrees(radians.y),
                                     math::radians_to_degrees(radians.z)};
        }
    }
    const bool cancelInput = !a_context.isPlayBusy && input.hasEdit && input.activeInputId &&
                             ImGui::IsKeyPressed(ImGuiKey_Escape, ImGuiInputFlags_None, input.activeInputId);
    ImGui::PushID("PlayTransform");
    const auto session = std::to_string(a_context.playSessionId);
    ImGui::PushID(session.c_str());
    {
        EditorDisabledScope disabled(a_context.isPlayBusy);
        edit_play_transform_field(input, a_actions, "Position", 0, input.transform.position, 0.01f, cancelInput);
        edit_play_transform_field(input, a_actions, "Rotation (degrees)", 1, input.rotationDegrees, 0.5f, cancelInput);
        edit_play_transform_field(input, a_actions, "Scale", 2, input.transform.scale, 0.01f, cancelInput);
    }
    ImGui::PopID();
    ImGui::PopID();
    if (cancelInput)
    {
        emit_editor_action(a_actions, EditorActionKind::SetPlayTransform, input.objectId, input.initial);
        input.hasEdit = false;
        input.activeField = -1;
        input.activeInputId = 0;
    }
    ImGui::TextDisabled("Play instance only. Changes are not saved.");
    ImGui::TextDisabled("Escape restores the value before this edit.");
}
} // namespace

Result<void> draw_transform_inspector(const SceneDocument &a_document, const EditorUiContext &a_context,
                                      EditorUiState &a_state, std::vector<EditorAction> &a_actions)
{
    if (a_state.wasPlaying != a_context.isPlaying)
    {
        // Editor と Play の入力 Scope を分離し、終了時に編集中の値を保存正本へ反映しない
        const auto inputId = a_state.wasPlaying ? a_state.playTransform.activeInputId : a_state.activeInputId;
        if (inputId && ImGui::GetActiveID() == inputId)
            ImGui::ClearActiveID();
        a_state.playTransform = {};
        a_state.hasEdit = false;
        a_state.activeField = -1;
        a_state.activeInputId = 0;
        a_state.needsSync = true;
        a_state.hasInvalidRotationInput = false;
        a_state.wasPlaying = a_context.isPlaying;
    }
    const SceneObjectDescriptor *object = nullptr;
    const auto *descriptor = a_context.isPlaying ? a_context.playScene : &a_document.scene().descriptor();
    if (descriptor)
    {
        for (const auto &candidate : descriptor->objects)
            if (candidate.objectId == a_document.selected_object_id())
            {
                object = &candidate;
                break;
            }
    }
    if (!a_context.isPlaying)
        synchronize_input(a_document, a_state, object);
    const auto *viewport = ImGui::GetMainViewport();
    const float width = (std::max)(a_context.displayWidth, 1.0f);
    const float height = (std::max)(a_context.displayHeight, 1.0f);
    const float panelWidth = (std::min)(300.0f, width * 0.27f);
    const float top = editor_panel_top();
    set_initial_panel(viewport->WorkPos.x + width - panelWidth, top, panelWidth,
                      (std::max)(height - (top - viewport->Pos.y) - 32, 64.0f));
    PanelScope panel("Inspector##EditorInspector", ImGuiWindowFlags_NoCollapse);
    if (!panel.is_visible())
        return Result<void>::success();
    if (!a_context.isProjectOpen)
    {
        ImGui::TextUnformatted("No project open");
        return Result<void>::success();
    }
    if (!object)
    {
        ImGui::TextUnformatted("Select an object.");
        return Result<void>::success();
    }
    ImGui::TextUnformatted(object->name.c_str());
    ImGui::Separator();
    if (a_context.isPlaying)
    {
        draw_play_transform(*object, a_context, a_state, a_actions);
        draw_script_inspector(*object, a_context, a_actions);
        return Result<void>::success();
    }
    // InputText の所有権を変更せず、実 Scalar と同じ Owner として Escape を観測する
    // 内部 API 依存は入力 Panel に限定する。追従条件は Module の README に記載する
    const bool cancelInput = a_state.hasEdit && a_state.activeInputId != 0 &&
                             ImGui::IsKeyPressed(ImGuiKey_Escape, ImGuiInputFlags_None, a_state.activeInputId);
    if (a_state.hasTransform)
    {
        EditorDisabledScope disabled(a_context.isPlaying);
        auto position =
            edit_transform_field(a_state, a_actions, "Position", 0, a_state.transform.position, 0.01f, cancelInput);
        if (!position.has_value())
            return position;
        auto rotation = edit_transform_field(a_state, a_actions, "Rotation (degrees)", 1, a_state.rotationDegrees, 0.5f,
                                             cancelInput);
        if (!rotation.has_value())
            return rotation;
        if (a_state.hasInvalidRotationInput)
            ImGui::TextWrapped("Enter finite rotation values. The last valid rotation is kept.");
        auto scale = edit_transform_field(a_state, a_actions, "Scale", 2, a_state.transform.scale, 0.01f, cancelInput);
        if (!scale.has_value())
            return scale;
        ImGui::Spacing();
        ImGui::TextDisabled("Drag to edit. Double-click for a value.");
        ImGui::TextDisabled("Escape cancels the current edit.");
    }
    if (object->mesh)
    {
        ImGui::Separator();
        ImGui::TextUnformatted("Mesh: Cube");
    }
    if (object->camera)
    {
        ImGui::Separator();
        ImGui::TextUnformatted("Perspective Camera");
    }
    if (cancelInput)
        finish_transform_input(a_state, a_actions, EditorActionKind::CancelTransformEdit);
    draw_script_inspector(*object, a_context, a_actions);
    return Result<void>::success();
}
} // namespace cue
