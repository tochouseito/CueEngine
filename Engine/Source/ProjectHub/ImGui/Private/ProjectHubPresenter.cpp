#include <Cue/ProjectHub/ImGui/ProjectHubPresenter.h>

#include <Cue/Foundation/Assert.h>
#include <Cue/IO/Error.h>
#include <Cue/Project/Error.h>
#include <Cue/ProjectHub/Error.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <imgui.h>

namespace
{
/// @brief Allocation失敗をProject Hub PresentationのFatal境界へ渡す
[[noreturn]] void terminate_allocation(const cue::AssertContext &a_context) noexcept
{
    a_context.fatal_handler().terminate("Project Hub Presentation allocation failed");
    std::terminate();
}

/// @brief 現在時刻をRecent Registry用Unix Millisecondsへ変換する
[[nodiscard]] std::uint64_t current_milliseconds() noexcept
{
    const auto duration = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(duration).count());
}

/// @brief Error ChainのRootが公開後のDurability確認失敗か返す
[[nodiscard]] bool is_durability_unknown(const cue::Error &a_error) noexcept
{
    const cue::ErrorCode &rootCode = a_error.root_code();
    return rootCode.domain() == "Cue.IO" &&
           rootCode.value() == static_cast<std::int64_t>(cue::IoError::DurabilityUnknown);
}

/// @brief Open拒否後の一覧公開部分成功から元の拒否Categoryを日本語理由へ変換する
[[nodiscard]] const char *open_rejection_text(const cue::Error &a_error) noexcept
{
    const std::span<const cue::ErrorCause> causes = a_error.causes();
    if (causes.empty())
    {
        return "Projectを開けませんでした。";
    }
    const cue::ErrorCode &code = causes.front().code();
    if (code.domain() == "Cue.ProjectHub")
    {
        switch (static_cast<cue::project_hub::ProjectHubError>(code.value()))
        {
        case cue::project_hub::ProjectHubError::ProjectMissing:
            return "Project Folderが見つからないため開けませんでした。";
        case cue::project_hub::ProjectHubError::ProjectBroken:
            return "Project情報を読み取れないため開けませんでした。";
        case cue::project_hub::ProjectHubError::ProjectIdentityMismatch:
            return "Projectの識別情報が一覧と一致しないため開けませんでした。";
        case cue::project_hub::ProjectHubError::ProjectUnsupported:
            return "このVersionのCueEngineでは対応できないためProjectを開けませんでした。";
        default:
            return "Projectを開けませんでした。";
        }
    }
    if (code.domain() == "Cue.Project")
    {
        return "Project情報または互換性情報が正しくないため開けませんでした。";
    }
    return "Projectを開けませんでした。";
}

/// @brief Entry Stateを日本語表示へ変換する
[[nodiscard]] const char *entry_state_text(cue::project_hub::ProjectEntryState a_state) noexcept
{
    switch (a_state)
    {
    case cue::project_hub::ProjectEntryState::Available:
        return "利用可能";
    case cue::project_hub::ProjectEntryState::Missing:
        return "見つかりません";
    case cue::project_hub::ProjectEntryState::Moved:
        return "移動を確認してください";
    case cue::project_hub::ProjectEntryState::Broken:
        return "Projectが壊れています";
    }
    return "状態不明";
}

/// @brief Broken Entryの安定分類を行内の日本語診断へ変換する
[[nodiscard]] const char *entry_problem_text(cue::project_hub::ProjectEntryProblem a_problem) noexcept
{
    switch (a_problem)
    {
    case cue::project_hub::ProjectEntryProblem::None:
        return nullptr;
    case cue::project_hub::ProjectEntryProblem::LocatorAccessFailed:
        return "Folderへアクセスできません";
    case cue::project_hub::ProjectEntryProblem::DescriptorInvalid:
        return "CueProject.jsonを読み取れません";
    case cue::project_hub::ProjectEntryProblem::IdentityMismatch:
        return "Project IDが一覧と一致しません";
    case cue::project_hub::ProjectEntryProblem::CompatibilityInvalid:
        return "互換性情報を評価できません";
    }
    return "Projectの診断状態が不明です";
}

/// @brief Broken Entryの安定分類をUserが実行できる回復手順へ変換する
[[nodiscard]] const char *entry_problem_recovery_text(cue::project_hub::ProjectEntryProblem a_problem) noexcept
{
    switch (a_problem)
    {
    case cue::project_hub::ProjectEntryProblem::None:
        return nullptr;
    case cue::project_hub::ProjectEntryProblem::LocatorAccessFailed:
        return "Folderの存在、アクセス権、File／Directory種別、Reparse Pointを確認してください。";
    case cue::project_hub::ProjectEntryProblem::DescriptorInvalid:
        return "Project Folder内のCueProject.jsonが存在し、正しいJSONと必須項目を保持しているか確認してください。";
    case cue::project_hub::ProjectEntryProblem::IdentityMismatch:
        return "同一Projectの移動先を登録する場合は、このEntryを残したまま移動先Folderを指定し、再関連付けを有効にしてください。"
               "現在のFolder内Projectを別Projectとして登録する場合だけ、このEntryを一覧から除外して通常登録してください。";
    case cue::project_hub::ProjectEntryProblem::CompatibilityInvalid:
        return "CueProject.jsonのFormat VersionとEngine互換性情報を確認し、対応するCueEngineで開いてください。";
    }
    return "Project FolderとCueProject.jsonを確認してください。";
}

