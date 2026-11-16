#include "EditorUiPanels.h"

#include <utility>

#include <imgui_internal.h>

namespace cue
{
namespace
{
/// @brief Path で選択を引き直し、Snapshot の更新をまたいで Pointer を保持しない
const EditorAssetFolder *find_folder(std::span<const EditorAssetFolder> a_folders, const Path &a_path)
{
    for (const auto &folder : a_folders)
    {
        if (folder.path.utf8() == a_path.utf8())
            return &folder;
        if (const auto *selected = find_folder(folder.folders, a_path))
            return selected;
    }
    return nullptr;
}

/// @brief Folder 選択と展開を分け、右側から移動した選択先の親を展開する
void draw_folder(const EditorAssetFolder &a_folder, Path &a_selected)
{
    const auto &path = a_folder.path.utf8();
    const bool isSelected = a_selected.utf8() == path;
    if (a_selected.utf8().starts_with(path + '/'))
        ImGui::SetNextItemOpen(true, ImGuiCond_Always);
    ImGuiTreeNodeFlags flags =
        ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_DrawLinesToNodes;
    if (isSelected)
        flags |= ImGuiTreeNodeFlags_Selected;
    if (a_folder.folders.empty())
        flags |= ImGuiTreeNodeFlags_Leaf;
    const bool isOpen = ImGui::TreeNodeEx(path.c_str(), flags, "%s", a_folder.name.c_str());
    const bool isSelectedNow = ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen();
    if (isOpen)
    {
        struct TreeScope final
        {
            /// @brief UI の操作要求を確保できない場合も Tree を閉じる
            ~TreeScope()
            {
                ImGui::TreePop();
            }
        } tree;
        for (const auto &folder : a_folder.folders)
            draw_folder(folder, a_selected);
    }
    if (isSelectedNow)
        a_selected = a_folder.path;
}

/// @brief Browser からの操作を借用 Snapshot と切り離して返す
void request_asset(EditorUiState &a_state, std::vector<EditorAction> &a_actions, EditorActionKind a_kind,
                   const EditorAsset &a_asset, bool a_isEnabled = true)
{
    finish_transform_input(a_state, a_actions);
    EditorAction action;
    action.kind = a_kind;
    action.assetPath = a_asset.path;
    action.scriptTypeId = a_asset.scriptTypeId;
    action.isEnabled = a_isEnabled;
    a_actions.push_back(std::move(action));
}

/// @brief 一つの Asset を表示し、選択操作からだけ開く・削除する要求を返す
void draw_asset(const EditorUiContext &a_context, EditorUiState &a_state, std::vector<EditorAction> &a_actions,
                const EditorAsset &a_asset)
{
    ImGui::PushID(a_asset.path.utf8().c_str());
    struct IdScope final
    {
        /// @brief ファイル単位の ID Scope を戻す
        ~IdScope()
        {
            ImGui::PopID();
        }
    } id;
    const auto label = a_asset.name + "###Asset";
    const bool isActivated = ImGui::Selectable(label.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick);
    if (isActivated && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
    {
        if (a_asset.kind == EditorAssetKind::Scene && a_context.canManageAssets)
            request_asset(a_state, a_actions, EditorActionKind::OpenAssetScene, a_asset);
        if (a_asset.kind == EditorAssetKind::Script && a_context.script.canEditSource && !a_asset.isMissing)
            request_asset(a_state, a_actions, EditorActionKind::OpenScriptSource, a_asset);
    }
    if (a_asset.kind == EditorAssetKind::Script && ImGui::BeginPopupContextItem())
    {
        struct PopupScope final
        {
            /// @brief Action の確保失敗でも Popup Stack を戻す
            ~PopupScope()
            {
                ImGui::EndPopup();
            }
        } popup;
        if (ImGui::MenuItem(a_context.text("Open in Visual Studio", "Visual Studio で開く###Open in Visual Studio"),
                            nullptr, false, a_context.script.canEditSource && !a_asset.isMissing))
            request_asset(a_state, a_actions, EditorActionKind::OpenScriptSource, a_asset);
        if (ImGui::MenuItem(a_context.text("Show Uses...", "使用箇所を表示...###Show Uses..."), nullptr, false,
                            a_context.canManageAssets))
            request_asset(a_state, a_actions, EditorActionKind::InspectScriptRemoval, a_asset, false);
        if (ImGui::MenuItem(
                a_context.text("Delete from Project...", "プロジェクトから削除...###Delete from Project..."), nullptr,
                false, a_context.canManageAssets))
            request_asset(a_state, a_actions, EditorActionKind::InspectScriptRemoval, a_asset);
    }
    if (a_asset.isMissing)
        ImGui::TextDisabled("%s", a_context.text("Source missing", "ソースファイル欠落"));
    else if (a_asset.kind == EditorAssetKind::Script)
        ImGui::TextDisabled("%s", a_context.text("C++ Script", "C++ スクリプト"));
    else if (a_asset.kind == EditorAssetKind::Scene)
        ImGui::TextDisabled("%s", a_context.text("Scene", "シーン"));
}
} // namespace

void draw_asset_browser(const EditorUiContext &a_context, EditorUiState &a_state, std::vector<EditorAction> &a_actions)
{
    if (!a_context.hasAssetBrowser || !a_state.isAssetBrowserVisible)
        return;

    const auto *viewport = ImGui::GetMainViewport();
    const auto folderWidth = (std::min)(240.0f, a_context.displayWidth * 0.25f);
    const auto height = a_context.reservedBottomHeight;
    const auto top = viewport->Pos.y + a_context.displayHeight - ImGui::GetFrameHeight() - 6 - height;
    if (!find_folder(a_context.assetFolders, a_state.assetFolder))
        a_state.assetFolder = a_context.assetFolders.empty() ? Path{} : a_context.assetFolders.front().path;

    set_initial_panel(viewport->Pos.x, top, folderWidth, height);
    {
        PanelScope panel(
            a_context.text("Project Folders###EditorProjectFolders", "プロジェクトフォルダー###EditorProjectFolders"));
        if (panel.is_visible())
            for (const auto &folder : a_context.assetFolders)
                draw_folder(folder, a_state.assetFolder);
    }

    set_initial_panel(viewport->Pos.x + folderWidth, top, a_context.displayWidth - folderWidth, height);
    PanelScope panel(a_context.text("Assets###EditorAssets", "アセット###EditorAssets"));
    if (!panel.is_visible())
        return;
    if (ImGui::SmallButton(a_context.text("Refresh", "更新###Refresh")))
        emit_editor_action(a_actions, EditorActionKind::RefreshAssets);
    if (can_create_editor_script(a_context) &&
        ImGui::BeginPopupContextWindow("AssetActions",
                                       ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
    {
        if (ImGui::MenuItem(a_context.text("New Script...", "スクリプトを作成...###New Script...")))
        {
            finish_transform_input(a_state, a_actions);
            a_state.scriptName.fill(0);
            a_state.isScriptCreationRequested = true;
            a_state.shouldOpenScriptCreationPopup = true;
            a_state.needsScriptNameFocus = true;
        }
        ImGui::EndPopup();
    }

    const auto *selected = find_folder(a_context.assetFolders, a_state.assetFolder);
    if (!selected)
        return;
    ImGui::SameLine();
    ImGui::TextUnformatted(selected->path.utf8().c_str());
    ImGui::Separator();

    const int columns = std::clamp(static_cast<int>(ImGui::GetContentRegionAvail().x / 150.0f), 1, 10);
    if (!ImGui::BeginTable("AssetGrid", columns, ImGuiTableFlags_SizingStretchSame))
        return;
    struct TableScope final
    {
        /// @brief Path の所有値確保が失敗しても Table Stack を閉じる
        ~TableScope()
        {
            ImGui::EndTable();
        }
    } table;
    for (const auto &folder : selected->folders)
    {
        ImGui::TableNextColumn();
        ImGui::PushID(folder.path.utf8().c_str());
        const bool isActivated = ImGui::Selectable(folder.name.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick);
        ImGui::PopID();
        if (isActivated && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            a_state.assetFolder = folder.path;
        ImGui::TextDisabled("%s", a_context.text("Folder", "フォルダー"));
    }
    for (const auto &asset : selected->assets)
    {
        ImGui::TableNextColumn();
        draw_asset(a_context, a_state, a_actions, asset);
    }
}

void draw_script_removal(const EditorUiContext &a_context, EditorUiState &a_state, std::vector<EditorAction> &a_actions)
{
    const auto *title =
        a_context.scriptRemoval && !a_context.scriptRemoval->canDelete
            ? a_context.text("Script Uses###EditorDeleteScript", "使用箇所###EditorDeleteScript")
            : a_context.text("Delete Script?###EditorDeleteScript", "スクリプトを削除しますか？###EditorDeleteScript");
    if (!a_context.scriptRemoval)
    {
        if (a_state.hasScriptRemovalPopup && ImGui::BeginPopupModal(title))
        {
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        a_state.hasScriptRemovalPopup = false;
        return;
    }
    if (!a_state.hasScriptRemovalPopup)
    {
        ImGui::OpenPopup(title);
        a_state.hasScriptRemovalPopup = true;
    }
    ImGui::SetNextWindowSize({640, 0}, ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
        return;
    struct ModalScope final
    {
        /// @brief 確定 Action を確保できなくても Modal Stack を戻す
        ~ModalScope()
        {
            ImGui::EndPopup();
        }
    } modal;
    const auto &removal = *a_context.scriptRemoval;
    ImGui::TextUnformatted(removal.name.data(), removal.name.data() + removal.name.size());
    ImGui::SeparatorText(a_context.text("Source files", "ソースファイル"));
    for (const auto &file : removal.files)
        ImGui::BulletText("%s", file.c_str());
    ImGui::SeparatorText(a_context.text("Scene assignments", "シーンの割り当て"));
    if (ImGui::BeginChild("ScriptUses", {0, 140}, ImGuiChildFlags_Borders))
    {
        if (removal.uses.empty())
            ImGui::TextUnformatted(a_context.text("No assignments", "割り当てなし"));
        for (const auto &use : removal.uses)
            ImGui::BulletText("%s", use.c_str());
    }
    ImGui::EndChild();
    if (removal.canDelete && removal.requiresSceneSave)
        ImGui::TextWrapped("%s", a_context.text("Save the edited scene before deleting this script.",
                                                "このスクリプトを削除する前に、編集中のシーンを保存してください。"));
    if (removal.canDelete)
    {
        if (ImGui::Button(removal.requiresSceneSave ? a_context.text("Save and review again", "保存して再確認")
                                                    : a_context.text("Delete from Project", "プロジェクトから削除")))
            emit_editor_action(a_actions, EditorActionKind::ConfirmScriptRemoval);
        ImGui::SameLine();
    }
    if (ImGui::Button(removal.canDelete ? a_context.text("Cancel", "キャンセル###Cancel")
                                        : a_context.text("Close", "閉じる###Close")) ||
        ImGui::IsKeyPressed(ImGuiKey_Escape, ImGuiInputFlags_None, ImGuiKeyOwner_Any))
        emit_editor_action(a_actions, EditorActionKind::CancelScriptRemoval);
}
} // namespace cue
