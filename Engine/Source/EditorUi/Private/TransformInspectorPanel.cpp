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
} // namespace

Result<void> draw_transform_inspector(const SceneDocument &a_document, const EditorUiContext &a_context,
                                      EditorUiState &a_state, std::vector<EditorAction> &a_actions)
{
    const SceneObjectDescriptor *object = nullptr;
    for (const auto &candidate : a_document.scene().descriptor().objects)
        if (candidate.objectId == a_document.selected_object_id())
        {
            object = &candidate;
            break;
        }
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
    // InputText の所有権を変更せず、実 Scalar と同じ Owner として Escape を観測する
    // 内部 API 依存はこの一箇所に限定する。追従条件は Module の README に記載する
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