/// @brief Compatibility Statusを日本語表示へ変換する
[[nodiscard]] const char *compatibility_text(cue::ProjectCompatibilityStatus a_status) noexcept
{
    switch (a_status)
    {
    case cue::ProjectCompatibilityStatus::Compatible:
        return "互換";
    case cue::ProjectCompatibilityStatus::Degraded:
        return "制限付き互換";
    case cue::ProjectCompatibilityStatus::Unsupported:
        return "非対応";
    case cue::ProjectCompatibilityStatus::Unknown:
        return "互換性不明";
    }
    return "互換性不明";
}

/// @brief Project互換性の安定理由を原因と回復手順を含む日本語表示へ変換する
[[nodiscard]] const char *compatibility_reason_text(cue::ProjectCompatibilityReasonCode a_reason) noexcept
{
    switch (a_reason)
    {
    case cue::ProjectCompatibilityReasonCode::UnsupportedProjectFormat:
        return "Project Formatが非対応です。対応するCueEngineで開くか、正式なMigrationを使用してください。";
    case cue::ProjectCompatibilityReasonCode::EngineVersionTooOld:
        return "現在のCueEngineがProjectの要求Versionより古いため、CueEngineを更新してください。";
    case cue::ProjectCompatibilityReasonCode::EngineVersionTooNew:
        return "現在のCueEngineがProjectの対応範囲より新しいため、対応する旧Versionを使用してください。";
    case cue::ProjectCompatibilityReasonCode::CapabilityNotObserved:
        return "必要なHardware機能を観測できません。DeviceとDriverの状態を確認してください。";
    case cue::ProjectCompatibilityReasonCode::CapabilityNotQueried:
        return "必要なHardware機能を未確認です。診断を再実行してから一覧を更新してください。";
    case cue::ProjectCompatibilityReasonCode::CapabilityQueryFailed:
        return "Hardware機能の取得に失敗しました。DriverとLogを確認してから再試行してください。";
    case cue::ProjectCompatibilityReasonCode::HardwareUnsupported:
        return "現在のHardwareは必要機能に対応していません。対応Hardwareを使用してください。";
    case cue::ProjectCompatibilityReasonCode::EngineNotImplemented:
        return "必要機能を現在のCueEngineが実装していません。対応Versionへ更新してください。";
    case cue::ProjectCompatibilityReasonCode::CapabilityVersionUnknown:
        return "必要機能のVersionを確認できません。OSとDriverを更新して再確認してください。";
    case cue::ProjectCompatibilityReasonCode::CapabilityVersionTooLow:
        return "必要機能のVersionが不足しています。対応するDriverまたはHardwareを使用してください。";
    case cue::ProjectCompatibilityReasonCode::RuntimeDisabled:
        return "必要機能がRuntime設定で無効です。ProjectとRuntime設定を確認してください。";
    }
    return "互換性を判断できません。Project情報とEngine診断を確認してください。";
}

/// @brief Project CapabilityをUserがHardware／Driver項目と照合できる名前へ変換する
[[nodiscard]] const char *capability_text(cue::ProjectCapability a_capability) noexcept
{
    switch (a_capability)
    {
    case cue::ProjectCapability::Baseline3D:
        return "Baseline 3D";
    case cue::ProjectCapability::WaveOperations:
        return "Wave Operations";
    case cue::ProjectCapability::EnhancedBarriers:
        return "Enhanced Barriers";
    case cue::ProjectCapability::RayTracing:
        return "Ray Tracing";
    case cue::ProjectCapability::MeshShader:
        return "Mesh Shader";
    case cue::ProjectCapability::VariableRateShading:
        return "Variable Rate Shading";
    case cue::ProjectCapability::SamplerFeedback:
        return "Sampler Feedback";
    }
    return "Unknown Capability";
}

/// @brief Capability要件の必須／推奨区分を日本語表示へ変換する
[[nodiscard]] const char *requirement_kind_text(cue::CapabilityRequirementKind a_kind) noexcept
{
    return a_kind == cue::CapabilityRequirementKind::Required ? "必須" : "推奨";
}

/// @brief 固定Bufferへ初期文字列をNUL終端で設定する
template <std::size_t Size> void set_buffer(std::array<char, Size> &a_buffer, std::string_view a_text) noexcept
{
    const std::size_t length = std::min(a_text.size(), Size - 1);
    std::memcpy(a_buffer.data(), a_text.data(), length);
    a_buffer[length] = '\0';
}

/// @brief ImGuiのResize Callbackへ可変長文字列とFatal境界を渡す
struct StringInputContext final
{
    std::string *value;
    const cue::AssertContext *assertContext;
};

