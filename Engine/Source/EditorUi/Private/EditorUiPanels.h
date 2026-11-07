#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include <imgui.h>

#include <Editor/SceneDocument.h>
#include <EditorUi/EditorUi.h>

namespace cue
{
/// @brief WorkPos の次 Frame 確定を待たず、Menu Bar 分の Client 上端を予約する
[[nodiscard]] inline float editor_panel_top()
{
    const auto *viewport = ImGui::GetMainViewport();
    return viewport->Pos.y + ImGui::GetFrameHeight();
}

/// @brief Scene の正本から独立した入力 Buffer。Panel 構築中だけ Main Thread で使う
struct EditorUiState
{
    std::string selectedId;
    std::uint64_t sourceRevision = 0;
    math::floatTransform transform;
    math::float3 rotationDegrees;
    int activeField = -1;
    ImGuiID activeInputId = 0;
    bool hasTransform = false;
    bool hasEdit = false;
    bool needsSync = true;
    bool hasInvalidRotationInput = false;
    bool isBuildPanelVisible = false;
};

/// @brief 入力抑止の Stack を例外・早期 Return でも対応させる
class EditorDisabledScope final
{
  public:
    /// @brief 現在の Scope に限り入力を抑止する
    explicit EditorDisabledScope(bool a_isDisabled)
    {
        ImGui::BeginDisabled(a_isDisabled);
    }
    /// @brief BeginDisabled に対応する Stack を戻す
    ~EditorDisabledScope()
    {
        ImGui::EndDisabled();
    }
};

/// @brief 例外時にも ImGui Window の Begin / End を対応させる
class PanelScope final
{
  public:
    /// @brief Panel を開始し、折り畳み等による内容描画の要否を保持する
    explicit PanelScope(const char *a_name, ImGuiWindowFlags a_flags = 0)
        : m_isVisible(ImGui::Begin(a_name, nullptr, a_flags))
    {
        // 保存済みの旧 Layout でも Menu / Status を覆わせない。別 Viewport の Panel は動かさない
        if (!(a_flags & ImGuiWindowFlags_NoDocking) && !ImGui::IsWindowDocked() &&
            ImGui::GetWindowViewport() == ImGui::GetMainViewport())
        {
            const auto *viewport = ImGui::GetMainViewport();
            auto position = ImGui::GetWindowPos();
            const float top = editor_panel_top();
            const float bottom = viewport->Pos.y + viewport->Size.y - (ImGui::GetFrameHeight() + 6);
            const float minimumHeight = ImGui::GetStyle().WindowMinSize.y;
            const float boundedY = std::clamp(position.y, top, (std::max)(top, bottom - minimumHeight));
            if (position.y != boundedY)
            {
                position.y = boundedY;
                ImGui::SetWindowPos(position);
            }
            const auto size = ImGui::GetWindowSize();
            const float maximumHeight = (std::max)(bottom - position.y, minimumHeight);
            if (size.y > maximumHeight)
                ImGui::SetWindowSize({size.x, maximumHeight});
        }
    }
    /// @brief Action の Allocation 失敗でも ImGui の Stack を残さない
    ~PanelScope()
    {
        ImGui::End();
    }
    /// @brief 内容を描画できるか返す
    [[nodiscard]] bool is_visible() const noexcept
    {
        return m_isVisible;
    }

  private:
    bool m_isVisible = false;
};

/// @brief 一つの操作要求を末尾へ追加する。ID と Transform は Snapshot の所有値へ移す
void emit_editor_action(std::vector<EditorAction> &a_actions, EditorActionKind a_kind, std::string_view a_objectId = {},
                        const math::floatTransform &a_transform = {});
/// @brief 編集中の入力を一つの操作として閉じ、別操作との順序を固定する
void finish_transform_input(EditorUiState &a_state, std::vector<EditorAction> &a_actions,
                            EditorActionKind a_kind = EditorActionKind::CommitTransformEdit);
/// @brief File / Edit と Keyboard Shortcut の要求を構築する
void draw_scene_menu(const SceneDocument *a_document, EditorUiState &a_state, std::vector<EditorAction> &a_actions,
                     const EditorUiContext &a_context, bool a_isInputEnabled);
/// @brief Object 一覧と選択・Cube 追加の要求を構築する
void draw_object_list(const SceneDocument &a_document, const EditorUiContext &a_context, EditorUiState &a_state,
                      std::vector<EditorAction> &a_actions);
/// @brief 選択 Object の Transform 入力を所有 Buffer で編集する
[[nodiscard]] Result<void> draw_transform_inspector(const SceneDocument &a_document, const EditorUiContext &a_context,
                                                    EditorUiState &a_state, std::vector<EditorAction> &a_actions);
/// @brief 型情報と保存値から Script の割当・設定を表示し、所有値の操作要求を返す
void draw_script_inspector(const SceneObjectDescriptor &a_object, const EditorUiContext &a_context,
                           std::vector<EditorAction> &a_actions);
/// @brief Host から借用した論理 Texture ID の Image を描画する
void draw_scene_view(const EditorUiContext &a_context);
/// @brief 保存状態と回復可能な操作エラーを表示する
void draw_editor_status(const SceneDocument *a_document, const EditorUiContext &a_context);
/// @brief 対応対象の選択と Build 結果を表示し、Host が実行する所有値の要求を返す
void draw_build_panel(const EditorUiContext &a_context, EditorUiState &a_state, std::vector<EditorAction> &a_actions);
/// @brief Menu と Panel で同じ登録一覧を使い、未完了の Target Draft から Build 要求を作らない
[[nodiscard]] bool has_game_build_target(const EditorBuildContext &a_build) noexcept;

/// @brief 初回表示の Panel を現在の Client Area に収める
inline void set_initial_panel(float a_x, float a_y, float a_width, float a_height)
{
    ImGui::SetNextWindowPos({a_x, a_y}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize({a_width, a_height}, ImGuiCond_FirstUseEver);
}
} // namespace cue
