#include "EditorUiPanels.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <string>

#include <Math/Quaternion.h>

namespace cue
{
namespace
{
/// @brief 進捗率のない処理を一行の高さに収め、経過時間で円弧を回転させる
void draw_activity_spinner()
{
    constexpr float k_turn = 2.0f * std::numbers::pi_v<float>;
    constexpr double k_period = 0.8;
    const auto position = ImGui::GetCursorScreenPos();
    const float size = ImGui::GetTextLineHeight();
    const ImVec2 center{position.x + size * 0.5f, position.y + size * 0.5f};
    const float angle = static_cast<float>(std::fmod(ImGui::GetTime(), k_period) / k_period) * k_turn;
    ImGui::Dummy({size, size});
    auto *drawList = ImGui::GetWindowDrawList();
    drawList->PathArcTo(center, size * 0.35f, angle, angle + k_turn * 0.7f);
    drawList->PathStroke(ImGui::GetColorU32(ImGuiCol_PlotHistogram), size * 0.12f);
}

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
    PanelScope panel(a_context.text("Scene View##EditorSceneView", "シーンビュー###Scene View##EditorSceneView"),
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoScrollbar);
    if (!panel.is_visible())
        return;
    if (!a_context.isProjectOpen)
    {
        ImGui::TextUnformatted(a_context.text("Open a project from File > Open Project...",
                                              "「ファイル」→「プロジェクトを開く...」から開いてください。"));
        return;
    }
    if (!a_context.isSceneViewEnabled || a_context.sceneTextureId == 0)
    {
        ImGui::TextUnformatted(a_context.text("Scene View unavailable.", "シーンビューを利用できません。"));
        return;
    }
    {
        EditorDisabledScope disabled(!a_context.sceneViewCamera);
        if (ImGui::SmallButton(a_context.text("Scene camera", "シーンカメラ###Scene camera")))
            emit_editor_action(a_actions, EditorActionKind::ResetSceneViewCamera);
    }
    ImGui::SameLine();
    ImGui::TextDisabled(a_context.sceneViewCamera ? a_context.text("Editor camera", "エディターカメラ")
                                                  : a_context.text("Scene camera", "シーンカメラ"));
    if (a_context.isPlaying)
    {
        ImGui::SameLine();
        ImGui::TextUnformatted(a_context.script.isWaitingForDebugger
                                   ? a_context.text("Waiting for debugger", "デバッガーの接続待ち")
                               : a_context.isPlayBusy ? a_context.text("Play transition...", "再生状態を切り替え中...")
                                                      : a_context.text("Playing", "再生中"));
    }
    ImGui::TextDisabled(a_context.text("RMB + WASD/QE: move | RMB drag: look | Wheel: dolly",
                                       "右ボタン＋WASD/QE で移動、右ドラッグで視点変更、ホイールで前後移動"));
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
    const bool isBusy = a_context.build.isBuilding || a_context.script.isBusy;
    auto activity = a_context.build.isBuilding ? a_context.build.statusText : a_context.script.statusText;
    if (isBusy && activity.empty())
        activity = a_context.text("Working...", "処理中...");
    const bool hasError = !a_context.errorText.empty();
    const auto message = hasError ? a_context.errorText : (isBusy ? activity : a_context.noticeText);
    if (message.empty())
    {
        ImGui::TextUnformatted(!a_document || !a_context.isProjectOpen
                                   ? a_context.text("No project open", "プロジェクト未選択")
                               : a_document->is_dirty() ? a_context.text("Unsaved changes", "未保存の変更あり")
                                                        : a_context.text("Ready", "準備完了"));
        return;
    }
    // 長いメッセージでもボタンを表示し、クリップボードには省略前の全文を渡す
    if (ImGui::SmallButton(a_context.text("Copy", "コピー###Copy")))
    {
        const std::string text(message);
        ImGui::SetClipboardText(text.c_str());
    }
    ImGui::SameLine();
    if (isBusy)
    {
        draw_activity_spinner();
        ImGui::SameLine();
        // 作業中のエラーもコピーできるよう、段階名とエラーメッセージを併記する
        if (hasError)
        {
            ImGui::TextUnformatted(activity.data(), activity.data() + activity.size());
            ImGui::SameLine();
        }
    }
    if (hasError)
        ImGui::PushStyleColor(ImGuiCol_Text, {1.0f, 0.45f, 0.4f, 1.0f});
    ImGui::TextUnformatted(message.data(), message.data() + message.size());
    if (hasError)
        ImGui::PopStyleColor();
}
} // namespace cue
