#include "EditorUiPanels.h"

#include <algorithm>
#include <cfloat>
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

/// @brief 表示先を変更した場合だけ選択と、元の Folder に対する検索条件を解除する
void open_folder(EditorUiState &a_state, const Path &a_folder)
{
    if (a_state.assetFolder.utf8() == a_folder.utf8())
        return;
    a_state.assetFolder = a_folder;
    a_state.selectedAsset = {};
    a_state.assetSearch.fill(0);
}

/// @brief 対象ファイルや Folder の場所を他の開発 Tool で使えるようにする
void draw_copy_path(const EditorUiContext &a_context, const Path &a_path)
{
    if (ImGui::MenuItem(a_context.text("Copy Path", "パスをコピー###Copy Path")))
        ImGui::SetClipboardText(a_path.utf8().c_str());
}

/// @brief 表示対象の Folder を切り替えるか返す。FileSystem は変更しない
bool draw_folder_menu(const EditorUiContext &a_context, const EditorAssetFolder &a_folder)
{
    if (!ImGui::BeginPopupContextItem())
        return false;
    EditorPopupScope popup;
    const bool shouldOpen = ImGui::MenuItem(a_context.text("Open Folder", "フォルダーを開く###Open Folder"));
    ImGui::Separator();
    draw_copy_path(a_context, a_folder.path);
    return shouldOpen;
}

