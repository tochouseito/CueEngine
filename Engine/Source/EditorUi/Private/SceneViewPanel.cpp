#include "EditorUiPanels.h"

#include <algorithm>
#include <string>

#include <Math/Quaternion.h>

namespace cue
{
namespace
{
/// @brief Hover 中の Camera 操作だけを所有要求へ変換し、ゲーム側の Camera / 入力は変更しない
void navigate_scene_view(const EditorUiContext &a_context, std::vector<EditorAction> &a_actions)
{
    if (!ImGui::IsItemHovered() || !ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) ||
        ImGui::IsAnyItemActive())
        return;
    const auto &io = ImGui::GetIO();
    auto camera = a_context.sceneViewCamera.value_or(a_context.sceneCameraTransform);
    const bool isRotating = ImGui::IsMouseDown(ImGuiMouseButton_Right);
    bool hasChanged = false;
    if (isRotating && (io.MouseDelta.x != 0 || io.MouseDelta.y != 0))
    {
        auto yaw = math::rotation_axis_angle(math::float3{0, 1, 0}, -io.MouseDelta.x * 0.003f);
        auto pitch = math::rotation_axis_angle(math::float3{1, 0, 0}, -io.MouseDelta.y * 0.003f);
        if (yaw.has_value() && pitch.has_value())
        {
            auto rotation = math::normalize(*yaw.try_value() * camera.rotation * *pitch.try_value());
            if (rotation.has_value())
            {
                camera.rotation = rotation.take_value();
                hasChanged = true;
            }
        }
    }
    math::float3 movement;
    if (isRotating && !io.WantTextInput)
    {
        movement.x =
            static_cast<float>(ImGui::IsKeyDown(ImGuiKey_D)) - static_cast<float>(ImGui::IsKeyDown(ImGuiKey_A));
        movement.y =
            static_cast<float>(ImGui::IsKeyDown(ImGuiKey_E)) - static_cast<float>(ImGui::IsKeyDown(ImGuiKey_Q));
        movement.z =
            static_cast<float>(ImGui::IsKeyDown(ImGuiKey_S)) - static_cast<float>(ImGui::IsKeyDown(ImGuiKey_W));
        const float speed = io.KeyShift ? 8.0f : 3.0f;
        movement = movement * (speed * std::clamp(io.DeltaTime, 0.0f, 0.1f));
    }
    movement.z -= io.MouseWheel * 0.5f;
    if (movement != math::float3{})
    {
        auto rotated = math::rotate(camera.rotation, movement);
        if (rotated.has_value())
        {
            camera.position = camera.position + *rotated.try_value();
            hasChanged = true;
        }
    }
    if (hasChanged && camera.to_matrix().has_value())
        emit_editor_action(a_actions, EditorActionKind::SetSceneViewCamera, {}, camera);
}
} // namespace

void draw_scene_view(const EditorUiContext &a_context, std::vector<EditorAction> &a_actions)
{
    const auto *viewport = ImGui::GetMainViewport();
    const float width = (std::max)(a_context.displayWidth, 1.0f);
    const float height = (std::max)(a_context.displayHeight, 1.0f);
    const float left = (std::min)(240.0f, width * 0.22f);
    const float right = (std::min)(300.0f, width * 0.27f);
    const float top = editor_panel_top();
    set_initial_panel(viewport->WorkPos.x + left, top, (std::max)(width - left - right, 64.0f),
                      (std::max)(height - (top - viewport->Pos.y) - 32, 64.0f));
    PanelScope panel("Scene View##EditorSceneView",
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoScrollbar);
    if (!panel.is_visible())
        return;
    if (!a_context.isProjectOpen)
    {
        ImGui::TextUnformatted("Open a project from File > Open Project...");
        return;
    }
    if (!a_context.isSceneViewEnabled || a_context.sceneTextureId == 0)
    {
        ImGui::TextUnformatted("Scene View unavailable.");
        return;
    }
    {
        EditorDisabledScope disabled(!a_context.sceneViewCamera);
        if (ImGui::SmallButton("Scene camera"))
            emit_editor_action(a_actions, EditorActionKind::ResetSceneViewCamera);
    }
    ImGui::SameLine();
    ImGui::TextDisabled(a_context.sceneViewCamera ? "Editor camera" : "Scene camera");
    if (a_context.isPlaying)
    {
        ImGui::SameLine();
        ImGui::TextUnformatted(a_context.script.isWaitingForDebugger ? "Waiting for debugger"
                               : a_context.isPlayBusy                ? "Play transition..."
                                                                     : "Playing");
    }
    ImGui::TextDisabled("RMB + WASD/QE: move | RMB drag: look | Wheel: dolly");
    const auto available = ImGui::GetContentRegionAvail();
    if (available.x <= 0 || available.y <= 0)
        return;
    const float aspect = width / height;
    ImVec2 size{available.x, available.x / aspect};
    if (size.y > available.y)
        size = {available.y * aspect, available.y};
    const auto cursor = ImGui::GetCursorPos();
    ImGui::SetCursorPos({cursor.x + (available.x - size.x) * 0.5f, cursor.y + (available.y - size.y) * 0.5f});
    ImGui::Image(ImTextureRef(static_cast<ImTextureID>(a_context.sceneTextureId)), size);
    navigate_scene_view(a_context, a_actions);
}

void draw_editor_status(const SceneDocument *a_document, const EditorUiContext &a_context)
{
    const auto *viewport = ImGui::GetMainViewport();
    const float height = ImGui::GetFrameHeight() + 6;
    ImGui::SetNextWindowPos({viewport->Pos.x, viewport->Pos.y + a_context.displayHeight - height});
    ImGui::SetNextWindowSize({(std::max)(a_context.displayWidth, 1.0f), height});
    PanelScope panel("Status##EditorStatus", ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
                                                 ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollWithMouse);
    if (!panel.is_visible())
        return;
    const bool hasError = !a_context.errorText.empty();
    const auto message = hasError ? a_context.errorText : a_context.noticeText;
    if (message.empty())
    {
        ImGui::TextUnformatted(!a_document || !a_context.isProjectOpen ? "No project open"
                               : a_document->is_dirty()              ? "Unsaved changes"
                                                                      : "Ready");
        return;
    }
    // 長いメッセージでもボタンを表示し、クリップボードには省略前の全文を渡す
    if (ImGui::SmallButton("Copy"))
    {
        const std::string text(message);
        ImGui::SetClipboardText(text.c_str());
    }
    ImGui::SameLine();
    if (hasError)
        ImGui::PushStyleColor(ImGuiCol_Text, {1.0f, 0.45f, 0.4f, 1.0f});
    ImGui::TextUnformatted(message.data(), message.data() + message.size());
    if (hasError)
        ImGui::PopStyleColor();
}
} // namespace cue
