#include <EditorUi/EditorUi.h>

#include <algorithm>
#include <new>
#include <optional>
#include <utility>

#include <imgui.h>
#include <imgui_internal.h>

#include <Editor/SceneDocument.h>

#include "EditorUiPanels.h"

namespace cue
{
namespace
{
/// @brief 標準 Panel の配置を一つの DockSpace に作り、非表示の Panel にも初回表示位置を割り当てる
void build_editor_layout(ImGuiID a_dockId, ImVec2 a_size)
{
    ImGui::DockBuilderRemoveNode(a_dockId);
    ImGui::DockBuilderAddNode(a_dockId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(a_dockId, a_size);

    auto top = a_dockId;
    const auto bottom = ImGui::DockBuilderSplitNode(top, ImGuiDir_Down, 0.30f, nullptr, &top);
    const auto hierarchy = ImGui::DockBuilderSplitNode(top, ImGuiDir_Left, 0.20f, nullptr, &top);
    const auto inspector = ImGui::DockBuilderSplitNode(top, ImGuiDir_Right, 0.30f, nullptr, &top);
    ImGuiID assets = 0;
    const auto folders = ImGui::DockBuilderSplitNode(bottom, ImGuiDir_Left, 0.24f, nullptr, &assets);
    ImGui::DockBuilderDockWindow("Hierarchy###Objects##EditorObjects", hierarchy);
    ImGui::DockBuilderDockWindow("Scene View##EditorSceneView", top);
    ImGui::DockBuilderDockWindow("Inspector##EditorInspector", inspector);
    ImGui::DockBuilderDockWindow("Project Folders###EditorProjectFolders", folders);
    ImGui::DockBuilderDockWindow("Assets###EditorAssets", assets);
    ImGui::DockBuilderDockWindow("Log###EditorLog", assets);
    ImGui::DockBuilderDockWindow("Build##EditorBuild", assets);
    ImGui::DockBuilderFinish(a_dockId);
    // 初期配置では作成順に関係なくファイル一覧を最初に表示する
    if (auto *node = ImGui::DockBuilderGetNode(assets))
        node->SelectedTabId = ImHashStr("#TAB", 0, ImHashStr("Assets###EditorAssets"));
}

/// @brief Toolbar と Status の間を背景付き Docking 先にする
void draw_editor_dockspace(const EditorUiContext &a_context, EditorUiState &a_state)
{
    if (!(ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_DockingEnable))
        return;
    const auto *viewport = ImGui::GetMainViewport();
    const float top = editor_panel_top();
    const float statusHeight = ImGui::GetFrameHeight() + 6;
    ImGui::SetNextWindowPos({viewport->Pos.x, top});
    ImGui::SetNextWindowSize({(std::max)(a_context.displayWidth, 1.0f),
                              (std::max)(a_context.displayHeight - (top - viewport->Pos.y) - statusHeight, 1.0f)});
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
    PanelScope background("EditorDockSpace", ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
                                                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                                                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
                                                 ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar(3);
    if (!background.is_visible())
        return;
    const auto dockId = ImGui::GetID("EditorDockArea");
    if (a_state.shouldResetLayout ||
        (!a_state.hasInitializedLayout && a_context.displayWidth >= 640 && a_context.displayHeight >= 400))
    {
        const auto *node = ImGui::DockBuilderGetNode(dockId);
        // 過去の Dock やフローティング Panel の配置は、明示的な初期化まで引き継ぐ
        const bool hasSavedLayout = (node && (node->IsSplitNode() || !node->Windows.empty())) ||
                                    ImGui::FindWindowSettingsByID(ImHashStr("Scene View##EditorSceneView")) ||
                                    ImGui::FindWindowSettingsByID(ImHashStr("Assets###EditorAssets"));
        if (a_state.shouldResetLayout || !hasSavedLayout)
            build_editor_layout(dockId, ImGui::GetContentRegionAvail());
        else if (!ImGui::FindWindowSettingsByID(ImHashStr("Log###EditorLog")))
        {
            // 旧 Layout の選択中 Tab を維持し、Log を開いたときは Assets と同じ Dock を使う
            a_state.isLogVisible = false;
            const auto *assets = ImGui::FindWindowSettingsByID(ImHashStr("Assets###EditorAssets"));
            if (assets && assets->DockId && ImGui::DockBuilderGetNode(assets->DockId))
                ImGui::DockBuilderDockWindow("Log###EditorLog", assets->DockId);
        }
        a_state.hasInitializedLayout = true;
        a_state.shouldResetLayout = false;
    }
    ImGui::DockSpace(dockId, {0, 0}, ImGuiDockNodeFlags_None);
}
/// @brief Document がない起動状態を表示し、内部用の Scene や World を生成しない
void draw_empty_project_panels(const EditorUiContext &a_context)
{
    const auto *viewport = ImGui::GetMainViewport();
    const float width = (std::max)(a_context.displayWidth, 1.0f);
    const float height = (std::max)(a_context.displayHeight, 1.0f);
    const float top = editor_panel_top();
    const float panelHeight = (std::max)(height - (top - viewport->Pos.y) - 32, 64.0f);
    const float leftWidth = (std::min)(240.0f, width * 0.22f);
    const float rightWidth = (std::min)(300.0f, width * 0.27f);
    set_initial_panel(viewport->WorkPos.x, top, leftWidth, panelHeight);
    {
        PanelScope panel(hierarchy_panel_title(a_context), ImGuiWindowFlags_NoCollapse);
        if (panel.is_visible())
            ImGui::TextUnformatted(a_context.text("No project open", "プロジェクト未選択"));
    }
    set_initial_panel(viewport->WorkPos.x + width - rightWidth, top, rightWidth, panelHeight);
    {
        PanelScope panel(a_context.text("Inspector##EditorInspector", "インスペクター###Inspector##EditorInspector"),
                         ImGuiWindowFlags_NoCollapse);
        if (panel.is_visible())
            ImGui::TextUnformatted(a_context.text("No project open", "プロジェクト未選択"));
    }
}
/// @brief Modal の選択・取消だけを返し、確認待ちの間も通常 Frame を進行させる
void draw_unsaved_confirmation(const EditorUiContext &a_context, bool &a_wasRequested, bool &a_hasResponse,
                               bool &a_shouldClose, std::vector<EditorAction> &a_actions)
{
    const bool isBuild = a_context.isBuildConfirmationRequested;
    const char *popupName = a_context.text("Save changes?##EditorUnsavedChanges",
                                           "変更を保存しますか？###Save changes?##EditorUnsavedChanges");
    // reset は Context 外から呼ばれるため、表示中 Popup の回収を描画境界へ遅延する
    if (a_shouldClose)
    {
        if (ImGui::BeginPopupModal(popupName))
        {
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        a_shouldClose = false;
    }
    // Host が要求を取り消した場合も Popup を閉じ、次の確認を別の操作として受け付ける
    if (!a_context.isUnsavedConfirmationRequested)
    {
        if (a_wasRequested && ImGui::BeginPopupModal(popupName))
        {
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        a_wasRequested = false;
        a_hasResponse = false;
        return;
    }
    if (!a_wasRequested)
    {
        ImGui::OpenPopup(popupName);
        a_wasRequested = true;
    }
    // Host が返却済み Action を適用するまで、同じ確認を再生成・再送しない
    if (a_hasResponse)
        return;

    const auto *viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, {0.5f, 0.5f});
    bool isOpen = true;
    const bool isVisible = ImGui::BeginPopupModal(popupName, &isOpen,
                                                  ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                                      ImGuiWindowFlags_NoDocking);
    if (isVisible)
    {
        struct PopupScope final
        {
            /// @brief 選択 Action の割当失敗でも Popup の Stack を閉じる
            ~PopupScope()
            {
                ImGui::EndPopup();
            }
        } popup;
        ImGui::TextUnformatted(a_context.text("This scene has unsaved changes.", "シーンに未保存の変更があります。"));
        ImGui::TextUnformatted(isBuild ? a_context.text("Save before building the project startup scene?",
                                                        "起動シーンをビルドする前に、変更を保存しますか？")
                                       : a_context.text("Save before continuing?", "続行する前に変更を保存しますか？"));
        if (isBuild)
            ImGui::TextUnformatted(
                a_context.text("Build Saved Files keeps your unsaved edits in the Editor.",
                               "保存済みファイルでビルドしても、編集中の変更は Editor に残ります。"));
        ImGui::Separator();
        std::optional<EditorActionKind> response;
        if (ImGui::Button(isBuild ? a_context.text("Save and Build", "保存してビルド###Save and Build")
                                  : a_context.text("Save", "保存###Save"),
                          {isBuild ? 150.0f : 100.0f, 0}))
            response = EditorActionKind::ConfirmSaveChanges;
        ImGui::SameLine();
        if (ImGui::Button(isBuild ? a_context.text("Build Saved Files", "保存済みでビルド###Build Saved Files")
                                  : a_context.text("Discard", "破棄###Discard"),
                          {isBuild ? 150.0f : 100.0f, 0}))
            response = EditorActionKind::ConfirmDiscardChanges;
        ImGui::SameLine();
        // Modal の初回 Frame でも取消を観測する。Shortcut の前 Frame 予約や入力所有権へ依存しない
        const bool isCancelled = ImGui::IsKeyPressed(ImGuiKey_Escape, ImGuiInputFlags_None, ImGuiKeyOwner_Any);
        if (ImGui::Button(a_context.text("Cancel", "キャンセル###Cancel"), {100, 0}) || isCancelled)
            response = EditorActionKind::CancelDocumentChange;
        if (response)
        {
            emit_editor_action(a_actions, *response);
            a_hasResponse = true;
            ImGui::CloseCurrentPopup();
        }
    }
    // Title Bar の X でも明示的な取消を返し、保留中の処理を残さない
    if (!isOpen && !a_hasResponse)
    {
        emit_editor_action(a_actions, EditorActionKind::CancelDocumentChange);
        a_hasResponse = true;
    }
}

/// @brief File 名の入力段階で Path と非 ASCII 文字を拒否し、最終的な予約語検証は Creator に委ねる
bool is_script_name_input(std::string_view a_name) noexcept
{
    const auto isLetter = [](char a_character)
    { return (a_character >= 'A' && a_character <= 'Z') || (a_character >= 'a' && a_character <= 'z'); };
    if (a_name.empty() || !isLetter(a_name.front()))
        return false;
    return std::all_of(
        a_name.begin(), a_name.end(), [&](char a_character)
        { return isLetter(a_character) || (a_character >= '0' && a_character <= '9') || a_character == '_'; });
}

/// @brief Source 作成を所有値の Action にし、FileSystem と Project の更新を Host へ委ねる
void draw_script_creation(const EditorUiContext &a_context, EditorUiState &a_state,
                          std::vector<EditorAction> &a_actions)
{
    const char *popupName =
        a_context.text("New Script##EditorCreateScript", "スクリプトの作成###New Script##EditorCreateScript");
    if (!can_create_editor_script(a_context))
    {
        a_state.shouldCloseScriptCreationPopup |= a_state.isScriptCreationRequested;
        a_state.isScriptCreationRequested = false;
        a_state.shouldOpenScriptCreationPopup = false;
        a_state.needsScriptNameFocus = false;
        a_state.scriptName.fill(0);
    }
    // Project 切替や外部からの操作開始で取り消した Popup を、ImGui Context のある境界で回収する
    if (a_state.shouldCloseScriptCreationPopup)
    {
        if (ImGui::BeginPopupModal(popupName))
        {
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        a_state.shouldCloseScriptCreationPopup = false;
    }
    if (!a_state.isScriptCreationRequested)
        return;
    if (a_state.shouldOpenScriptCreationPopup)
    {
        ImGui::OpenPopup(popupName);
        a_state.shouldOpenScriptCreationPopup = false;
    }
    const auto *viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, {0.5f, 0.5f});
    bool isOpen = true;
    const bool isVisible = ImGui::BeginPopupModal(popupName, &isOpen,
                                                  ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                                      ImGuiWindowFlags_NoDocking);
    if (isVisible)
    {
        struct PopupScope final
        {
            /// @brief Action の確保失敗でも Popup Stack を対応させる
            ~PopupScope()
            {
                ImGui::EndPopup();
            }
        } popup;
        ImGui::TextUnformatted(a_context.text("Create a C++ Behaviour with script registration.",
                                              "登録処理を含む C++ Behaviour を作成します。"));
        if (a_state.needsScriptNameFocus)
        {
            ImGui::SetKeyboardFocusHere();
            a_state.needsScriptNameFocus = false;
        }
        ImGui::SetNextItemWidth(320);
        const bool isSubmitted = ImGui::InputText(a_context.text("Name", "名前###Name"), a_state.scriptName.data(),
                                                  a_state.scriptName.size(), ImGuiInputTextFlags_EnterReturnsTrue);
        const std::string_view name(a_state.scriptName.data());
        const bool isValid = is_script_name_input(name);
        ImGui::Text("Source/%s.h", a_state.scriptName.data());
        ImGui::Text("Source/%s.cpp", a_state.scriptName.data());
        if (!name.empty() && !isValid)
            ImGui::TextUnformatted(a_context.text("Use ASCII letters, digits and underscores; start with a letter.",
                                                  "半角英字で始め、半角英数字とアンダースコアを使ってください。"));
        bool isCreated = false;
        {
            EditorDisabledScope disabled(!isValid);
            isCreated = ImGui::Button(a_context.text("Create", "作成###Create"), {100, 0});
        }
        ImGui::SameLine();
        const bool isCancelled = ImGui::Button(a_context.text("Cancel", "キャンセル###Cancel"), {100, 0}) ||
                                 ImGui::IsKeyPressed(ImGuiKey_Escape, ImGuiInputFlags_None, ImGuiKeyOwner_Any);
        if (!isCancelled && (isCreated || isSubmitted) && isValid)
        {
            EditorAction action;
            action.kind = EditorActionKind::CreateScript;
            action.scriptName = name;
            a_actions.push_back(std::move(action));
            isOpen = false;
        }
        if (isCancelled)
            isOpen = false;
        if (!isOpen)
            ImGui::CloseCurrentPopup();
    }
    if (!isOpen)
    {
        a_state.isScriptCreationRequested = false;
        a_state.scriptName.fill(0);
    }
}
} // namespace

struct EditorUi::State final : EditorUiState
{
    bool wasUnsavedConfirmationRequested = false;
    bool hasUnsavedResponse = false;
    bool shouldCloseUnsavedPopup = false;
};

EditorUi::EditorUi(CreateToken) noexcept
{
}
EditorUi::~EditorUi() = default;

Result<std::unique_ptr<EditorUi>> EditorUi::create()
{
    try
    {
        auto ui = std::make_unique<EditorUi>(CreateToken{});
        ui->m_state = std::make_unique<State>();
        return Result<std::unique_ptr<EditorUi>>::success(std::move(ui));
    }
    catch (const std::bad_alloc &)
    {
        return Result<std::unique_ptr<EditorUi>>::failure(
            {ErrorCategory::PlatformFailure, "EditorUi.create.allocation"});
    }
}

Result<std::vector<EditorAction>> EditorUi::draw(const SceneDocument &a_document, const EditorUiContext &a_context)
{
    return draw(&a_document, a_context);
}

Result<std::vector<EditorAction>> EditorUi::draw(const SceneDocument *a_document, const EditorUiContext &a_context)
{
    if (!ImGui::GetCurrentContext() || !m_state)
        return Result<std::vector<EditorAction>>::failure({ErrorCategory::InvalidState, "EditorUi.draw.context"});
    try
    {
        auto context = a_context;
        if (!a_document)
        {
            // 呼出側の表示値が残っていても、Document のない Frame では編集要求を生成しない
            reset();
            context.isProjectOpen = false;
            context.isSceneViewEnabled = false;
            context.isUnsavedConfirmationRequested = false;
            context.isBuildConfirmationRequested = false;
            context.canPlay = false;
            context.isPlaying = false;
            context.isPlayBusy = false;
            context.canStop = false;
            context.script = {};
            context.build = {};
            context.hasAssetBrowser = false;
            context.scriptRemoval = nullptr;
        }
        std::vector<EditorAction> actions;
        actions.reserve(16);
        if ((context.hasAssetBrowser && m_state->isAssetBrowserVisible) ||
            (context.isProjectOpen && m_state->isLogVisible))
            context.reservedBottomHeight = (std::min)(240.0f, context.displayHeight * 0.30f);
        const bool wasScriptCreationRequested = m_state->isScriptCreationRequested;
        // Modal の初回 Frame も無効化し、Popup がまだ開く前の背景入力を処理しない
        const bool isInputBlocked =
            context.isUnsavedConfirmationRequested || wasScriptCreationRequested || context.scriptRemoval;
        ImGui::BeginDisabled(isInputBlocked);
        struct DisabledScope final
        {
            /// @brief Panel の失敗・例外でも Disabled の Stack を対応させる
            ~DisabledScope()
            {
                ImGui::EndDisabled();
            }
        };
        {
            DisabledScope disabled;
            draw_editor_dockspace(context, *m_state);
            if (a_document)
            {
                // 未選択の起動 Shell は表示だけを維持し、仮 Document を編集対象として公開しない
                EditorDisabledScope noProject(!context.isProjectOpen);
                draw_object_list(*a_document, context, *m_state, actions);
                auto inspector = draw_transform_inspector(*a_document, context, *m_state, actions);
                if (!inspector.has_value())
                    return Result<std::vector<EditorAction>>::failure(*inspector.try_error());
                draw_scene_view(context, actions);
            }
            else
            {
                draw_empty_project_panels(context);
                draw_scene_view(context, actions);
            }
            draw_asset_browser(context, *m_state, actions);
            draw_editor_log(context, *m_state);
            if (a_document && context.isProjectOpen)
                draw_build_panel(context, *m_state, actions);
            draw_editor_status(a_document, context, *m_state);
            draw_editor_toolbar(a_document, context, *m_state, actions, !isInputBlocked);
            // Panel より後に Menu を構築し、旧 Layout から移行する Frame でも入力領域を確保する
            draw_scene_menu(a_document, *m_state, actions, context, !isInputBlocked);
        }
        if (isInputBlocked)
            actions.clear();
        draw_script_creation(context, *m_state, actions);
        if (context.isUnsavedConfirmationRequested)
            context.scriptRemoval = nullptr;
        draw_script_removal(context, *m_state, actions);
        draw_unsaved_confirmation(context, m_state->wasUnsavedConfirmationRequested, m_state->hasUnsavedResponse,
                                  m_state->shouldCloseUnsavedPopup, actions);
        return Result<std::vector<EditorAction>>::success(std::move(actions));
    }
    catch (const std::bad_alloc &)
    {
        return Result<std::vector<EditorAction>>::failure({ErrorCategory::PlatformFailure, "EditorUi.draw.allocation"});
    }
}

void EditorUi::reset() noexcept
{
    if (!m_state)
        return;
    m_state->selectedId.clear();
    m_state->hasTransform = false;
    m_state->hasEdit = false;
    m_state->activeField = -1;
    m_state->activeInputId = 0;
    m_state->needsSync = true;
    m_state->hasInvalidRotationInput = false;
    m_state->playTransform = {};
    m_state->wasPlaying = false;
    m_state->shouldCloseScriptCreationPopup |= m_state->isScriptCreationRequested;
    m_state->isScriptCreationRequested = false;
    m_state->shouldOpenScriptCreationPopup = false;
    m_state->needsScriptNameFocus = false;
    m_state->scriptName.fill(0);
    // 応答消費直後の次 Pump が別の確認を開始しても、false Frame を挟まず再表示できるようにする
    m_state->shouldCloseUnsavedPopup |= m_state->wasUnsavedConfirmationRequested && !m_state->hasUnsavedResponse;
    m_state->wasUnsavedConfirmationRequested = false;
    m_state->hasUnsavedResponse = false;
}

void emit_editor_action(std::vector<EditorAction> &a_actions, EditorActionKind a_kind, std::string_view a_objectId,
                        const math::floatTransform &a_transform)
{
    a_actions.push_back({a_kind, std::string(a_objectId), a_transform});
}

void finish_transform_input(EditorUiState &a_state, std::vector<EditorAction> &a_actions, EditorActionKind a_kind)
{
    if (!a_state.hasEdit)
        return;
    emit_editor_action(a_actions, a_kind, a_state.selectedId);
    a_state.hasEdit = false;
    a_state.activeField = -1;
    a_state.activeInputId = 0;
    a_state.needsSync = true;
}
} // namespace cue