/// @brief FontにないUnicode ScalarをEscapeした識別可能な表示文字列へ変換する
[[nodiscard]] std::string make_renderable_text(std::string_view a_text, bool *a_usedUnicodeEscape = nullptr);
/// @brief 入力値を変更せずFont非対応Unicode ScalarのEscape Previewだけを併記する
void draw_unicode_escape_preview(std::string_view a_text, const cue::AssertContext &a_context) noexcept;

/// @brief ImGuiが要求した容量へProject Locator文字列を拡張する
int resize_string_input(ImGuiInputTextCallbackData *a_data) noexcept
{
    if (a_data->EventFlag != ImGuiInputTextFlags_CallbackResize || a_data->UserData == nullptr)
    {
        return 0;
    }
    StringInputContext &context = *static_cast<StringInputContext *>(a_data->UserData);
    try
    {
        context.value->resize(static_cast<std::size_t>(a_data->BufTextLen));
    }
    catch (...)
    {
        terminate_allocation(*context.assertContext);
    }
    a_data->Buf = context.value->data();
    return 0;
}

/// @brief Windows上限まで拡張できるProject Locator入力を描画する
bool input_locator(const char *a_label, std::string &a_value, const cue::AssertContext &a_context) noexcept
{
    StringInputContext context{&a_value, &a_context};
    const bool changed = ImGui::InputText(a_label, a_value.data(), a_value.capacity() + 1U,
                                          ImGuiInputTextFlags_CallbackResize, resize_string_input, &context);
    draw_unicode_escape_preview(a_value, a_context);
    return changed;
}

/// @brief FontにないUnicode Scalarと表示名中のEscape開始文字を衝突しないASCII表記へ変換する
[[nodiscard]] std::string make_renderable_text(std::string_view a_text, bool *a_usedUnicodeEscape)
{
    std::string rendered;
    rendered.reserve(a_text.size());
    const char *cursor = a_text.data();
    const char *const end = cursor + a_text.size();
    ImFont *font = ImGui::GetFont();
    constexpr char k_hexDigits[] = "0123456789ABCDEF";
    while (cursor < end)
    {
        const auto first = static_cast<unsigned char>(*cursor);
        unsigned int scalar = first;
        int byteCount = 1;
        if ((first & 0xE0U) == 0xC0U && end - cursor >= 2)
        {
            scalar = static_cast<unsigned int>(first & 0x1FU) << 6U;
            scalar |= static_cast<unsigned char>(cursor[1]) & 0x3FU;
            byteCount = 2;
        }
        else if ((first & 0xF0U) == 0xE0U && end - cursor >= 3)
        {
            scalar = static_cast<unsigned int>(first & 0x0FU) << 12U;
            scalar |= static_cast<unsigned int>(static_cast<unsigned char>(cursor[1]) & 0x3FU) << 6U;
            scalar |= static_cast<unsigned char>(cursor[2]) & 0x3FU;
            byteCount = 3;
        }
        else if ((first & 0xF8U) == 0xF0U && end - cursor >= 4)
        {
            scalar = static_cast<unsigned int>(first & 0x07U) << 18U;
            scalar |= static_cast<unsigned int>(static_cast<unsigned char>(cursor[1]) & 0x3FU) << 12U;
            scalar |= static_cast<unsigned int>(static_cast<unsigned char>(cursor[2]) & 0x3FU) << 6U;
            scalar |= static_cast<unsigned char>(cursor[3]) & 0x3FU;
            byteCount = 4;
        }
        else if (first >= 0x80U)
        {
            scalar = 0xFFFDU;
        }
        if (byteCount == 1 && scalar == static_cast<unsigned int>('\\'))
        {
            rendered.append("\\\\");
            ++cursor;
            continue;
        }
        const bool isRepresentable = scalar <= static_cast<unsigned int>((std::numeric_limits<ImWchar>::max)());
        if (font != nullptr && isRepresentable && font->IsGlyphInFont(static_cast<ImWchar>(scalar)))
        {
            rendered.append(cursor, static_cast<std::size_t>(byteCount));
            cursor += byteCount;
            continue;
        }

        if (a_usedUnicodeEscape != nullptr)
        {
            *a_usedUnicodeEscape = true;
        }
        rendered.append("\\u{");
        int highestShift = 12;
        while (highestShift < 20 && scalar >= (1U << (highestShift + 4)))
        {
            highestShift += 4;
        }
        for (int shift = highestShift; shift >= 0; shift -= 4)
        {
            rendered.push_back(k_hexDigits[(scalar >> shift) & 0x0FU]);
        }
        rendered.push_back('}');
        cursor += byteCount;
    }
    return rendered;
}

void draw_unicode_escape_preview(std::string_view a_text, const cue::AssertContext &a_context) noexcept
{
    try
    {
        bool usedUnicodeEscape = false;
        const std::string renderedText = make_renderable_text(a_text, &usedUnicodeEscape);
        if (usedUnicodeEscape)
        {
            ImGui::TextDisabled("識別表示（Font非対応文字はUnicode Escape）");
            ImGui::TextWrapped("%s", renderedText.c_str());
        }
    }
    catch (...)
    {
        terminate_allocation(a_context);
    }
}
} // namespace

