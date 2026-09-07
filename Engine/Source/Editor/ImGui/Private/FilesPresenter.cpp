#include <Cue/Editor/ImGui/FilesPresenter.h>

#include <Cue/EditorCore/Error.h>
#include <Cue/Foundation/Assert.h>
#include <Cue/IO/Error.h>
#include <Cue/ProjectFiles/Error.h>

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <span>
#include <utility>

#include <imgui.h>

namespace
{
constexpr const char *k_dragPayload = "CUE_FILES_LOCATOR";

/// @brief ViewModel内のDirectory Locatorに対応するSnapshotを返す
[[nodiscard]] const cue::project_files::ProjectFileDirectorySnapshot *find_directory(
    const cue::editor_core::FilesViewModel &a_view, std::string_view a_locator) noexcept
{
    for (const cue::project_files::ProjectFileDirectorySnapshot &directory : a_view.directories())
    {
        if (directory.directory == a_locator)
        {
            return &directory;
        }
    }
    return nullptr;
}

/// @brief 一つのDirectoryがViewModelで展開済みか返す
[[nodiscard]] bool is_expanded(const cue::editor_core::FilesViewModel &a_view, std::string_view a_locator) noexcept
{
    return std::ranges::find(a_view.expanded_directories(), a_locator) != a_view.expanded_directories().end();
}

/// @brief 操作不能なWorkspace Entry診断を日本語表示へ変換する
[[nodiscard]] const char *rejection_message(cue::WorkspaceDiagnosticCode a_code) noexcept
{
    switch (a_code)
    {
    case cue::WorkspaceDiagnosticCode::UnsupportedName:
        return "対応していない名前です";
    case cue::WorkspaceDiagnosticCode::ReparsePoint:
        return "Reparse Pointは操作できません";
    case cue::WorkspaceDiagnosticCode::EntryDisappeared:
        return "項目が見つかりません";
    case cue::WorkspaceDiagnosticCode::PermissionDenied:
        return "アクセス権限がありません";
    case cue::WorkspaceDiagnosticCode::TypeChanged:
        return "項目の種類が変更されました";
    case cue::WorkspaceDiagnosticCode::EnumerationFailed:
        return "項目を確認できません";
    }
    return "操作できません";
}

/// @brief 安定Error分類を利用者向け日本語理由へ変換する
[[nodiscard]] const char *localized_error_message(const cue::Error &a_error) noexcept
{
    const cue::ErrorCode &code = a_error.code();
    if (code.domain() == "Cue.EditorCore")
    {
        switch (static_cast<cue::editor_core::EditorCoreError>(code.value()))
        {
        case cue::editor_core::EditorCoreError::InvalidWorkspaceRequest:
            return "入力したProject相対Pathまたは操作内容が無効です";
        case cue::editor_core::EditorCoreError::WorkspaceEntryInUse:
            return "開いているSceneまたはその親Folderは変更できません";
        case cue::editor_core::EditorCoreError::WorkspaceUnavailable:
            return "Project Filesを確認できません。更新して状態を再確認してください";
        case cue::editor_core::EditorCoreError::WorkspaceGenerationExhausted:
            return "Filesの更新回数が上限に達しました。Editorを再起動してください";
        default:
            return "Editorの現在状態ではこのFiles操作を実行できません";
        }
    }
    if (code.domain() == "Cue.ProjectFiles")
    {
        switch (static_cast<cue::project_files::ProjectFileError>(code.value()))
        {
        case cue::project_files::ProjectFileError::InvalidRequest:
            return "入力したPathまたは操作内容が無効です";
        case cue::project_files::ProjectFileError::ProtectedEntry:
            return "保護されたProject領域は変更できません";
        case cue::project_files::ProjectFileError::InUse:
            return "使用中の項目は変更できません";
        case cue::project_files::ProjectFileError::Conflict:
            return "同じ名前の項目が既に存在します";
        case cue::project_files::ProjectFileError::LimitExceeded:
            return "安全な操作上限を超えています";
        case cue::project_files::ProjectFileError::RecoveryRequired:
            return "操作結果を確定できません。Filesを更新して確認してください";
        case cue::project_files::ProjectFileError::Busy:
            return "別のFiles操作が完了するまで待ってください";
        case cue::project_files::ProjectFileError::StorageFailure:
            return "FileまたはFolderへアクセスできません";
        }
    }
    const cue::ErrorCode &root = a_error.root_code();
    if (root.domain() == "Cue.IO")
    {
        switch (static_cast<cue::IoError>(root.value()))
        {
        case cue::IoError::OutsideRoot:
            return "Project Root外は操作できません";
        case cue::IoError::NotFound:
            return "対象のFileまたはFolderが見つかりません";
        case cue::IoError::AlreadyExists:
            return "同じ名前の項目が既に存在します";
        case cue::IoError::PermissionDenied:
            return "FileまたはFolderへのアクセス権限がありません";
        case cue::IoError::CapacityExceeded:
            return "安全な操作上限を超えています";
        case cue::IoError::Busy:
            return "FileまたはFolderが使用中です";
        default:
            return "FileまたはFolderを安全に操作できません";
        }
    }
    return "Files操作を完了できません";
}
} // namespace

