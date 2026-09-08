#include <Cue/Editor/ImGui/BuildPresenter.h>

#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Error.h>

#include <algorithm>
#include <cstdlib>
#include <string>
#include <utility>

#include <imgui.h>

namespace
{
constexpr std::size_t k_maximumSavedOperations = 8U;

/// @brief Build ConfigurationをUI表示用Labelへ変換する
[[nodiscard]] const char *configuration_label(cue::BuildConfiguration a_configuration) noexcept
{
    switch (a_configuration)
    {
    case cue::BuildConfiguration::Debug:
        return "Debug";
    case cue::BuildConfiguration::Development:
        return "Development";
    case cue::BuildConfiguration::Release:
        return "Release";
    }
    return "不明";
}

/// @brief Build Operation Stateを日本語UI表示用Labelへ変換する
[[nodiscard]] const char *state_label(cue::GameBuildOperationState a_state) noexcept
{
    switch (a_state)
    {
    case cue::GameBuildOperationState::Idle:
        return "待機中";
    case cue::GameBuildOperationState::Running:
        return "実行中";
    case cue::GameBuildOperationState::Succeeded:
        return "成功";
    case cue::GameBuildOperationState::Failed:
        return "失敗";
    case cue::GameBuildOperationState::Cancelled:
        return "キャンセル済み";
    case cue::GameBuildOperationState::TimedOut:
        return "タイムアウト";
    }
    return "不明";
}

/// @brief Build StageをUI表示用Labelへ変換する
[[nodiscard]] const char *stage_label(cue::BuildStage a_stage) noexcept
{
    switch (a_stage)
    {
    case cue::BuildStage::Configure:
        return "Configure";
    case cue::BuildStage::Build:
        return "Build";
    }
    return "不明";
}

/// @brief Child Process StreamをConsole表示用Labelへ変換する
[[nodiscard]] const char *stream_label(cue::ChildProcessStream a_stream) noexcept
{
    switch (a_stream)
    {
    case cue::ChildProcessStream::StandardOutput:
        return "stdout";
    case cue::ChildProcessStream::StandardError:
        return "stderr";
    }
    return "不明";
}

/// @brief ASCII大小文字を区別せずConsole Filterとの部分一致を判定する
[[nodiscard]] bool contains_ascii_case_insensitive(std::string_view a_text, std::string_view a_filter) noexcept
{
    if (a_filter.empty())
    {
        return true;
    }
    /// @brief ASCII英大文字だけを小文字へ正規化する
    const auto fold = [](unsigned char a_value) noexcept
    { return a_value >= 'A' && a_value <= 'Z' ? static_cast<unsigned char>(a_value - 'A' + 'a') : a_value; };
    return std::search(a_text.begin(), a_text.end(), a_filter.begin(), a_filter.end(),
                       /// @brief 正規化済みASCII文字同士の一致を判定する
                       [fold](char a_left, char a_right) noexcept
                       {
                           return fold(static_cast<unsigned char>(a_left)) == fold(static_cast<unsigned char>(a_right));
                       }) != a_text.end();
}

/// @brief 一ByteをImGui表示用Hex Escapeへ追加する
void append_byte_escape(std::string &a_output, unsigned char a_value)
{
    constexpr char hexDigits[] = "0123456789ABCDEF";
    a_output.append("\\x");
    a_output.push_back(hexDigits[(a_value >> 4U) & 0x0FU]);
    a_output.push_back(hexDigits[a_value & 0x0FU]);
}

/// @brief 指定位置からStrict UTF-8 Scalarを構成するByte数を返す
[[nodiscard]] std::size_t valid_utf8_sequence_length(std::string_view a_text, std::size_t a_index) noexcept
{
    const auto first = static_cast<unsigned char>(a_text[a_index]);
    std::size_t length = 0U;
    if (first >= 0xC2U && first <= 0xDFU)
    {
        length = 2U;
    }
    else if (first >= 0xE0U && first <= 0xEFU)
    {
        length = 3U;
    }
    else if (first >= 0xF0U && first <= 0xF4U)
    {
        length = 4U;
    }
    else
    {
        return 0U;
    }
    if (a_index + length > a_text.size())
    {
        return 0U;
    }
    const auto second = static_cast<unsigned char>(a_text[a_index + 1U]);
    if ((second & 0xC0U) != 0x80U || (first == 0xE0U && second < 0xA0U) || (first == 0xEDU && second >= 0xA0U) ||
        (first == 0xF0U && second < 0x90U) || (first == 0xF4U && second >= 0x90U))
    {
        return 0U;
    }
    for (std::size_t offset = 2U; offset < length; ++offset)
    {
        if ((static_cast<unsigned char>(a_text[a_index + offset]) & 0xC0U) != 0x80U)
        {
            return 0U;
        }
    }
    return length;
}

/// @brief 制御Byteと不正UTF-8をEscapeしてImGui Textへ安全に渡せるLog文字列を構築する
[[nodiscard]] std::string make_visible_log_text(std::string_view a_bytes)
{
    std::string visible;
    visible.reserve(a_bytes.size());
    for (std::size_t index = 0U; index < a_bytes.size();)
    {
        const auto value = static_cast<unsigned char>(a_bytes[index]);
        if ((value < 0x20U && value != '\n' && value != '\t') || value == 0x7FU)
        {
            append_byte_escape(visible, value);
            ++index;
        }
        else if (value < 0x80U)
        {
            visible.push_back(static_cast<char>(value));
            ++index;
        }
        else
        {
            const std::size_t length = valid_utf8_sequence_length(a_bytes, index);
            if (length == 0U)
            {
                append_byte_escape(visible, value);
                ++index;
            }
            else
            {
                visible.append(a_bytes.substr(index, length));
                index += length;
            }
        }
    }
    return visible;
}
} // namespace