namespace cue::project_hub
{
ProjectHubPresenter::ProjectHubPresenter(ProjectHubService &a_service, const AssertContext &a_assertContext) noexcept
    : m_service(&a_service), m_assertContext(&a_assertContext)
{
    set_buffer(m_projectName, "NewGame");
    set_buffer(m_displayName, "新しいゲーム");
    if (!m_service->templates().empty())
    {
        try
        {
            m_selectedTemplateId = m_service->templates()[0].id;
        }
        catch (...)
        {
            terminate_allocation(a_assertContext);
        }
    }
}

Result<std::unique_ptr<ProjectHubPresenter>> ProjectHubPresenter::create(ProjectHubService &a_service,
                                                                         const AssertContext &a_assertContext) noexcept
{
    try
    {
        std::unique_ptr<ProjectHubPresenter> presenter(new ProjectHubPresenter(a_service, a_assertContext));
        return Result<std::unique_ptr<ProjectHubPresenter>>::success(std::move(presenter));
    }
    catch (...)
    {
        terminate_allocation(a_assertContext);
    }
}

void ProjectHubPresenter::draw(bool a_canLaunchEditor) noexcept
{
    if (ImGui::IsKeyPressed(ImGuiKey_Escape) && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId))
    {
        m_isExitRequested = true;
    }

    ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    constexpr ImGuiWindowFlags k_windowFlags =
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize;
    if (ImGui::Begin("CueEngine Project Hub", nullptr, k_windowFlags))
    {
        const bool canUseGlobalShortcuts =
            ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
            !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
        const bool createShortcut =
            canUseGlobalShortcuts && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_N);
        const bool registerShortcut =
            canUseGlobalShortcuts && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_O);
        if (ImGui::Button("新しいProject (Ctrl+N)") || createShortcut)
        {
            m_openCreateDialog = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("既存Projectを登録 (Ctrl+O)") || registerShortcut)
        {
            m_confirmMovedProject = false;
            m_openRegisterDialog = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("一覧を更新"))
        {
            Result<void> refreshed = m_service->refresh();
            if (refreshed)
            {
                set_status("Project一覧を更新しました。");
            }
            else
            {
                const Error &error = *refreshed.try_error();
                if (is_durability_unknown(error))
                {
                    set_warning("Project一覧の状態を更新しましたが、Diskへの永続化を確認できませんでした。"
                                "現在の一覧を確認し、次回起動後にも同じ状態が保持されているか再確認してください。");
                }
                else
                {
                    set_error(error);
                }
            }
        }
        ImGui::Separator();
        draw_project_list(a_canLaunchEditor);
        if (!m_message.empty())
        {
            ImGui::Separator();
            const ImVec4 color = m_hasError     ? ImVec4(1.0F, 0.35F, 0.35F, 1.0F)
                                 : m_hasWarning ? ImVec4(1.0F, 0.75F, 0.25F, 1.0F)
                                                : ImVec4(0.45F, 0.9F, 0.55F, 1.0F);
            ImGui::PushStyleColor(ImGuiCol_Text, color);
            ImGui::TextWrapped("%s", m_message.c_str());
            ImGui::PopStyleColor();
        }
    }
    ImGui::End();

    draw_create_dialog();
    draw_register_dialog();
    draw_remove_dialog();
    draw_migrate_dialog();
}