/// @brief Folder 選択と展開を分け、右側から移動した選択先の親を展開する
void draw_folder(const EditorUiContext &a_context, const EditorAssetFolder &a_folder, EditorUiState &a_state)
{
    const auto &path = a_folder.path.utf8();
    const bool isSelected = a_state.assetFolder.utf8() == path;
    if (a_state.assetFolder.utf8().starts_with(path + '/'))
        ImGui::SetNextItemOpen(true, ImGuiCond_Always);
    ImGuiTreeNodeFlags flags =
        ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_DrawLinesToNodes;
    if (isSelected)
        flags |= ImGuiTreeNodeFlags_Selected;
    if (a_folder.folders.empty())
        flags |= ImGuiTreeNodeFlags_Leaf;
    const bool isOpen = ImGui::TreeNodeEx(path.c_str(), flags, "%s %s", editor_icon::k_folder.data(),
                                         a_folder.name.c_str());
    const bool isSelectedNow = ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen();
    struct TreeScope final
    {
        bool isOpen;
        /// @brief UI の操作要求を確保できない場合も Tree を閉じる
        ~TreeScope()
        {
            if (isOpen)
                ImGui::TreePop();
        }
    } tree{isOpen};
    if (draw_folder_menu(a_context, a_folder) || isSelectedNow)
        open_folder(a_state, a_folder.path);
    if (isOpen)
        for (const auto &folder : a_folder.folders)
            draw_folder(a_context, folder, a_state);
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

/// @brief 現在の Font Size と Cell 幅に合わせ、文字列を一行で表示する
void draw_asset_name(std::string_view a_name, ImVec2 a_pos, float a_right, bool a_centered, const char *a_status)
{
    const auto textSize = ImGui::CalcTextSize(a_name.data(), a_name.data() + a_name.size());
    const bool isClipped = textSize.x > a_right - a_pos.x;
    if (a_centered && !isClipped)
        a_pos.x += (a_right - a_pos.x - textSize.x) * 0.5f;
    if (a_right > a_pos.x)
    {
        ImGui::RenderTextEllipsis(ImGui::GetWindowDrawList(), a_pos, {a_right, a_pos.y + ImGui::GetTextLineHeight()},
                                  a_right, a_name.data(), a_name.data() + a_name.size(), &textSize);
    }
    if ((isClipped || a_status) && ImGui::BeginItemTooltip())
    {
        ImGui::TextUnformatted(a_name.data(), a_name.data() + a_name.size());
        if (a_status)
            ImGui::TextDisabled("%s", a_status);
        ImGui::EndTooltip();
    }
}

/// @brief 両モードで Path を選択 ID に使い、ダブルクリックだけを Open 操作として返す
bool draw_asset_item(EditorUiState &a_state, const Path &a_path, std::string_view a_name, std::string_view a_icon,
                     const char *a_status = nullptr)
{
    const bool isList = a_state.isAssetListVisible;
    const auto pos = ImGui::GetCursorScreenPos();
    const auto &style = ImGui::GetStyle();
    const float width = ImGui::GetContentRegionAvail().x;
    const float textHeight = ImGui::GetTextLineHeight();
    const float iconSize = ImGui::GetFontSize() * (isList ? 1.0f : 2.5f);
    const float padding = style.FramePadding.y;
    const float height = isList ? textHeight + padding * 2 : iconSize + textHeight + padding * 5;
    auto *drawList = ImGui::GetWindowDrawList();
    if (!isList && width > 0.0f)
        drawList->AddRectFilled(pos, {pos.x + width, pos.y + height}, ImGui::GetColorU32(ImGuiCol_FrameBg),
                                style.FrameRounding);

    auto flags = ImGuiSelectableFlags_AllowDoubleClick | ImGuiSelectableFlags_NoPadWithHalfSpacing;
    if (isList)
        flags |= ImGuiSelectableFlags_SpanAllColumns;
    ImGui::PushID(a_path.utf8().c_str());
    const bool wasActivated = ImGui::Selectable("##AssetEntry", a_state.selectedAsset.utf8() == a_path.utf8(),
                                              flags, {0.0f, height});
    ImGui::PopID();
    const bool shouldOpen = wasActivated && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
    if (wasActivated)
        a_state.selectedAsset = a_path;

    if (ImGui::IsItemVisible())
    {
        auto *font = ImGui::GetFont();
        const auto iconExtent = font->CalcTextSizeA(iconSize, FLT_MAX, 0.0f, a_icon.data(), a_icon.data() + a_icon.size());
        const float iconX = isList ? pos.x : pos.x + (std::max)(0.0f, (width - iconExtent.x) * 0.5f);
        const float iconY = pos.y + padding * (isList ? 1.0f : 2.0f);
        const auto iconColor = ImGui::GetColorU32(a_status ? ImGuiCol_TextDisabled : ImGuiCol_Text);
        drawList->AddText(font, iconSize, {iconX, iconY}, iconColor, a_icon.data(), a_icon.data() + a_icon.size());
        const auto namePos = isList ? ImVec2{iconX + iconExtent.x + style.ItemInnerSpacing.x, iconY}
                                    : ImVec2{pos.x + style.FramePadding.x, iconY + iconSize + padding};
        draw_asset_name(a_name, namePos, pos.x + width - style.FramePadding.x, !isList, a_status);
    }
    return shouldOpen;
}

/// @brief Scene と Script の権限を両モードで共有し、対応する既存 Action を返す
void draw_asset(const EditorUiContext &a_context, EditorUiState &a_state, std::vector<EditorAction> &a_actions,
                const EditorAsset &a_asset)
{
    const auto icon = a_asset.kind == EditorAssetKind::Scene     ? editor_icon::k_scene
                      : a_asset.kind == EditorAssetKind::Script ? editor_icon::k_script
                                                                 : editor_icon::k_file;
    const char *status = a_asset.isMissing ? a_context.text("Source missing", "ソースファイル欠落") : nullptr;
    if (draw_asset_item(a_state, a_asset.path, a_asset.name, icon, status))
    {
        if (a_asset.kind == EditorAssetKind::Scene && a_context.canManageAssets)
            request_asset(a_state, a_actions, EditorActionKind::OpenAssetScene, a_asset);
        if (a_asset.kind == EditorAssetKind::Script && a_context.script.canEditSource && !a_asset.isMissing)
            request_asset(a_state, a_actions, EditorActionKind::OpenScriptSource, a_asset);
    }
    if (ImGui::BeginPopupContextItem())
    {
        EditorPopupScope popup;
        if (a_asset.kind == EditorAssetKind::Scene)
        {
            if (ImGui::MenuItem(a_context.text("Open Scene", "シーンを開く###Open Scene"), nullptr, false,
                                a_context.canManageAssets))
                request_asset(a_state, a_actions, EditorActionKind::OpenAssetScene, a_asset);
            ImGui::Separator();
        }
        else if (a_asset.kind == EditorAssetKind::Script)
        {
            if (ImGui::MenuItem(a_context.text("Open in Visual Studio", "Visual Studio で開く###Open in Visual Studio"),
                                nullptr, false, a_context.script.canEditSource && !a_asset.isMissing))
                request_asset(a_state, a_actions, EditorActionKind::OpenScriptSource, a_asset);
            if (ImGui::MenuItem(a_context.text("Show Uses...", "使用箇所を表示...###Show Uses..."), nullptr, false,
                                a_context.canManageAssets))
                request_asset(a_state, a_actions, EditorActionKind::InspectScriptRemoval, a_asset, false);
            ImGui::Separator();
        }
        draw_copy_path(a_context, a_asset.path);
        if (a_asset.kind == EditorAssetKind::Script)
        {
            ImGui::Separator();
            if (ImGui::MenuItem(a_context.text("Delete from Project...", "プロジェクトから削除...###Delete from Project..."),
                                nullptr, false, a_context.canManageAssets))
                request_asset(a_state, a_actions, EditorActionKind::InspectScriptRemoval, a_asset);
        }
    }
    if (a_state.isAssetListVisible)
    {
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        const auto *type = a_asset.kind == EditorAssetKind::Scene     ? a_context.text("Scene", "シーン")
                           : a_asset.kind == EditorAssetKind::Script ? a_context.text("C++ Script", "C++ スクリプト")
                                                                      : a_context.text("File", "ファイル");
        ImGui::TextDisabled("%s", status ? status : type);
    }
}

/// @brief 英字の大文字小文字を無視し、UTF-8 の日本語を含む名前も部分一致で検索する
bool matches_asset_name(std::string_view a_name, std::string_view a_query) noexcept
{
    return a_query.empty() || !std::ranges::search(a_name, a_query, [](char a_left, char a_right)
    {
        return ImToUpper(a_left) == ImToUpper(a_right);
    }).empty();
}

/// @brief Folder 直下の検索結果を描き、表示対象が一つでもあるか返す
bool draw_assets(const EditorUiContext &a_context, EditorUiState &a_state, std::vector<EditorAction> &a_actions,
                 const EditorAssetFolder &a_selected)
{
    const bool isList = a_state.isAssetListVisible;
    const auto fontSize = ImGui::GetFontSize();
    const int columns = isList ? 2 : std::clamp(static_cast<int>(ImGui::GetContentRegionAvail().x / (fontSize * 7)), 1, 10);
    const auto flags = isList ? ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp
                              : ImGuiTableFlags_SizingStretchSame;
    if (!ImGui::BeginTable(isList ? "AssetList" : "AssetGrid", columns, flags))
        return true;
    struct TableScope final
    {
        /// @brief Path の所有値確保が失敗しても Table Stack を閉じる
        ~TableScope()
        {
            ImGui::EndTable();
        }
    } table;
    if (isList)
    {
        ImGui::TableSetupColumn(a_context.text("Name", "名前"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(a_context.text("Type", "種類"), ImGuiTableColumnFlags_WidthFixed, fontSize * 9);
        ImGui::TableHeadersRow();
    }
    const std::string_view query(a_state.assetSearch.data());
    const EditorAssetFolder *nextFolder = nullptr;
    bool hasEntries = false;
    for (const auto &folder : a_selected.folders)
    {
        if (!matches_asset_name(folder.name, query))
            continue;
        hasEntries = true;
        ImGui::TableNextColumn();
        const bool shouldOpenItem = draw_asset_item(a_state, folder.path, folder.name, editor_icon::k_folder);
        const bool shouldOpen = draw_folder_menu(a_context, folder);
        if (shouldOpen || shouldOpenItem)
            nextFolder = &folder;
        if (isList)
        {
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", a_context.text("Folder", "フォルダー"));
        }
    }
    for (const auto &asset : a_selected.assets)
    {
        if (!matches_asset_name(asset.name, query))
            continue;
        hasEntries = true;
        ImGui::TableNextColumn();
        draw_asset(a_context, a_state, a_actions, asset);
    }
    // 遷移で検索 Buffer を変更する前に元の Folder の描画を終える
    if (nextFolder)
        open_folder(a_state, nextFolder->path);
    return hasEntries;
}

/// @brief 自分の PC の絶対 Path を使わず、Project 内の現在地を表示する
std::string asset_location(std::span<const EditorAssetFolder> a_roots, const EditorAssetFolder &a_selected)
{
    const std::string_view selectedPath(a_selected.path.utf8());
    for (const auto &root : a_roots)
    {
        const std::string_view rootPath(root.path.utf8());
        if (selectedPath == rootPath)
            return root.name;
        if (!selectedPath.starts_with(rootPath) || selectedPath[rootPath.size()] != '/')
            continue;
        auto location = root.name;
        for (const char character : selectedPath.substr(rootPath.size()))
        {
            if (character == '/')
                location += " > ";
            else
                location += character;
        }
        return location;
    }
    return a_selected.name;
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
        open_folder(a_state, a_context.assetFolders.empty() ? Path{} : a_context.assetFolders.front().path);

    set_initial_panel(viewport->Pos.x, top, folderWidth, height);
    {
        PanelScope panel(
            a_context.text("Project Folders###EditorProjectFolders", "プロジェクトフォルダー###EditorProjectFolders"));
        if (panel.is_visible())
            for (const auto &folder : a_context.assetFolders)
                draw_folder(a_context, folder, a_state);
    }

    set_initial_panel(viewport->Pos.x + folderWidth, top, a_context.displayWidth - folderWidth, height);
    PanelScope panel(a_context.text("Assets###EditorAssets", "アセット###EditorAssets"));
    if (!panel.is_visible())
        return;
    const auto refresh = editor_icon_label(editor_icon::k_refresh,
                                           a_context.text("Refresh###RefreshAssets", "更新###RefreshAssets"));
    if (ImGui::SmallButton(refresh.c_str()))
        emit_editor_action(a_actions, EditorActionKind::RefreshAssets);
    ImGui::SameLine();
    if (ImGui::RadioButton(a_context.text("Tiles###AssetGridMode", "タイル###AssetGridMode"), !a_state.isAssetListVisible))
        a_state.isAssetListVisible = false;
    ImGui::SameLine();
    if (ImGui::RadioButton(a_context.text("List###AssetListMode", "一覧###AssetListMode"), a_state.isAssetListVisible))
        a_state.isAssetListVisible = true;
    if (const auto *selected = find_folder(a_context.assetFolders, a_state.assetFolder))
    {
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##AssetSearch", a_context.text("Search by name", "名前で検索"),
                                 a_state.assetSearch.data(), a_state.assetSearch.size());
        const auto location = asset_location(a_context.assetFolders, *selected);
        ImGui::TextUnformatted(location.c_str());
        ImGui::Separator();
        if (!draw_assets(a_context, a_state, a_actions, *selected))
            ImGui::TextDisabled("%s", a_state.assetSearch[0] ? a_context.text("No matching names", "一致する名前がありません")
                                                           : a_context.text("This folder is empty", "フォルダーは空です"));
    }
    if (ImGui::BeginPopupContextWindow("AssetActions",
                                       ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
    {
        EditorPopupScope popup;
        if (ImGui::MenuItem(a_context.text("New Script...", "スクリプトを作成...###New Script..."), nullptr, false,
                            can_create_editor_script(a_context)))
            request_script_creation(a_state, a_actions);
        if (ImGui::MenuItem(a_context.text("Refresh", "更新###Refresh")))
            emit_editor_action(a_actions, EditorActionKind::RefreshAssets);
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
