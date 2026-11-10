#include "EditorScriptController.h"

#include <utility>

#include <EditorIde/ScriptSource.h>
#include <Platform/FileSystem.h>

namespace cue
{
namespace
{
/// @brief Metadata 専用子も含め、DLL の置換と重なる期間を判定する
bool play_is_busy(const PlayClient *a_play) noexcept
{
    if (!a_play)
        return false;
    const auto state = a_play->state();
    return state == PlayState::Describing || state == PlayState::Starting || state == PlayState::Running ||
           state == PlayState::Stopping || state == PlayState::WaitingForDebugger;
}
} // namespace

EditorScriptController::EditorScriptController(const Project &a_project, IFileSystem &a_files,
                                               IGameBuildService &a_build, IEditorIdeService &a_ide,
                                               EditorScriptCallbacks a_callbacks)
    : m_project(a_project), m_files(a_files), m_build(a_build), m_ide(a_ide), m_callbacks(std::move(a_callbacks)),
      m_owner(std::this_thread::get_id())
{
}

Result<void> EditorScriptController::check_start(bool a_allowPlay) const
{
    if (std::this_thread::get_id() != m_owner)
        return Result<void>::failure({ErrorCategory::WrongThread, "EditorScript.owner"});
    const auto &build = m_build.status();
    if (m_status.isBusy || m_ide.status().state == IdeState::Running || build.state == GameBuildState::Configuring ||
        build.state == GameBuildState::Building || build.isApplicationRunning ||
        (!a_allowPlay && play_is_busy(m_callbacks.play ? m_callbacks.play() : nullptr)))
        return Result<void>::failure({ErrorCategory::InvalidState, "EditorScript.busy"});
    if (!m_project.descriptor().gameTarget)
        return Result<void>::failure({ErrorCategory::InvalidState, "EditorScript.no_game_target"});
    return Result<void>::success();
}

void EditorScriptController::transition(EditorScriptState a_state, const char *a_message, bool a_busy)
{
    m_status.state = a_state;
    m_status.message = a_message;
    m_status.isBusy = a_busy;
}

Result<void> EditorScriptController::start(Request a_request)
{
    auto access = check_start(a_request == Request::OpenSource || a_request == Request::OpenFolder);
    if (!access.has_value())
        return access;
    auto target = current_native_game_build_target();
    if (!target.has_value())
        return Result<void>::failure(*target.try_error());
    if (m_callbacks.configuration)
        target.try_value()->configuration = m_callbacks.configuration();
    auto valid = validate_game_build_target(*target.try_value());
    if (!valid.has_value())
        return valid;
    // Source を開く操作は、既に確認した Workspace を再利用し、毎回 Configure しない
    if (a_request == Request::OpenSource && !m_workspace.is_empty())
    {
        auto info = m_files.stat(m_workspace);
        if (info.has_value() && *info.try_value() && (**info.try_value()).type == FileType::Directory &&
            m_workspace.utf8() == m_project.root().utf8())
        {
            auto opened = m_ide.open_source(m_workspace, m_source, 1);
            if (!opened.has_value())
                return opened;
            m_request = a_request;
            m_status.error.reset();
            transition(EditorScriptState::OpeningIde, "Opening script source", true);
            return Result<void>::success();
        }
        m_workspace = {};
    }
    const auto purpose = a_request == Request::OpenSource || a_request == Request::OpenFolder
                             ? GameBuildPurpose::IdeProject
                             : GameBuildPurpose::ScriptModule;
    auto started = m_build.start(m_project, *target.try_value(), purpose);
    if (!started.has_value())
        return started;
    m_ownsBuild = true;
    m_request = a_request;
    m_status.error.reset();
    m_status.processId = 0;
    transition(purpose == GameBuildPurpose::IdeProject ? EditorScriptState::PreparingWorkspace
                                                       : EditorScriptState::BuildingScripts,
               purpose == GameBuildPurpose::IdeProject ? "Preparing Visual Studio workspace" : "Building game DLL",
               true);
    return Result<void>::success();
}

Result<void> EditorScriptController::open_folder()
{
    return start(Request::OpenFolder);
}

Result<void> EditorScriptController::open_script(std::string_view a_typeId)
{
    auto access = check_start(true);
    if (!access.has_value())
        return access;
    auto source = resolve_script_source(m_files, m_project, a_typeId);
    if (!source.has_value())
        return Result<void>::failure(*source.try_error());
    m_source = source.take_value();
    return start(Request::OpenSource);
}

Result<void> EditorScriptController::build_scripts()
{
    return start(Request::BuildScripts);
}

Result<void> EditorScriptController::debug_play(const Scene &a_scene)
{
    auto access = check_start(false);
    if (!access.has_value())
        return access;
    // UI / Document の借用を Build 中へ持ち越さない
    m_scene = a_scene;
    auto started = start(Request::DebugPlay);
    if (!started.has_value())
        m_scene.reset();
    return started;
}

Result<void> EditorScriptController::fail(Error a_error)
{
    auto cancelled = cancel();
    (void)cancelled;
    a_error.category = ErrorCategory::PlatformFailure;
    m_status.error = a_error;
    transition(EditorScriptState::Failed, "Script / IDE operation failed", false);
    m_workspace = {};
    return Result<void>::failure(std::move(a_error));
}

Result<void> EditorScriptController::cancel()
{
    if (std::this_thread::get_id() != m_owner)
        return Result<void>::failure({ErrorCategory::WrongThread, "EditorScript.owner"});
    std::optional<Error> failure;
    auto ide = m_ide.cancel();
    if (!ide.has_value())
        failure = *ide.try_error();
    if (m_ownsBuild)
    {
        auto stopped = m_build.cancel();
        if (!stopped.has_value() && !failure)
            failure = *stopped.try_error();
    }
    auto *play = m_callbacks.play ? m_callbacks.play() : nullptr;
    if (m_request == Request::DebugPlay && m_hasDebugSession && play && play->is_debugging() &&
        play->session_id() == m_debugSessionId)
    {
        auto stopped = play->stop();
        if (!stopped.has_value() && !failure)
            failure = *stopped.try_error();
    }
    m_ownsBuild = false;
    if (m_request == Request::DebugPlay)
    {
        m_hasDebugSession = false;
        m_debugSessionId = 0;
    }
    m_scene.reset();
    transition(EditorScriptState::Cancelled, "Script / IDE operation cancelled", false);
    return failure ? Result<void>::failure(*failure) : Result<void>::success();
}

Result<void> EditorScriptController::start_debug_child()
{
    if (!m_scene || !m_callbacks.startDebug)
        return fail({ErrorCategory::InvalidState, "EditorScript.debug_scene"});
    auto started = m_callbacks.startDebug(*m_scene);
    if (!started.has_value())
        return fail(*started.try_error());
    m_hasDebugSession = true;
    auto *play = m_callbacks.play ? m_callbacks.play() : nullptr;
    m_debugSessionId = play ? play->session_id() : 0;
    m_scene.reset();
    transition(EditorScriptState::StartingDebug, "Waiting for Play debugger gate", true);
    return Result<void>::success();
}

Result<void> EditorScriptController::poll()
{
    if (std::this_thread::get_id() != m_owner)
        return Result<void>::failure({ErrorCategory::WrongThread, "EditorScript.owner"});
    // IDE 操作を挟んでも、古い Debug Session の所有権を次の通常 Play へ持ち越さない
    if (m_hasDebugSession)
    {
        const auto *play = m_callbacks.play ? m_callbacks.play() : nullptr;
        const bool isSameSession = play && play->is_debugging() && play->session_id() == m_debugSessionId;
        const bool isStopped =
            isSameSession && (play->state() == PlayState::Stopped || play->state() == PlayState::Failed);
        if (!isSameSession || isStopped)
        {
            m_hasDebugSession = false;
            m_debugSessionId = 0;
            if (isSameSession && play->state() == PlayState::Failed && m_request == Request::DebugPlay)
                return fail({ErrorCategory::PlatformFailure, std::string(play->error())});
            if (m_status.isBusy && m_request == Request::DebugPlay)
                return fail({ErrorCategory::PlatformFailure, "EditorScript.debug_session_ended"});
            if (m_status.state == EditorScriptState::Debugging)
                transition(EditorScriptState::Succeeded, "Debug Play stopped", false);
        }
    }
    auto ide = m_ide.poll();
    if (!ide.has_value() && m_status.isBusy)
        return fail(*ide.try_error());
    if (!m_status.isBusy && m_status.state != EditorScriptState::Debugging)
        return Result<void>::success();
    if (m_status.state == EditorScriptState::PreparingWorkspace || m_status.state == EditorScriptState::BuildingScripts)
    {
        const auto &build = m_build.status();
        if (build.state == GameBuildState::Failed)
            return fail(build.error.value_or(Error{ErrorCategory::PlatformFailure, "EditorScript.build_failed"}));
        if (build.state == GameBuildState::Cancelled)
            return cancel();
        if (build.state != GameBuildState::Succeeded)
            return Result<void>::success();
        m_ownsBuild = false;
        m_workspace = build.workspaceDirectory;
        if (m_workspace.is_empty() || m_workspace.utf8() != m_project.root().utf8())
            return fail({ErrorCategory::InvalidState, "EditorScript.workspace_mismatch"});
        if (m_request == Request::DebugPlay)
            return start_debug_child();
        if (m_request == Request::BuildScripts)
        {
            auto refreshed = m_callbacks.reload
                                 ? m_callbacks.reload()
                                 : Result<void>::failure({ErrorCategory::InvalidState, "EditorScript.reload"});
            if (!refreshed.has_value())
                return fail(*refreshed.try_error());
            transition(EditorScriptState::ReloadingScripts, "Reloading script types", true);
            return Result<void>::success();
        }
        auto opened = m_request == Request::OpenSource ? m_ide.open_source(m_workspace, m_source, 1)
                                                       : m_ide.open_folder(m_workspace);
        if (!opened.has_value())
            return fail(*opened.try_error());
        transition(EditorScriptState::OpeningIde, "Opening Visual Studio", true);
    }
    else if (m_status.state == EditorScriptState::OpeningIde || m_status.state == EditorScriptState::AttachingDebugger)
    {
        const auto &status = m_ide.status();
        if (status.state == IdeState::Failed)
            return fail(status.error.value_or(Error{ErrorCategory::PlatformFailure, "EditorScript.ide_failed"}));
        if (status.state == IdeState::Cancelled)
            return cancel();
        if (status.state != IdeState::Succeeded)
            return Result<void>::success();
        if (m_status.state == EditorScriptState::OpeningIde)
            transition(EditorScriptState::Succeeded, "Visual Studio ready", false);
        else
        {
            auto *play = m_callbacks.play ? m_callbacks.play() : nullptr;
            if (!play || play->process_id() != m_status.processId)
                return fail({ErrorCategory::InvalidState, "EditorScript.debug_process_changed"});
            auto continued = play->continue_debug();
            if (!continued.has_value())
                return fail(*continued.try_error());
            transition(EditorScriptState::StartingDebug, "Starting script under debugger", true);
        }
    }
    else
    {
        auto *play = m_callbacks.play ? m_callbacks.play() : nullptr;
        if (!play || play->state() == PlayState::Failed)
            return fail(
                {ErrorCategory::PlatformFailure, play ? std::string(play->error()) : "EditorScript.play_missing"});
        if (m_status.state == EditorScriptState::ReloadingScripts && play->state() == PlayState::Stopped)
            transition(EditorScriptState::Succeeded, "Game DLL built and script types reloaded", false);
        else if (play->state() == PlayState::WaitingForDebugger && m_status.state == EditorScriptState::StartingDebug)
        {
            m_status.processId = play->process_id();
            auto attached = m_ide.attach_debugger(m_workspace, m_status.processId, m_callbacks.playExecutable);
            if (!attached.has_value())
                return fail(*attached.try_error());
            transition(EditorScriptState::AttachingDebugger, "Attaching Visual Studio debugger", true);
        }
        else if (m_hasDebugSession && play->state() == PlayState::Running)
            transition(EditorScriptState::Debugging, "Debug Play running", false);
        else if (m_hasDebugSession && play->state() == PlayState::Stopped)
        {
            m_hasDebugSession = false;
            transition(EditorScriptState::Succeeded, "Debug Play stopped", false);
        }
    }
    return Result<void>::success();
}

const EditorScriptStatus &EditorScriptController::status() const noexcept
{
    return m_status;
}
} // namespace cue