void ProjectHubPresenter::draw_project_list(bool a_canLaunchEditor) noexcept
{
    std::string openProjectId;
    std::string pinProjectId;
    bool pinValue = false;
    try
    {
        if (ImGui::BeginChild("RecentProjects", ImVec2(0.0F, -92.0F), ImGuiChildFlags_Borders))
        {
            for (const ProjectRowView &project : m_service->projects())
            {
                ImGui::PushID(project.projectId.c_str());
                const bool isSelected = m_selectedProjectId == project.projectId;
                if (ImGui::Selectable("##ProjectRow", isSelected, ImGuiSelectableFlags_AllowDoubleClick))
                {
                    m_selectedProjectId = project.projectId;
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && project.canOpen && a_canLaunchEditor)
                    {
                        openProjectId = project.projectId;
                    }
                }
                ImDrawList *drawList = ImGui::GetWindowDrawList();
                const ImVec2 rowMinimum = ImGui::GetItemRectMin();
                const ImVec2 rowMaximum = ImGui::GetItemRectMax();
                const float textOffset = (rowMaximum.y - rowMinimum.y - ImGui::GetTextLineHeight()) * 0.5F;
                const std::string renderedDisplayName = make_renderable_text(project.displayName);
                drawList->PushClipRect(rowMinimum, ImVec2(rowMinimum.x + 250.0F, rowMaximum.y), true);
                drawList->AddText(ImVec2(rowMinimum.x + ImGui::GetStyle().FramePadding.x, rowMinimum.y + textOffset),
                                  ImGui::GetColorU32(ImGuiCol_Text), renderedDisplayName.data(),
                                  renderedDisplayName.data() + renderedDisplayName.size());
                drawList->PopClipRect();
                ImGui::SameLine(260.0F);
                const char *problemText = entry_problem_text(project.problem);
                ImGui::TextDisabled("%s / %s", problemText != nullptr ? problemText : entry_state_text(project.state),
                                    compatibility_text(project.compatibilityStatus));
                if (project.isPinned)
                {
                    ImGui::SameLine();
                    ImGui::TextUnformatted("[Pin]");
                }
                if (isSelected)
                {
                    ImGui::Indent();
                    const std::string renderedLocator = make_renderable_text(project.locator);
                    ImGui::TextDisabled("Locator（Unicode Escape表示）");
                    ImGui::TextWrapped("%s", renderedLocator.c_str());
                    const char *recoveryText = entry_problem_recovery_text(project.problem);
                    if (recoveryText != nullptr)
                    {
                        ImGui::TextWrapped("確認: %s", recoveryText);
                    }
                    for (const ProjectCompatibilityReason &reason : project.compatibilityReasons)
                    {
                        if (reason.capability.has_value())
                        {
                            if (reason.minimumVersion.has_value())
                            {
                                ImGui::TextWrapped("互換性 [%s / %s / 必要Version %u.%u以上]: %s",
                                                   capability_text(*reason.capability),
                                                   requirement_kind_text(reason.requirementKind),
                                                   reason.minimumVersion->major, reason.minimumVersion->minor,
                                                   compatibility_reason_text(reason.code));
                            }
                            else
                            {
                                ImGui::TextWrapped("互換性 [%s / %s]: %s", capability_text(*reason.capability),
                                                   requirement_kind_text(reason.requirementKind),
                                                   compatibility_reason_text(reason.code));
                            }
                        }
                        else if ((reason.code == ProjectCompatibilityReasonCode::EngineVersionTooOld ||
                                  reason.code == ProjectCompatibilityReasonCode::EngineVersionTooNew) &&
                                 project.engineCompatibility.has_value())
                        {
                            const EngineCompatibility &range = *project.engineCompatibility;
                            if (range.maximumExclusive.has_value())
                            {
                                ImGui::TextWrapped("互換性 [対応Engine %u.%u.%u以上、%u.%u.%u未満]: %s",
                                                   range.minimum.major, range.minimum.minor, range.minimum.patch,
                                                   range.maximumExclusive->major, range.maximumExclusive->minor,
                                                   range.maximumExclusive->patch, compatibility_reason_text(reason.code));
                            }
                            else
                            {
                                ImGui::TextWrapped("互換性 [対応Engine %u.%u.%u以上]: %s", range.minimum.major,
                                                   range.minimum.minor, range.minimum.patch,
                                                   compatibility_reason_text(reason.code));
                            }
                        }
                        else
                        {
                            ImGui::TextWrapped("互換性: %s", compatibility_reason_text(reason.code));
                        }
                    }
                    ImGui::Unindent();
                }
                ImGui::PopID();
            }
        }
        const bool projectListHasKeyboardFocus = ImGui::IsWindowFocused();
        ImGui::EndChild();

        const ProjectRowView *selected = nullptr;
        for (const ProjectRowView &project : m_service->projects())
        {
            if (project.projectId == m_selectedProjectId)
            {
                selected = &project;
                break;
            }
        }
        ImGui::BeginDisabled(selected == nullptr || !selected->canOpen || !a_canLaunchEditor);
        const bool canActivateWithEnter = a_canLaunchEditor && selected != nullptr && selected->canOpen &&
                                          projectListHasKeyboardFocus &&
                                          !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId) &&
                                          ImGui::IsKeyPressed(ImGuiKey_Enter);
        if (ImGui::Button("Editorで開く") || canActivateWithEnter)
        {
            openProjectId = selected->projectId;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(selected == nullptr || !selected->canMigrate);
        if (ImGui::Button("Project Formatを更新"))
        {
            m_pendingMigrateProjectId = selected->projectId;
            m_openMigrateDialog = true;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(selected == nullptr);
        if (ImGui::Button(selected != nullptr && selected->isPinned ? "Pinを解除" : "Pinに固定"))
        {
            pinProjectId = selected->projectId;
            pinValue = !selected->isPinned;
        }
        ImGui::SameLine();
        if (ImGui::Button("一覧から除外"))
        {
            m_pendingRemoveProjectId = selected->projectId;
            m_openRemoveDialog = true;
        }
        ImGui::EndDisabled();
    }
    catch (...)
    {
        terminate_allocation(*m_assertContext);
    }

    if (!openProjectId.empty())
    {
        m_selectedProjectId = std::move(openProjectId);
        open_selected_project();
        return;
    }
    if (!pinProjectId.empty())
    {
        Result<void> pinned = m_service->set_project_pinned(pinProjectId, pinValue);
        if (pinned)
        {
            set_status(pinValue ? "ProjectをPinに固定しました。" : "ProjectのPinを解除しました。");
        }
        else
        {
            const Error &error = *pinned.try_error();
            if (is_durability_unknown(error))
            {
                set_warning(pinValue
                                ? "ProjectをPinに固定しましたが、Diskへの永続化を確認できませんでした。"
                                  "一覧のPin表示を確認し、次回起動後にも保持されているか再確認してください。"
                                : "ProjectのPinを解除しましたが、Diskへの永続化を確認できませんでした。"
                                  "一覧のPin表示を確認し、次回起動後にも解除されているか再確認してください。");
            }
            else
            {
                set_error(error);
            }
        }
    }
}