namespace cue::editor
{
FilesPresenter::FilesPresenter(editor_core::FilesWorkspaceService &a_service,
                               const AssertContext &a_assertContext) noexcept
    : m_service(&a_service), m_assertContext(&a_assertContext)
{
    assign_input(m_search, a_service.view_model().search_filter());
}

void FilesPresenter::draw() noexcept
{
    try
    {
        std::optional<FilesIntent> pendingIntent;
        {
            const editor_core::FilesViewModel &view = m_service->view_model();
            if (ImGui::Begin("Files"))
            {
                draw_toolbar(view, pendingIntent);
                if (view.is_stale())
                {
                    ImGui::TextColored(ImVec4(1.0F, 0.7F, 0.2F, 1.0F),
                                       "外部変更を追跡できないため再確認が必要です");
                }
                if (!m_message.empty())
                {
                    ImGui::TextColored(m_hasError ? ImVec4(1.0F, 0.35F, 0.35F, 1.0F)
                                                  : ImVec4(0.45F, 0.9F, 0.55F, 1.0F),
                                       "%s", m_message.c_str());
                }
                ImGui::BeginChild("Directories", ImVec2(230.0F, 0.0F), ImGuiChildFlags_Borders |
                                                                             ImGuiChildFlags_ResizeX);
                const bool rootOpen = ImGui::TreeNodeEx("Source Assets", ImGuiTreeNodeFlags_DefaultOpen |
                                                                             ImGuiTreeNodeFlags_OpenOnArrow);
                accept_move_drop({}, pendingIntent);
                if (rootOpen)
                {
                    draw_directory_children({}, view, pendingIntent);
                    ImGui::TreePop();
                }
                ImGui::EndChild();
                ImGui::SameLine();
                ImGui::BeginChild("Entries", ImVec2(0.0F, 0.0F), ImGuiChildFlags_Borders);
                draw_file_list(view, pendingIntent);
                ImGui::EndChild();
            }
            ImGui::End();
        }
        draw_action_dialog(pendingIntent);
        draw_delete_dialog(pendingIntent);

        if (pendingIntent.has_value())
        {
            static_cast<void>(submit(std::move(*pendingIntent)));
        }
        else
        {
            Result<bool> changed = m_service->poll_external_changes();
            if (!changed)
            {
                set_error(*changed.try_error());
            }
            else if (*changed.try_value())
            {
                m_message = "外部変更を反映しました";
                m_hasError = false;
            }
        }
    }
    catch (...)
    {
        terminate_exception();
    }
}

Result<void> FilesPresenter::submit(FilesIntent a_intent) noexcept
{
    try
    {
        switch (a_intent.kind)
        {
        case FilesIntentKind::Refresh:
        {
            Result<void> result = m_service->refresh();
            if (!result)
            {
                set_error(*result.try_error());
                return result;
            }
            m_message = "Filesを更新しました";
            m_hasError = false;
            return Result<void>::success();
        }
        case FilesIntentKind::SetSearchFilter:
        {
            Result<void> result = m_service->set_search_filter(a_intent.destination);
            if (!result)
            {
                set_error(*result.try_error());
                return result;
            }
            m_message.clear();
            m_hasError = false;
            return Result<void>::success();
        }
        case FilesIntentKind::ClearSelection:
        {
            Result<void> result = m_service->clear_selection();
            if (!result)
            {
                set_error(*result.try_error());
            }
            else
            {
                m_message.clear();
                m_hasError = false;
            }
            return result;
        }
        case FilesIntentKind::SetExpanded:
        case FilesIntentKind::Select:
            break;
        default:
            break;
        }

        if (a_intent.kind == FilesIntentKind::SetExpanded || a_intent.kind == FilesIntentKind::Select)
        {
            Result<void> result = a_intent.kind == FilesIntentKind::SetExpanded
                                      ? m_service->set_expanded(a_intent.source, a_intent.isExpanded)
                                      : m_service->select(a_intent.source);
            if (!result)
            {
                set_error(*result.try_error());
            }
            else
            {
                m_message.clear();
                m_hasError = false;
            }
            return result;
        }
        if (a_intent.kind == FilesIntentKind::NavigateDirectory)
        {
            Result<void> expanded = m_service->set_expanded(a_intent.source, true);
            if (!expanded)
            {
                set_error(*expanded.try_error());
                return expanded;
            }
            Result<void> selected = m_service->select(a_intent.source);
            if (!selected)
            {
                set_error(*selected.try_error());
                return selected;
            }
            m_currentDirectory = std::move(a_intent.source);
            m_message.clear();
            m_hasError = false;
            return Result<void>::success();
        }
        if (a_intent.kind == FilesIntentKind::PrepareDelete)
        {
            Result<project_files::ProjectFileDeletePreview> preview = m_service->preview_delete(a_intent.source);
            if (!preview)
            {
                set_error(*preview.try_error());
                return Result<void>::failure(std::move(*preview.try_error()));
            }
            m_deleteTarget.emplace(std::move(a_intent.source));
            m_deleteEntryCount = preview.try_value()->descendantCount + 1U;
            m_deleteByteSize = preview.try_value()->byteSize;
            m_openDeleteDialog = true;
            return Result<void>::success();
        }

        Result<project_files::ProjectFileOperationOutcome> operation =
            Result<project_files::ProjectFileOperationOutcome>::failure(
                Error::create(m_assertContext->fatal_handler(),
                              ErrorCode::create(m_assertContext->fatal_handler(), "Cue.Editor.ImGui", 1),
                              "Files intent is not supported"));
        switch (a_intent.kind)
        {
        case FilesIntentKind::CreateFolder:
            operation = m_service->create_directory(a_intent.destination);
            break;
        case FilesIntentKind::CreateEmptyFile:
            operation = m_service->create_file(a_intent.destination, std::span<const std::byte>{});
            break;
        case FilesIntentKind::Rename:
            operation = m_service->rename(a_intent.source, a_intent.destination);
            break;
        case FilesIntentKind::Move:
            operation = m_service->move(a_intent.source, a_intent.destination);
            break;
        case FilesIntentKind::Copy:
            operation = m_service->copy(a_intent.source, a_intent.destination);
            break;
        case FilesIntentKind::Delete:
            operation = m_service->delete_entry(a_intent.source);
            break;
        case FilesIntentKind::Restore:
            operation = m_service->restore(a_intent.source);
            break;
        default:
            break;
        }
        if (!operation)
        {
            set_error(*operation.try_error());
            return Result<void>::failure(std::move(*operation.try_error()));
        }
        set_operation_status(a_intent.kind, *operation.try_value());
        return Result<void>::success();
    }
    catch (...)
    {
        terminate_exception();
    }
}

bool FilesPresenter::is_delete_confirmation_pending() const noexcept
{
    return m_deleteTarget.has_value();
}

std::size_t FilesPresenter::delete_confirmation_entry_count() const noexcept
{
    return m_deleteEntryCount;
}

std::uint64_t FilesPresenter::delete_confirmation_byte_size() const noexcept
{
    return m_deleteByteSize;
}

std::string_view FilesPresenter::message() const noexcept
{
    return m_message;
}

bool FilesPresenter::has_error_message() const noexcept
{
    return m_hasError;
}

void FilesPresenter::draw_directory_children(std::string_view a_parent, const editor_core::FilesViewModel &a_view,
                                             std::optional<FilesIntent> &a_pendingIntent)
{
    const project_files::ProjectFileDirectorySnapshot *snapshot = find_directory(a_view, a_parent);
    if (snapshot == nullptr)
    {
        return;
    }
    for (const project_files::ProjectFileEntry &entry : snapshot->entries)
    {
        if (entry.type != WorkspaceEntryType::Directory || !entry.is_operable())
        {
            continue;
        }
        const bool expanded = is_expanded(a_view, entry.locator);
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
        if (expanded)
        {
            ImGui::SetNextItemOpen(true, ImGuiCond_Always);
        }
        if (a_view.selection() == entry.locator)
        {
            flags |= ImGuiTreeNodeFlags_Selected;
        }
        const bool opened = ImGui::TreeNodeEx(entry.locator.c_str(), flags, "%s", entry.displayName.c_str());
        if (ImGui::IsItemToggledOpen() && !a_pendingIntent.has_value())
        {
            a_pendingIntent = FilesIntent{FilesIntentKind::SetExpanded, entry.locator, {}, opened};
        }
        else if (ImGui::IsItemClicked() && !a_pendingIntent.has_value())
        {
            m_currentDirectory = entry.locator;
            a_pendingIntent = FilesIntent{FilesIntentKind::Select, entry.locator};
        }
        if (ImGui::BeginDragDropSource())
        {
            ImGui::SetDragDropPayload(k_dragPayload, entry.locator.c_str(), entry.locator.size() + 1U);
            ImGui::TextUnformatted(entry.displayName.c_str());
            ImGui::EndDragDropSource();
        }
        accept_move_drop(entry.locator, a_pendingIntent);
        if (opened)
        {
            draw_directory_children(entry.locator, a_view, a_pendingIntent);
            ImGui::TreePop();
        }
    }
}

void FilesPresenter::draw_file_list(const editor_core::FilesViewModel &a_view,
                                    std::optional<FilesIntent> &a_pendingIntent)
{
    const std::span<const project_files::ProjectFileEntry> entries =
        a_view.try_search_result() != nullptr
            ? std::span<const project_files::ProjectFileEntry>(a_view.try_search_result()->entries)
            : (find_directory(a_view, m_currentDirectory) != nullptr
                   ? std::span<const project_files::ProjectFileEntry>(
                         find_directory(a_view, m_currentDirectory)->entries)
                   : std::span<const project_files::ProjectFileEntry>{});
    for (const project_files::ProjectFileEntry &entry : entries)
    {
        const bool selected = a_view.selection() == entry.locator;
        const char *prefix = entry.type == WorkspaceEntryType::Directory ? "[Folder]" : "[File]";
        if (ImGui::Selectable((prefix + std::string(" ") + entry.displayName + "##" + entry.locator).c_str(),
                              selected) &&
            entry.is_operable() && !a_pendingIntent.has_value())
        {
            if (entry.type == WorkspaceEntryType::Directory && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            {
                a_pendingIntent = FilesIntent{FilesIntentKind::NavigateDirectory, entry.locator};
            }
            else
            {
                a_pendingIntent = FilesIntent{FilesIntentKind::Select, entry.locator};
            }
        }
        if (!entry.is_operable())
        {
            ImGui::SameLine();
            ImGui::TextDisabled("操作対象外: %s", entry.rejection.has_value()
                                                     ? rejection_message(*entry.rejection)
                                                     : "理由を確認できません");
        }
        if (entry.is_operable() && ImGui::BeginDragDropSource())
        {
            ImGui::SetDragDropPayload(k_dragPayload, entry.locator.c_str(), entry.locator.size() + 1U);
            ImGui::TextUnformatted(entry.displayName.c_str());
            ImGui::EndDragDropSource();
        }
        if (entry.type == WorkspaceEntryType::Directory && entry.is_operable())
        {
            accept_move_drop(entry.locator, a_pendingIntent);
        }
    }

    ImGui::Separator();
    ImGui::TextUnformatted("Trashから復元");
    for (const project_files::RecoveryEntry &entry : a_view.recovery_entries())
    {
        ImGui::PushID(entry.operationId.c_str());
        ImGui::TextUnformatted(entry.originalPath.c_str());
        ImGui::SameLine();
        const bool canRestore = entry.state == project_files::RecoveryEntryState::Recoverable;
        ImGui::BeginDisabled(!canRestore);
        if (ImGui::Button("復元") && !a_pendingIntent.has_value())
        {
            a_pendingIntent = FilesIntent{FilesIntentKind::Restore, entry.operationId};
        }
        ImGui::EndDisabled();
        if (!canRestore)
        {
            ImGui::SameLine();
            ImGui::TextDisabled("手動照合が必要です");
        }
        ImGui::PopID();
    }
}

void FilesPresenter::draw_action_dialog(std::optional<FilesIntent> &a_pendingIntent)
{
    if (m_openActionDialog)
    {
        ImGui::OpenPopup(dialog_title());
        m_openActionDialog = false;
    }
    bool isOpen = m_dialogMode != DialogMode::None;
    if (!isOpen || !ImGui::BeginPopupModal(dialog_title(), &isOpen, ImGuiWindowFlags_AlwaysAutoResize))
    {
        if (!isOpen)
        {
            m_dialogMode = DialogMode::None;
        }
        return;
    }
    if (!isOpen)
    {
        m_dialogMode = DialogMode::None;
        ImGui::EndPopup();
        return;
    }
    if (m_dialogMode == DialogMode::Rename || m_dialogMode == DialogMode::Move || m_dialogMode == DialogMode::Copy)
    {
        ImGui::InputText("元", m_source.data(), m_source.size(), ImGuiInputTextFlags_ReadOnly);
    }
    ImGui::InputText("先", m_destination.data(), m_destination.size());
    if (ImGui::Button("実行") && !a_pendingIntent.has_value())
    {
        FilesIntentKind kind = FilesIntentKind::CreateFolder;
        switch (m_dialogMode)
        {
        case DialogMode::CreateEmptyFile:
            kind = FilesIntentKind::CreateEmptyFile;
            break;
        case DialogMode::Rename:
            kind = FilesIntentKind::Rename;
            break;
        case DialogMode::Move:
            kind = FilesIntentKind::Move;
            break;
        case DialogMode::Copy:
            kind = FilesIntentKind::Copy;
            break;
        default:
            break;
        }
        a_pendingIntent = FilesIntent{kind, m_source.data(), m_destination.data()};
        m_dialogMode = DialogMode::None;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("キャンセル") || ImGui::IsKeyPressed(ImGuiKey_Escape))
    {
        m_dialogMode = DialogMode::None;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void FilesPresenter::draw_delete_dialog(std::optional<FilesIntent> &a_pendingIntent)
{
    if (m_openDeleteDialog && m_deleteTarget.has_value())
    {
        ImGui::OpenPopup("復元可能な削除");
        m_openDeleteDialog = false;
    }
    bool isOpen = m_deleteTarget.has_value();
    if (!isOpen || !ImGui::BeginPopupModal("復元可能な削除", &isOpen, ImGuiWindowFlags_AlwaysAutoResize))
    {
        if (!isOpen)
        {
            m_deleteTarget.reset();
        }
        return;
    }
    if (!isOpen)
    {
        m_deleteTarget.reset();
        ImGui::EndPopup();
        return;
    }
    ImGui::TextWrapped("%s をProject内Trashへ移動します。永久削除は行いません。", m_deleteTarget->c_str());
    ImGui::Text("対象Entry数: %zu", m_deleteEntryCount);
    ImGui::Text("概算Size: %llu byte", static_cast<unsigned long long>(m_deleteByteSize));
    if (ImGui::Button("Trashへ移動") && !a_pendingIntent.has_value())
    {
        a_pendingIntent = FilesIntent{FilesIntentKind::Delete, *m_deleteTarget};
        m_deleteTarget.reset();
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("キャンセル") || ImGui::IsKeyPressed(ImGuiKey_Escape))
    {
        m_deleteTarget.reset();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void FilesPresenter::draw_toolbar(const editor_core::FilesViewModel &a_view,
                                  std::optional<FilesIntent> &a_pendingIntent)
{
    if (m_focusSearch)
    {
        ImGui::SetKeyboardFocusHere();
        m_focusSearch = false;
    }
    if (ImGui::InputTextWithHint("##FilesSearch", "検索", m_search.data(), m_search.size()) &&
        !a_pendingIntent.has_value())
    {
        a_pendingIntent = FilesIntent{FilesIntentKind::SetSearchFilter, {}, m_search.data()};
    }
    ImGui::SameLine();
    if (ImGui::Button("更新") && !a_pendingIntent.has_value())
    {
        a_pendingIntent = FilesIntent{FilesIntentKind::Refresh};
    }
    ImGui::SameLine();
    if (ImGui::Button("新規フォルダー"))
    {
        m_dialogMode = DialogMode::CreateFolder;
        assign_input(m_source, {});
        assign_input(m_destination, compose_destination(m_currentDirectory, "NewFolder"));
        m_openActionDialog = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("新規ファイル"))
    {
        m_dialogMode = DialogMode::CreateEmptyFile;
        assign_input(m_source, {});
        assign_input(m_destination, compose_destination(m_currentDirectory, "NewFile.txt"));
        m_openActionDialog = true;
    }

    const std::optional<std::string_view> selection = a_view.selection();
    const bool hasSelection = selection.has_value();
    ImGui::BeginDisabled(!hasSelection);
    for (const auto &[label, mode] : std::array<std::pair<const char *, DialogMode>, 3U>{
             std::pair{"名前変更", DialogMode::Rename}, {"移動", DialogMode::Move}, {"コピー", DialogMode::Copy}})
    {
        ImGui::SameLine();
        if (ImGui::Button(label) && selection.has_value())
        {
            m_dialogMode = mode;
            assign_input(m_source, *selection);
            assign_input(m_destination, *selection);
            m_openActionDialog = true;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("削除") && selection.has_value() && !a_pendingIntent.has_value())
    {
        a_pendingIntent = FilesIntent{FilesIntentKind::PrepareDelete, std::string(*selection)};
    }
    ImGui::EndDisabled();

    const bool canUseShortcut =
        ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput &&
        !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
    if (canUseShortcut && ImGui::IsKeyPressed(ImGuiKey_F5) && !a_pendingIntent.has_value())
    {
        a_pendingIntent = FilesIntent{FilesIntentKind::Refresh};
    }
    if (canUseShortcut && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_F))
    {
        m_focusSearch = true;
    }
    if (canUseShortcut && ImGui::IsKeyPressed(ImGuiKey_Delete) && selection.has_value() &&
        !a_pendingIntent.has_value())
    {
        a_pendingIntent = FilesIntent{FilesIntentKind::PrepareDelete, std::string(*selection)};
    }
}

void FilesPresenter::accept_move_drop(std::string_view a_destinationDirectory,
                                      std::optional<FilesIntent> &a_pendingIntent)
{
    if (!ImGui::BeginDragDropTarget())
    {
        return;
    }
    if (const ImGuiPayload *payload = ImGui::AcceptDragDropPayload(k_dragPayload);
        payload != nullptr && payload->Data != nullptr && payload->DataSize > 1 && !a_pendingIntent.has_value())
    {
        const std::string_view source(static_cast<const char *>(payload->Data),
                                      static_cast<std::size_t>(payload->DataSize - 1));
        a_pendingIntent = FilesIntent{FilesIntentKind::Move, std::string(source),
                                     compose_destination(a_destinationDirectory, filename(source))};
    }
    ImGui::EndDragDropTarget();
}

std::string_view FilesPresenter::filename(std::string_view a_locator) noexcept
{
    const std::size_t separator = a_locator.find_last_of('/');
    return separator == std::string_view::npos ? a_locator : a_locator.substr(separator + 1U);
}

std::string FilesPresenter::compose_destination(std::string_view a_directory, std::string_view a_name)
{
    if (a_directory.empty())
    {
        return std::string(a_name);
    }
    return std::string(a_directory) + "/" + std::string(a_name);
}

void FilesPresenter::assign_input(std::array<char, 256U> &a_input, std::string_view a_value) noexcept
{
    a_input.fill('\0');
    const std::size_t size = std::min(a_value.size(), a_input.size() - 1U);
    std::copy_n(a_value.data(), size, a_input.data());
}

const char *FilesPresenter::dialog_title() const noexcept
{
    switch (m_dialogMode)
    {
    case DialogMode::CreateFolder:
        return "フォルダーを作成";
    case DialogMode::CreateEmptyFile:
        return "ファイルを作成";
    case DialogMode::Rename:
        return "名前を変更";
    case DialogMode::Move:
        return "移動";
    case DialogMode::Copy:
        return "コピー";
    case DialogMode::None:
        return "Files操作";
    }
    return "Files操作";
}

void FilesPresenter::set_error(const Error &a_error)
{
    m_message = "Files操作に失敗しました: ";
    m_message += localized_error_message(a_error);
    m_hasError = true;
}

void FilesPresenter::set_operation_status(FilesIntentKind a_kind,
                                          project_files::ProjectFileOperationOutcome a_outcome)
{
    if (a_outcome == project_files::ProjectFileOperationOutcome::NotCommitted)
    {
        if (m_service->view_model().try_error() != nullptr)
        {
            set_error(*m_service->view_model().try_error());
        }
        else
        {
            m_message = "Files操作は適用されませんでした";
            m_hasError = true;
        }
        return;
    }
    m_hasError = a_outcome != project_files::ProjectFileOperationOutcome::Committed;
    if (a_outcome == project_files::ProjectFileOperationOutcome::CommittedButDurabilityUnknown)
    {
        m_message = "Files操作は反映されましたが、永続化を確認できませんでした";
        return;
    }
    if (a_outcome == project_files::ProjectFileOperationOutcome::ReconciliationRequired)
    {
        m_message = "Files操作の状態を確定できないため、再確認が必要です";
        return;
    }
    switch (a_kind)
    {
    case FilesIntentKind::CreateFolder:
        m_message = "フォルダーを作成しました";
        break;
    case FilesIntentKind::CreateEmptyFile:
        m_message = "ファイルを作成しました";
        break;
    case FilesIntentKind::Rename:
        m_message = "名前を変更しました";
        break;
    case FilesIntentKind::Move:
        m_message = "移動しました";
        break;
    case FilesIntentKind::Copy:
        m_message = "コピーしました";
        break;
    case FilesIntentKind::Delete:
        m_message = "Project内Trashへ移動しました";
        break;
    case FilesIntentKind::Restore:
        m_message = "Trashから復元しました";
        break;
    default:
        m_message = "Files操作が完了しました";
        break;
    }
}

[[noreturn]] void FilesPresenter::terminate_exception() const noexcept
{
    m_assertContext->fatal_handler().terminate("Files presenter operation failed unexpectedly");
    std::abort();
}
} // namespace cue::editor
