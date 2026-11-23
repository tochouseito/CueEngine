#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <imgui.h>

#include <Editor/SceneDocument.h>
#include <EditorUi/EditorUi.h>

namespace cue
{
/// @brief 操作 Button と上下の余白に必要な高さを返す
[[nodiscard]] inline float editor_toolbar_height()
{
    const auto &style = ImGui::GetStyle();
    return (std::max)(style.WindowMinSize.y, ImGui::GetFrameHeight() + style.FramePadding.y * 2);
}

/// @brief WorkPos の次 Frame 確定を待たず、Menu と Toolbar の高さを予約する
[[nodiscard]] inline float editor_panel_top()
{
    const auto *viewport = ImGui::GetMainViewport();
    return viewport->Pos.y + ImGui::GetFrameHeight() + editor_toolbar_height();
}

namespace editor_icon
{
// EditorHost が UI Font に合成する Material Symbols Outlined の Code Point
inline constexpr std::string_view k_save = "\uE161";
inline constexpr std::string_view k_play = "\uE037";
inline constexpr std::string_view k_stop = "\uE047";
inline constexpr std::string_view k_debug = "\uE868";
inline constexpr std::string_view k_folder = "\uE2C7";
inline constexpr std::string_view k_file = "\uE24D";
inline constexpr std::string_view k_script = "\uE86F";
inline constexpr std::string_view k_scene = "\uE413";
inline constexpr std::string_view k_camera = "\uE04B";
inline constexpr std::string_view k_object = "\uE1BD";
inline constexpr std::string_view k_refresh = "\uE5D5";
inline constexpr std::string_view k_log = "\uE8EF";
} // namespace editor_icon

/// @brief Icon と Label を同じ行で表示し、Label の ### ID を維持する
[[nodiscard]] inline std::string editor_icon_label(std::string_view a_icon, std::string_view a_label)
{
    std::string label(a_icon);
    label += ' ';
    label += a_label;
    return label;
}

/// @brief 保存済みの Docking 配置を引き継ぐため、Hierarchy の表示名と従来の Window ID を分ける
[[nodiscard]] inline const char *hierarchy_panel_title(const EditorUiContext &a_context) noexcept
{
    return a_context.text("Hierarchy###Objects##EditorObjects", "ヒエラルキー###Objects##EditorObjects");
}

/// @brief Scene の正本から独立した入力 Buffer。Panel 構築中だけ Main Thread で使う
struct EditorUiState
{
    // Window の位置は ImGui に任せ、閲覧先とファイル選択は Path で保持する
    Path assetFolder;
    Path selectedAsset;
    std::array<char, 256> assetSearch{};
    bool isAssetListVisible = false;
    bool isAssetBrowserVisible = true;
    bool hasInitializedLayout = false;
    bool shouldResetLayout = false;
    bool isLogVisible = true;
    bool shouldFocusLog = false;
    bool hasScriptRemovalPopup = false;
    /// @brief Play 専用の入力 Buffer。Session / 選択の変更で破棄し、Document の編集 Transaction と共有しない
    struct PlayTransformInput
    {
        std::string objectId;
        std::uint64_t session = 0;
        math::floatTransform transform;
        math::floatTransform initial;
        math::float3 rotationDegrees;
        int activeField = -1;
        ImGuiID activeInputId = 0;
        bool hasEdit = false;
    } playTransform;
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
    bool shouldFocusBuildPanel = false;
    bool wasPlaying = false;
    std::array<char, 65> scriptName{};
    bool isScriptCreationRequested = false;
    bool shouldOpenScriptCreationPopup = false;
    bool shouldCloseScriptCreationPopup = false;
    bool needsScriptNameFocus = false;
};

/// @brief Action の確保失敗でも開いた Popup の Stack を戻す
class EditorPopupScope final
{
  public:
    /// @brief BeginPopup 成功後に生成する
    EditorPopupScope() = default;
    /// @brief 対応する Popup を閉じる
    ~EditorPopupScope()
    {
        ImGui::EndPopup();
    }
};

/// @brief Source 作成は Project があり、他の操作や確認が動いていない編集境界だけで要求する
[[nodiscard]] inline bool can_create_editor_script(const EditorUiContext &a_context) noexcept
{
    return a_context.isProjectOpen && a_context.script.canCreate && !a_context.isPlaying && !a_context.isPlayBusy &&
           !a_context.script.isBusy && !a_context.build.isBuilding && !a_context.build.isApplicationRunning &&
           !a_context.isUnsavedConfirmationRequested && !a_context.isBuildConfirmationRequested;
}

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
/// @brief よく使う操作を一行にまとめ、Menu と同じ編集要求を返す
void draw_editor_toolbar(const SceneDocument *a_document, const EditorUiContext &a_context, EditorUiState &a_state,
                         std::vector<EditorAction> &a_actions, bool a_isInputEnabled);
/// @brief Edit と Asset Browser で共有する Script 名入力を開始する
void request_script_creation(EditorUiState &a_state, std::vector<EditorAction> &a_actions);
/// @brief Object 一覧を表示し、選択と行メニューの要求を構築する
void draw_object_list(const SceneDocument &a_document, const EditorUiContext &a_context, EditorUiState &a_state,
                      std::vector<EditorAction> &a_actions);
/// @brief 選択 Object の Transform 入力を所有 Buffer で編集する
[[nodiscard]] Result<void> draw_transform_inspector(const SceneDocument &a_document, const EditorUiContext &a_context,
                                                    EditorUiState &a_state, std::vector<EditorAction> &a_actions);
/// @brief 型情報と保存値から Script の割当・設定を表示し、所有値の操作要求を返す
void draw_script_inspector(const SceneObjectDescriptor &a_object, const EditorUiContext &a_context,
                           std::vector<EditorAction> &a_actions);
/// @brief Host から借用した論理 Texture ID の Image を描画する
void draw_scene_view(const EditorUiContext &a_context, std::vector<EditorAction> &a_actions);
/// @brief 保存状態と回復可能な操作エラーを表示する
void draw_editor_status(const SceneDocument *a_document, const EditorUiContext &a_context, EditorUiState &a_state);
/// @brief Host から借用した通知・エラーを全文表示する
void draw_editor_log(const EditorUiContext &a_context, EditorUiState &a_state);
/// @brief 対応対象の選択と Build 結果を表示し、Host が実行する所有値の要求を返す
void draw_build_panel(const EditorUiContext &a_context, EditorUiState &a_state, std::vector<EditorAction> &a_actions);
/// @brief 独立した Window で Folder とその直下の Asset を表示する
void draw_asset_browser(const EditorUiContext &a_context, EditorUiState &a_state, std::vector<EditorAction> &a_actions);
/// @brief Host の削除予定を表示し、確定または取消を一度返す
void draw_script_removal(const EditorUiContext &a_context, EditorUiState &a_state,
                         std::vector<EditorAction> &a_actions);
/// @brief Menu と Panel で同じ登録一覧を使い、未完了の Target Draft から Build 要求を作らない
[[nodiscard]] bool has_game_build_target(const EditorBuildContext &a_build) noexcept;

/// @brief 初回表示の Panel を現在の Client Area に収める
inline void set_initial_panel(float a_x, float a_y, float a_width, float a_height)
{
    ImGui::SetNextWindowPos({a_x, a_y}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize({a_width, a_height}, ImGuiCond_FirstUseEver);
}
} // namespace cue