namespace cue::editor
{
BuildPresenter::BuildPresenter(GameBuildService &a_service, std::string a_projectRoot,
                               BuildWorkspaceCompatibility a_workspaceCompatibility,
                               std::unique_ptr<BuildOperationIdSource> a_operationIdSource,
                               const AssertContext &a_assertContext) noexcept
    : m_service(&a_service), m_assertContext(&a_assertContext), m_operationIdSource(std::move(a_operationIdSource)),
      m_projectRoot(std::move(a_projectRoot)), m_workspaceCompatibility(a_workspaceCompatibility)
{
}

BuildPresenter::~BuildPresenter() noexcept
{
    if (m_service != nullptr && m_service->snapshot().state == GameBuildOperationState::Running)
    {
        static_cast<void>(m_service->request_cancel());
        static_cast<void>(m_service->wait_for_completion());
    }
}

std::unique_ptr<BuildPresenter> BuildPresenter::create(GameBuildService &a_service, std::string a_projectRoot,
                                                       BuildWorkspaceCompatibility a_workspaceCompatibility,
                                                       std::unique_ptr<BuildOperationIdSource> a_operationIdSource,
                                                       const AssertContext &a_assertContext) noexcept
{
    try
    {
        if (a_projectRoot.empty() || !a_operationIdSource)
        {
            a_assertContext.fatal_handler().terminate("Build Presenter dependencies are invalid");
            std::abort();
        }
        auto presenter = std::unique_ptr<BuildPresenter>(
            new BuildPresenter(a_service, std::move(a_projectRoot), a_workspaceCompatibility,
                               std::move(a_operationIdSource), a_assertContext));
        presenter->refresh();
        return presenter;
    }
    catch (...)
    {
        a_assertContext.fatal_handler().terminate("Build Presenter creation failed unexpectedly");
        std::abort();
    }
}

void BuildPresenter::refresh() noexcept
{
    try
    {
        BuildOperationSnapshot snapshot = m_service->snapshot();
        if (snapshot.operationId != m_current.operationId)
        {
            save_current_operation();
            m_displayOperationId = snapshot.operationId;
        }
        m_current = std::move(snapshot);

        if (m_isShutdownWaitingForCancel && m_current.state != GameBuildOperationState::Running)
        {
            Result<void> completed = m_service->wait_for_completion();
            if (!completed)
            {
                set_error(*completed.try_error(), "終了前のBuild停止");
                m_isShutdownWaitingForCancel = false;
                return;
            }
            m_isShutdownWaitingForCancel = false;
            mark_shutdown_ready();
        }
    }
    catch (...)
    {
        terminate_exception();
    }
}

void BuildPresenter::process_shortcuts() noexcept
{
    try
    {
        const bool canUseKeyboard =
            !ImGui::GetIO().WantTextInput && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
        if (canUseKeyboard && ImGui::IsKeyPressed(ImGuiKey_F6, false))
        {
            const ImGuiIO &input = ImGui::GetIO();
            static_cast<void>(submit(input.KeyCtrl    ? EditorBuildCommand::Cancel
                                     : input.KeyShift ? EditorBuildCommand::Retry
                                                      : EditorBuildCommand::Start));
        }
    }
    catch (...)
    {
        terminate_exception();
    }
}

void BuildPresenter::draw() noexcept
{
    try
    {
        refresh();
        if (ImGui::Begin("Game Build"))
        {
            draw_toolbar();
            if (!m_message.empty())
            {
                ImGui::PushStyleColor(ImGuiCol_Text, m_hasError ? ImVec4(1.0F, 0.35F, 0.35F, 1.0F)
                                                                : ImVec4(0.35F, 0.85F, 0.45F, 1.0F));
                ImGui::TextWrapped("%s", m_message.c_str());
                ImGui::PopStyleColor();
            }
            ImGui::Separator();
            draw_progress();
            ImGui::Separator();
            draw_console();
            ImGui::Separator();
            draw_artifacts();
        }
        ImGui::End();
        draw_shutdown_confirmation();
    }
    catch (...)
    {
        terminate_exception();
    }
}

bool BuildPresenter::submit(EditorBuildCommand a_command) noexcept
{
    try
    {
        refresh();
        if ((a_command == EditorBuildCommand::Start && !can_start()) ||
            (a_command == EditorBuildCommand::Cancel && !can_cancel()) ||
            (a_command == EditorBuildCommand::Retry && !can_retry()))
        {
            return false;
        }
        if (a_command == EditorBuildCommand::Cancel)
        {
            Result<void> cancelled = m_service->request_cancel();
            if (!cancelled)
            {
                set_error(*cancelled.try_error(), "Buildのキャンセル");
                return false;
            }
            set_status("Buildのキャンセルを要求しました。");
            return true;
        }

        Result<std::string> operationId = m_operationIdSource->next_operation_id();
        if (!operationId)
        {
            set_error(*operationId.try_error(), "Build Operation IDの生成");
            return false;
        }
        const std::string requestedOperationId = *operationId.try_value();
        Result<void> submitted = Result<void>::success();
        if (a_command == EditorBuildCommand::Retry)
        {
            submitted = m_service->retry(std::move(*operationId.try_value()));
        }
        else
        {
            Result<BuildProfile> profile =
                BuildProfile::create(m_configuration, BuildTarget::GameModule, *m_assertContext);
            if (!profile)
            {
                set_error(*profile.try_error(), "Build Profileの作成");
                return false;
            }
            BuildRequest request{m_projectRoot, std::move(*profile.try_value()), std::move(*operationId.try_value()),
                                 m_workspaceCompatibility};
            submitted =
                m_service->start(std::move(request), m_forceConfigure ? CMakeConfigureMode::Required
                                                                      : CMakeConfigureMode::ReuseCompatibleTree);
        }
        if (!submitted)
        {
            set_error(*submitted.try_error(), a_command == EditorBuildCommand::Retry ? "Buildの再実行" : "Buildの開始");
            return false;
        }
        m_displayOperationId = requestedOperationId;
        refresh();
        set_status(a_command == EditorBuildCommand::Retry ? "Buildを最初から再実行しました。"
                                                          : "Game Module Buildを開始しました。");
        return true;
    }
    catch (...)
    {
        terminate_exception();
    }
}

bool BuildPresenter::set_configuration(BuildConfiguration a_configuration) noexcept
{
    if (!can_start())
    {
        return false;
    }
    m_configuration = a_configuration;
    return true;
}

bool BuildPresenter::set_force_configure(bool a_forceConfigure) noexcept
{
    if (!can_start())
    {
        return false;
    }
    m_forceConfigure = a_forceConfigure;
    return true;
}

bool BuildPresenter::begin_editor_shutdown() noexcept
{
    refresh();
    if (m_current.state != GameBuildOperationState::Running)
    {
        return true;
    }
    m_openShutdownConfirmation = true;
    m_isShutdownConfirmationPending = true;
    return false;
}

bool BuildPresenter::respond_to_editor_shutdown(EditorBuildShutdownDecision a_decision) noexcept
{
    if (!m_isShutdownConfirmationPending)
    {
        return false;
    }
    if (a_decision == EditorBuildShutdownDecision::KeepEditorOpen)
    {
        m_openShutdownConfirmation = false;
        m_isShutdownConfirmationPending = false;
        m_isShutdownWaitingForCancel = false;
        return false;
    }
    refresh();
    if (m_current.state != GameBuildOperationState::Running)
    {
        mark_shutdown_ready();
        return true;
    }
    if (!m_isShutdownWaitingForCancel && !submit(EditorBuildCommand::Cancel))
    {
        refresh();
        if (m_current.state != GameBuildOperationState::Running)
        {
            mark_shutdown_ready();
            return true;
        }
        return false;
    }
    m_isShutdownWaitingForCancel = true;
    refresh();
    return m_isShutdownReady;
}

bool BuildPresenter::take_shutdown_ready() noexcept
{
    const bool ready = m_isShutdownReady;
    m_isShutdownReady = false;
    return ready;
}

const BuildOperationSnapshot &BuildPresenter::current_snapshot() const noexcept
{
    return m_current;
}

std::span<const BuildOperationSnapshot> BuildPresenter::saved_operations() const noexcept
{
    return m_savedOperations;
}

bool BuildPresenter::select_operation(std::string_view a_operationId) noexcept
{
    try
    {
        if (m_current.operationId == a_operationId)
        {
            m_displayOperationId.assign(a_operationId);
            return true;
        }
        const auto found = std::find_if(m_savedOperations.begin(), m_savedOperations.end(),
                                        /// @brief 指定Operation IDと一致する履歴を選ぶ
                                        [a_operationId](const BuildOperationSnapshot &a_snapshot) noexcept
                                        { return a_snapshot.operationId == a_operationId; });
        if (found == m_savedOperations.end())
        {
            return false;
        }
        m_displayOperationId.assign(a_operationId);
        return true;
    }
    catch (...)
    {
        terminate_exception();
    }
}

const BuildOperationSnapshot &BuildPresenter::displayed_snapshot() const noexcept
{
    if (m_current.operationId == m_displayOperationId)
    {
        return m_current;
    }
    const auto found = std::find_if(m_savedOperations.begin(), m_savedOperations.end(),
                                    /// @brief 現在の表示対象Operation IDと一致する履歴を選ぶ
                                    [this](const BuildOperationSnapshot &a_snapshot) noexcept
                                    { return a_snapshot.operationId == m_displayOperationId; });
    return found != m_savedOperations.end() ? *found : m_current;
}

std::size_t BuildPresenter::displayed_log_count() const noexcept
{
    const BuildOperationSnapshot &snapshot = displayed_snapshot();
    return static_cast<std::size_t>(std::count_if(snapshot.logs.begin(), snapshot.logs.end(),
                                                  /// @brief 表示対象Operationに属するLogだけを数える
                                                  [&snapshot](const BuildLogSnapshot &a_log) noexcept
                                                  { return a_log.operationId == snapshot.operationId; }));
}

std::string_view BuildPresenter::message() const noexcept
{
    return m_message;
}

bool BuildPresenter::has_error_message() const noexcept
{
    return m_hasError;
}

bool BuildPresenter::can_start() const noexcept
{
    return m_current.state != GameBuildOperationState::Running && !m_isShutdownConfirmationPending;
}

bool BuildPresenter::can_cancel() const noexcept
{
    return m_current.state == GameBuildOperationState::Running;
}

bool BuildPresenter::can_retry() const noexcept
{
    return m_current.state != GameBuildOperationState::Idle && m_current.state != GameBuildOperationState::Running &&
           !m_isShutdownConfirmationPending;
}

bool BuildPresenter::is_shutdown_confirmation_pending() const noexcept
{
    return m_isShutdownConfirmationPending;
}

void BuildPresenter::save_current_operation()
{
    if (m_current.operationId.empty() || m_current.state == GameBuildOperationState::Idle ||
        m_current.state == GameBuildOperationState::Running)
    {
        return;
    }
    const auto duplicate = std::find_if(m_savedOperations.begin(), m_savedOperations.end(),
                                        /// @brief 現在Operationと同じ履歴Entryを検出する
                                        [this](const BuildOperationSnapshot &a_snapshot) noexcept
                                        { return a_snapshot.operationId == m_current.operationId; });
    if (duplicate != m_savedOperations.end())
    {
        *duplicate = m_current;
        return;
    }
    if (m_savedOperations.size() == k_maximumSavedOperations)
    {
        m_savedOperations.erase(m_savedOperations.begin());
    }
    m_savedOperations.push_back(m_current);
}

void BuildPresenter::set_error(const Error &a_error, std::string_view a_operation)
{
    m_message.assign(a_operation);
    m_message.append("に失敗しました: ");
    m_message.append(a_error.summary());
    m_message.append(" (");
    m_message.append(a_error.code().domain());
    m_message.push_back(':');
    m_message.append(std::to_string(a_error.code().value()));
    m_message.push_back(')');
    if (const NativeError *native = a_error.try_native_error(); native != nullptr)
    {
        m_message.append(" Native=");
        m_message.append(native->domain());
        m_message.push_back(':');
        m_message.append(std::to_string(native->value()));
    }
    for (const ErrorContext &context : a_error.contexts())
    {
        m_message.append("\n");
        m_message.append(context.message());
    }
    for (const ErrorCause &cause : a_error.causes())
    {
        m_message.append("\n原因: ");
        m_message.append(cause.summary());
        m_message.append(" (");
        m_message.append(cause.code().domain());
        m_message.push_back(':');
        m_message.append(std::to_string(cause.code().value()));
        m_message.push_back(')');
        if (const NativeError *native = cause.try_native_error(); native != nullptr)
        {
            m_message.append(" Native=");
            m_message.append(native->domain());
            m_message.push_back(':');
            m_message.append(std::to_string(native->value()));
        }
        for (const ErrorContext &context : cause.contexts())
        {
            m_message.append("\n");
            m_message.append(context.message());
        }
    }
    m_hasError = true;
}

void BuildPresenter::set_status(std::string_view a_status)
{
    m_message.assign(a_status);
    m_hasError = false;
}

void BuildPresenter::draw_toolbar() noexcept
{
    ImGui::BeginDisabled(!can_start());
    if (ImGui::BeginCombo("Configuration", configuration_label(m_configuration)))
    {
        constexpr BuildConfiguration configurations[] = {BuildConfiguration::Debug, BuildConfiguration::Development,
                                                         BuildConfiguration::Release};
        for (const BuildConfiguration configuration : configurations)
        {
            if (ImGui::Selectable(configuration_label(configuration), configuration == m_configuration))
            {
                static_cast<void>(set_configuration(configuration));
            }
        }
        ImGui::EndCombo();
    }
    ImGui::Checkbox("Configureを実行する", &m_forceConfigure);
    ImGui::EndDisabled();

    ImGui::BeginDisabled(!can_start());
    if (ImGui::Button("Build (F6)"))
    {
        static_cast<void>(submit(EditorBuildCommand::Start));
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!can_cancel());
    if (ImGui::Button("Cancel (Ctrl+F6)"))
    {
        static_cast<void>(submit(EditorBuildCommand::Cancel));
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!can_retry());
    if (ImGui::Button("Retry (Shift+F6)"))
    {
        static_cast<void>(submit(EditorBuildCommand::Retry));
    }
    ImGui::EndDisabled();
    if (m_current.state == GameBuildOperationState::Running)
    {
        ImGui::SameLine();
        ImGui::TextDisabled("別のBuildは開始できません");
    }
}

void BuildPresenter::draw_progress() noexcept
{
    const BuildOperationSnapshot &selected = displayed_snapshot();
    ImGui::Text("状態: %s", state_label(selected.state));
    if (selected.activeStage)
    {
        ImGui::Text("Stage: %s", stage_label(*selected.activeStage));
        ImGui::ProgressBar(selected.activeStage == BuildStage::Configure ? 0.35F : 0.75F, ImVec2(-1.0F, 0.0F));
    }
    else if (selected.state == GameBuildOperationState::Succeeded)
    {
        ImGui::ProgressBar(1.0F, ImVec2(-1.0F, 0.0F));
    }
    for (const BuildDiagnosticSnapshot &diagnostic : selected.diagnostics)
    {
        ImGui::TextColored(ImVec4(1.0F, 0.35F, 0.35F, 1.0F), "%s (%s:%lld)", diagnostic.summary.c_str(),
                           diagnostic.domain.c_str(), static_cast<long long>(diagnostic.code));
        if (diagnostic.nativeError)
        {
            ImGui::BulletText("Native: %s:%lld", diagnostic.nativeError->domain.c_str(),
                              static_cast<long long>(diagnostic.nativeError->code));
        }
        for (const std::string &context : diagnostic.contexts)
        {
            ImGui::BulletText("%s", context.c_str());
        }
    }
}

void BuildPresenter::draw_console()
{
    const BuildOperationSnapshot &selected = displayed_snapshot();
    const char *preview = selected.operationId.empty() ? "Operationなし" : selected.operationId.c_str();
    if (ImGui::BeginCombo("表示Operation", preview))
    {
        if (!m_current.operationId.empty() &&
            ImGui::Selectable(m_current.operationId.c_str(), m_displayOperationId == m_current.operationId))
        {
            m_displayOperationId = m_current.operationId;
        }
        for (const BuildOperationSnapshot &saved : m_savedOperations)
        {
            if (ImGui::Selectable(saved.operationId.c_str(), m_displayOperationId == saved.operationId))
            {
                m_displayOperationId = saved.operationId;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::InputTextWithHint("##BuildLogFilter", "Console Filter", m_filter.data(), m_filter.size());
    ImGui::SameLine();
    if (ImGui::Button("Copy"))
    {
        std::string copied;
        for (const BuildLogSnapshot &log : selected.logs)
        {
            if (log.operationId == selected.operationId && contains_ascii_case_insensitive(log.bytes, m_filter.data()))
            {
                copied.append("[");
                copied.append(stage_label(log.stage));
                copied.append("][");
                copied.append(stream_label(log.stream));
                copied.append("] ");
                copied.append(make_visible_log_text(log.bytes));
                if (!copied.ends_with('\n'))
                {
                    copied.push_back('\n');
                }
            }
        }
        ImGui::SetClipboardText(copied.c_str());
    }
    if (ImGui::BeginChild("BuildConsoleEntries", ImVec2(0.0F, 220.0F), true))
    {
        for (const BuildLogSnapshot &log : selected.logs)
        {
            if (log.operationId != selected.operationId || !contains_ascii_case_insensitive(log.bytes, m_filter.data()))
            {
                continue;
            }
            ImGui::TextDisabled("[%s] [%s]", stage_label(log.stage), stream_label(log.stream));
            ImGui::SameLine();
            const std::string visible = make_visible_log_text(log.bytes);
            ImGui::TextUnformatted(visible.data(), visible.data() + visible.size());
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            {
                ImGui::SetClipboardText(visible.c_str());
                set_status("選択したLogをClipboardへコピーしました。");
            }
        }
    }
    ImGui::EndChild();
}

void BuildPresenter::draw_artifacts() noexcept
{
    const BuildOperationSnapshot &selected = displayed_snapshot();
    ImGui::TextUnformatted("Artifact");
    if (selected.artifact)
    {
        const std::string_view artifactId = selected.artifact->artifact_id();
        ImGui::Text("成功Version: %.*s", static_cast<int>(artifactId.size()), artifactId.data());
        for (const BuildArtifactFile &file : selected.artifact->files())
        {
            ImGui::BulletText("%s (%llu bytes)", file.relativePath.c_str(),
                              static_cast<unsigned long long>(file.byteSize));
        }
    }
    else
    {
        ImGui::TextDisabled("このOperationに成功Artifactはありません。");
    }
    if (m_current.latestSuccessfulArtifact)
    {
        const std::string_view artifactId = m_current.latestSuccessfulArtifact->artifact_id();
        ImGui::Text("Latest Successful Build: %.*s", static_cast<int>(artifactId.size()), artifactId.data());
    }
    else
    {
        ImGui::TextDisabled("Latest Successful Buildはまだありません。");
    }
}

void BuildPresenter::draw_shutdown_confirmation() noexcept
{
    if (m_openShutdownConfirmation)
    {
        ImGui::OpenPopup("Build中のEditor終了");
        m_openShutdownConfirmation = false;
    }
    if (!m_isShutdownConfirmationPending ||
        !ImGui::BeginPopupModal("Build中のEditor終了", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        return;
    }
    ImGui::TextUnformatted("実行中のBuildをキャンセルしてEditorを終了しますか？");
    if (m_isShutdownWaitingForCancel)
    {
        ImGui::TextDisabled("Child Processの終了を待っています。");
    }
    ImGui::BeginDisabled(m_isShutdownWaitingForCancel);
    if (ImGui::Button("Buildをキャンセルして終了"))
    {
        static_cast<void>(respond_to_editor_shutdown(EditorBuildShutdownDecision::CancelBuildAndClose));
    }
    ImGui::SameLine();
    if (ImGui::Button("Editorへ戻る"))
    {
        static_cast<void>(respond_to_editor_shutdown(EditorBuildShutdownDecision::KeepEditorOpen));
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::EndPopup();
}

void BuildPresenter::mark_shutdown_ready() noexcept
{
    m_openShutdownConfirmation = false;
    m_isShutdownConfirmationPending = false;
    m_isShutdownWaitingForCancel = false;
    m_isShutdownReady = true;
}

[[noreturn]] void BuildPresenter::terminate_exception() const noexcept
{
    m_assertContext->fatal_handler().terminate("Build Presenter failed unexpectedly");
    std::abort();
}
} // namespace cue::editor
