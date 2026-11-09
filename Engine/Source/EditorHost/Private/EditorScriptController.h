#pragma once

#include <functional>
#include <optional>
#include <string_view>
#include <thread>

#include <EditorHost/ScriptStatus.h>
#include <EditorIde/IdeService.h>
#include <Project/Project.h>
#include <ProjectBuild/GameBuildService.h>
#include <Scene/Scene.h>
#include <ScriptPlay/PlayClient.h>

namespace cue
{
class IFileSystem;
/// @brief Host が所有する接続を操作中だけ借用する。Callback は Controller より長く有効にする
struct EditorScriptCallbacks final
{
    std::function<PlayClient *()> play;
    std::function<Result<void>()> reload;
    std::function<Result<void>(const Scene &)> startDebug;
    Path playExecutable;
};
/// @brief Build / Play / IDE の順序だけを管理する。全操作と破棄は生成 Thread で直列化する
/// 借用 Service と Project / FileSystem は Controller より長く保持する。Runtime World は所有しない
class EditorScriptController final
{
  public:
    /// @brief Host の長寿命 Service と Owner Thread の Callback を借用する
    EditorScriptController(const Project &a_project, IFileSystem &a_files, IGameBuildService &a_build,
                           IEditorIdeService &a_ide, EditorScriptCallbacks a_callbacks);
    /// @brief 新規要求と対象を複製しない
    EditorScriptController(const EditorScriptController &) = delete;
    EditorScriptController &operator=(const EditorScriptController &) = delete;
    /// @brief 専用 Workspace を再生成して Visual Studio へ開く
    [[nodiscard]] Result<void> open_folder();
    /// @brief Editor 専用 Source Map から解決し、対応 Source を開く
    [[nodiscard]] Result<void> open_script(std::string_view a_typeId);
    /// @brief Native Editor 構成の DLL を Build し、成功後だけ型情報を再読込する
    [[nodiscard]] Result<void> build_scripts();
    /// @brief 要求時の所有 Scene を保持し、Build / 子待機 / Attach / 開始の順に進める
    [[nodiscard]] Result<void> debug_play(const Scene &a_scene);
    /// @brief Bridge と所有 Build を取り消し、管理中の Debug Play へ Stop を要求する
    [[nodiscard]] Result<void> cancel();
    /// @brief Host が Build / Play を poll した後、IDE と状態遷移を非待機で進める
    [[nodiscard]] Result<void> poll();
    /// @brief Owner Thread が次の操作まで有効な表示値を借用する
    [[nodiscard]] const EditorScriptStatus &status() const noexcept;

  private:
    enum class Request
    {
        OpenFolder,
        OpenSource,
        BuildScripts,
        DebugPlay
    };
    /// @brief 所有・再入・既存の Build / IDE と Play の前提を確認する
    [[nodiscard]] Result<void> check_start(bool a_allowPlay) const;
    /// @brief 未完要求を部分公開せず、Native 対象の Build を開始する
    [[nodiscard]] Result<void> start(Request a_request);
    /// @brief 回復可能な要求失敗を状態へ保持し、関連する待機を取り消す
    [[nodiscard]] Result<void> fail(Error a_error);
    /// @brief 画面表示と Busy を一つの境界で更新する
    void transition(EditorScriptState a_state, const char *a_message, bool a_busy);
    /// @brief 所有 Scene の Debug 子起動を要求する
    [[nodiscard]] Result<void> start_debug_child();
    const Project &m_project;
    IFileSystem &m_files;
    IGameBuildService &m_build;
    IEditorIdeService &m_ide;
    EditorScriptCallbacks m_callbacks;
    std::thread::id m_owner;
    EditorScriptStatus m_status;
    Request m_request = Request::OpenFolder;
    Path m_workspace;
    Path m_source;
    std::optional<Scene> m_scene;
    bool m_ownsBuild = false;
    bool m_hasDebugSession = false;
    std::uint64_t m_debugSessionId = 0;
};
} // namespace cue