void ProjectHubPresenter::draw_create_dialog() noexcept
{
    if (m_openCreateDialog)
    {
        ImGui::OpenPopup("新しいProjectを作成");
        m_openCreateDialog = false;
    }
    bool isOpen = true;
    if (!ImGui::BeginPopupModal("新しいProjectを作成", &isOpen, ImGuiWindowFlags_AlwaysAutoResize))
    {
        return;
    }
    input_locator("作成先Folder", m_parentLocator, *m_assertContext);
    ImGui::InputText("Project名", m_projectName.data(), m_projectName.size());
    ImGui::InputText("表示名", m_displayName.data(), m_displayName.size());
    draw_unicode_escape_preview(m_displayName.data(), *m_assertContext);
    const ProjectTemplateView *selectedTemplate = nullptr;
    for (const ProjectTemplateView &candidate : m_service->templates())
    {
        if (candidate.id == m_selectedTemplateId)
        {
            selectedTemplate = &candidate;
            break;
        }
    }
    const char *templatePreview = selectedTemplate != nullptr ? selectedTemplate->displayName.c_str() : "選択なし";
    if (ImGui::BeginCombo("Template", templatePreview))
    {
        for (const ProjectTemplateView &candidate : m_service->templates())
        {
            const bool isSelected = candidate.id == m_selectedTemplateId;
            if (ImGui::Selectable(candidate.displayName.c_str(), isSelected))
            {
                try
                {
                    m_selectedTemplateId = candidate.id;
                }
                catch (...)
                {
                    terminate_allocation(*m_assertContext);
                }
            }
            if (isSelected)
            {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    const bool canCreate = !m_parentLocator.empty() && m_projectName[0] != '\0' && m_displayName[0] != '\0' &&
                           !m_selectedTemplateId.empty();
    ImGui::BeginDisabled(!canCreate);
    if (ImGui::Button("作成"))
    {
        Result<ProjectCreationOutcome> created =
            m_service->create_blank_project(m_parentLocator, m_projectName.data(), m_displayName.data(),
                                            m_selectedTemplateId, current_milliseconds());
        if (created)
        {
            if (created.try_value()->try_creation_durability_error() != nullptr ||
                created.try_value()->try_recent_persistence_error() != nullptr)
            {
                set_creation_warning(*created.try_value());
            }
            else
            {
                set_status("Projectを作成しました。");
            }
            ImGui::CloseCurrentPopup();
        }
        else
        {
            set_error(*created.try_error());
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("キャンセル") || ImGui::IsKeyPressed(ImGuiKey_Escape))
    {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void ProjectHubPresenter::draw_register_dialog() noexcept
{
    if (m_openRegisterDialog)
    {
        ImGui::OpenPopup("既存Projectを登録");
        m_openRegisterDialog = false;
    }
    bool isOpen = true;
    if (!ImGui::BeginPopupModal("既存Projectを登録", &isOpen, ImGuiWindowFlags_AlwaysAutoResize))
    {
        return;
    }
    input_locator("Project Folder", m_registerLocator, *m_assertContext);
    ImGui::Checkbox("移動した同一Projectとして再関連付け", &m_confirmMovedProject);
    ImGui::BeginDisabled(m_registerLocator.empty());
    if (ImGui::Button("登録"))
    {
        Result<void> registered =
            m_service->register_project(m_registerLocator, current_milliseconds(), m_confirmMovedProject);
        if (registered)
        {
            set_status("Projectを一覧へ登録しました。");
            ImGui::CloseCurrentPopup();
        }
        else
        {
            const Error &error = *registered.try_error();
            if (is_durability_unknown(error))
            {
                set_warning("Projectを一覧へ登録しましたが、Diskへの永続化を確認できませんでした。"
                            "一覧を更新し、表示されない場合はProject Folderを再登録してください。");
                ImGui::CloseCurrentPopup();
            }
            else
            {
                set_error(error);
            }
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("キャンセル") || ImGui::IsKeyPressed(ImGuiKey_Escape))
    {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void ProjectHubPresenter::draw_remove_dialog() noexcept
{
    if (m_openRemoveDialog)
    {
        ImGui::OpenPopup("一覧から除外");
        m_openRemoveDialog = false;
    }
    bool isOpen = true;
    if (!ImGui::BeginPopupModal("一覧から除外", &isOpen, ImGuiWindowFlags_AlwaysAutoResize))
    {
        return;
    }
    ImGui::TextUnformatted("Project Folderは削除せず、Recent一覧からだけ除外します。");
    if (ImGui::Button("除外"))
    {
        Result<void> removed = m_service->remove_project(m_pendingRemoveProjectId);
        if (removed)
        {
            if (m_selectedProjectId == m_pendingRemoveProjectId)
            {
                m_selectedProjectId.clear();
            }
            m_pendingRemoveProjectId.clear();
            set_status("Projectを一覧から除外しました。");
            ImGui::CloseCurrentPopup();
        }
        else
        {
            const Error &error = *removed.try_error();
            if (is_durability_unknown(error))
            {
                if (m_selectedProjectId == m_pendingRemoveProjectId)
                {
                    m_selectedProjectId.clear();
                }
                m_pendingRemoveProjectId.clear();
                set_warning("Projectを一覧から除外しましたが、Diskへの永続化を確認できませんでした。"
                            "一覧を更新して除外状態を再確認してください。");
                ImGui::CloseCurrentPopup();
            }
            else
            {
                set_error(error);
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("キャンセル") || ImGui::IsKeyPressed(ImGuiKey_Escape))
    {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void ProjectHubPresenter::draw_migrate_dialog() noexcept
{
    if (m_openMigrateDialog)
    {
        ImGui::OpenPopup("Project Formatを更新");
        m_openMigrateDialog = false;
    }
    bool isOpen = true;
    if (!ImGui::BeginPopupModal("Project Formatを更新", &isOpen, ImGuiWindowFlags_AlwaysAutoResize))
    {
        return;
    }
    ImGui::TextUnformatted("CueProject.jsonを現在のFormatへAtomicに更新します。");
    ImGui::TextWrapped("version 1のdefaultSceneはnullのまま保持されます。更新後にStartup Sceneを明示選択してください。");
    if (ImGui::Button("更新する"))
    {
        Result<ProjectDescriptorMigrationOutcome> migrated = m_service->migrate_project(m_pendingMigrateProjectId);
        if (migrated)
        {
            m_pendingMigrateProjectId.clear();
            if (migrated.try_value()->try_durability_error() != nullptr)
            {
                set_warning("Project Formatを更新しましたが、Diskへの耐久性を確認できませんでした。"
                            "CueProject.jsonを確認し、次回起動後にもversion 2が保持されているか再確認してください。");
            }
            else
            {
                set_status("Project Formatを更新しました。Startup Sceneを明示選択してください。");
            }
            ImGui::CloseCurrentPopup();
        }
        else
        {
            set_error(*migrated.try_error());
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("キャンセル") || ImGui::IsKeyPressed(ImGuiKey_Escape))
    {
        m_pendingMigrateProjectId.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void ProjectHubPresenter::open_selected_project() noexcept
{
    Result<EditorLaunchRequest> opened = m_service->open_project(m_selectedProjectId, current_milliseconds());
    if (opened)
    {
        m_launchRequest.emplace(std::move(*opened.try_value()));
        set_status("Editorを起動します。");
    }
    else
    {
        const Error &error = *opened.try_error();
        if (error.code().domain() == "Cue.ProjectHub" &&
            error.code().value() ==
                static_cast<std::int64_t>(ProjectHubError::OpenRejectedViewDurabilityUnknown))
        {
            std::string warning;
            try
            {
                warning.assign(open_rejection_text(error));
                warning.append(" 一覧状態は更新されましたが、Diskへの永続化を確認できませんでした。"
                               "Editorは起動していません。現在の一覧と次回起動後の状態を確認してください。");
            }
            catch (...)
            {
                terminate_allocation(*m_assertContext);
            }
            set_warning(warning);
        }
        else if (is_durability_unknown(error))
        {
            set_warning("Projectは開けませんでした。一覧状態は更新されましたが、Diskへの永続化を確認できませんでした。"
                        "Editorは起動していません。現在の一覧と次回起動後の状態を確認してください。");
        }
        else
        {
            set_error(error);
        }
    }
}

void ProjectHubPresenter::set_error(const Error &a_error) noexcept
{
    const ErrorCode &code = a_error.code();
    const ErrorCode &rootCode = a_error.root_code();
    const char *message = "処理に失敗しました。詳細はLogを確認してください。";
    if (code.domain() == "Cue.ProjectHub")
    {
        switch (static_cast<ProjectHubError>(code.value()))
        {
        case ProjectHubError::InvalidLocator:
            message = "Project Folderの場所が正しくありません。";
            break;
        case ProjectHubError::ProjectMissing:
            message = "Project Folderが見つかりません。";
            break;
        case ProjectHubError::ProjectBroken:
            message = "Project情報を読み取れません。";
            break;
        case ProjectHubError::ProjectIdentityMismatch:
            message = "Projectの識別情報が一覧と一致しません。";
            break;
        case ProjectHubError::ProjectUnsupported:
            message = "このVersionのCueEngineではProjectを開けません。";
            break;
        case ProjectHubError::PersistenceFailure:
            message = "Project一覧を保存できませんでした。";
            break;
        case ProjectHubError::InvalidTemplate:
            message = "選択したTemplateは利用できません。";
            break;
        case ProjectHubError::EditorLaunchFailed:
            message = "Editorを起動できませんでした。Editor実行Fileを確認してください。";
            break;
        case ProjectHubError::EditorProcessFailed:
            message = "Editorが異常終了したか、終了状態を確認できませんでした。Logを確認して再試行してください。";
            break;
        case ProjectHubError::OpenRejectedViewDurabilityUnknown:
            message = "Projectを開けず、一覧状態のDiskへの永続化も確認できませんでした。";
            break;
        default:
            break;
        }
    }
    else if (code.domain() == "Cue.Project")
    {
        switch (static_cast<ProjectError>(code.value()))
        {
        case ProjectError::InvalidProjectName:
            message = "Project名は予約語やPath区切りを含まない有効なFolder名にしてください。";
            break;
        case ProjectError::InvalidDisplayName:
            message = "表示名は制御文字を含まない1～256 byteのUTF-8文字列にしてください。";
            break;
        case ProjectError::InvalidProjectLocator:
            message = "Project Folderの場所が正しくありません。";
            break;
        case ProjectError::ProjectLocatorConflict:
            message = "同じProject Folderが別のProjectとして登録されています。";
            break;
        case ProjectError::DuplicateProjectId:
            message = "同じProject IDが別のFolderで登録済みです。移動した同一Projectなら再関連付けを有効にして登録してください。";
            break;
        case ProjectError::ProjectNotRegistered:
            message = "Projectは一覧に登録されていません。一覧を更新してください。";
            break;
        case ProjectError::IoFailure:
            message = "Project Fileを読み書きできませんでした。Folderの状態を確認してください。";
            break;
        default:
            message = "Project情報が正しくありません。入力内容とProject Fileを確認してください。";
            break;
        }
    }
    else if (rootCode.domain() == "Cue.IO" &&
             rootCode.value() == static_cast<std::int64_t>(IoError::PermissionDenied))
    {
        message = "Folderへアクセスする権限がありません。";
    }
    if (rootCode.domain() == "Cue.IO" &&
        rootCode.value() == static_cast<std::int64_t>(IoError::DurabilityUnknown))
    {
        message = "保存は完了しましたが、Diskへの永続化を確認できませんでした。";
    }
    set_status(message);
    m_hasError = true;
}

void ProjectHubPresenter::set_creation_warning(const ProjectCreationOutcome &a_outcome) noexcept
{
    const Error *creationError = a_outcome.try_creation_durability_error();
    const Error *recentError = a_outcome.try_recent_persistence_error();
    std::string warning;
    try
    {
        warning = "Project Folder（Unicode Escape表示）: ";
        warning.append(make_renderable_text(a_outcome.project_locator()));
        warning.push_back('\n');
        if (creationError != nullptr)
        {
            warning.append("Project作成: Fileは公開されましたが、Diskへの永続化を確認できませんでした。\n");
            warning.append("再確認: 上記Folderを開き、CueProject.jsonを確認してください。\n");
            warning.append("診断(Project作成): ");
            warning.append(creationError->root_code().domain());
            warning.push_back('/');
            warning.append(std::to_string(creationError->root_code().value()));
            warning.append(" - ");
            warning.append(creationError->summary());
            warning.push_back('\n');
        }
        if (recentError != nullptr)
        {
            if (a_outcome.is_recent_registered())
            {
                warning.append("Recent一覧: 登録は公開されましたが、Diskへの永続化を確認できませんでした。\n");
                warning.append("再確認: 一覧を更新し、表示されない場合は上記Project Folderを再登録してください。\n");
            }
            else
            {
                warning.append("Recent一覧: 登録または保存に失敗しました。\n");
                warning.append("再試行: 「既存Projectを登録」から上記Project Folderを再登録してください。\n");
            }
            warning.append("診断(Recent): ");
            warning.append(recentError->root_code().domain());
            warning.push_back('/');
            warning.append(std::to_string(recentError->root_code().value()));
            warning.append(" - ");
            warning.append(recentError->summary());
        }
    }
    catch (...)
    {
        terminate_allocation(*m_assertContext);
    }
    set_warning(warning);
}

void ProjectHubPresenter::set_warning(std::string_view a_warning) noexcept
{
    set_status(a_warning);
    m_hasWarning = true;
}

void ProjectHubPresenter::set_status(std::string_view a_status) noexcept
{
    try
    {
        m_message.assign(a_status);
    }
    catch (...)
    {
        terminate_allocation(*m_assertContext);
    }
    m_hasError = false;
    m_hasWarning = false;
}

std::optional<EditorLaunchRequest> ProjectHubPresenter::take_editor_launch_request() noexcept
{
    std::optional<EditorLaunchRequest> request = std::move(m_launchRequest);
    m_launchRequest.reset();
    return request;
}

void ProjectHubPresenter::report_editor_launch_failure(const Error &a_error) noexcept
{
    if (a_error.root_code().domain() == "Editor.ExitCode")
    {
        std::string message;
        try
        {
            message = "Editorが異常終了しました（Exit Code: ";
            message.append(std::to_string(a_error.root_code().value()));
            message.append("）。Logを確認して再試行してください。");
        }
        catch (...)
        {
            terminate_allocation(*m_assertContext);
        }
        set_status(message);
        m_hasError = true;
        return;
    }
    set_error(a_error);
}

void ProjectHubPresenter::report_editor_process_completed() noexcept
{
    set_status("Editorを終了しました。別のProjectを開けます。");
}

bool ProjectHubPresenter::is_exit_requested() const noexcept
{
    return m_isExitRequested;
}
} // namespace cue::project_hub
