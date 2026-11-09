#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <Foundation/Result.h>
#include <Math/Transform.h>
#include <ProjectBuild/BuildTarget.h>
#include <ScriptData/ScriptData.h>

namespace cue
{
class SceneDocument;
struct SceneDescriptor;

/// @brief UI が要求する操作。Native Dialog / World / FileSystem の実行は Host と Editor が担当する
enum class EditorActionKind
{
    OpenProject,
    NewScene,
    OpenScene,
    SaveScene,
    SaveSceneAs,
    AddCube,
    SelectObject,
    BeginTransformEdit,
    PreviewTransform,
    CommitTransformEdit,
    CancelTransformEdit,
    Undo,
    Redo,
    ToggleDiagnostics,
    Exit,
    ConfirmSaveChanges,
    ConfirmDiscardChanges,
    CancelDocumentChange,
    ReloadScripts,
    Play,
    Stop,
    AddScript,
    RemoveScript,
    SetScriptEnabled,
    SetScriptProperty,
    SetGameBuildTarget,
    BuildGame,
    CancelGameBuild,
    RunGame,
    OpenIde,
    OpenScriptSource,
    BuildScripts,
    DebugPlay,
    CancelScriptOperation,
    SetPlayTransform,
    SetSceneViewCamera,
    ResetSceneViewCamera,
};

/// @brief UI の借用が終わった後に適用できる所有値の操作要求
struct EditorAction final
{
    EditorActionKind kind = EditorActionKind::SelectObject;
    std::string objectId;
    math::floatTransform transform;
    std::string scriptTypeId;
    std::string scriptInstanceId;
    ScriptPropertyValue property;
    bool isEnabled = true;
    GameBuildTarget buildTarget;
};

/// @brief Build の表示用 Snapshot。View / Span は draw の間だけ Host から借用する
/// 選択 Target は独立した所有値で、UI 変更は Action にして返す。Process / FileSystem は操作しない
struct EditorBuildContext final
{
    std::span<const GameBuildPlatform> platforms;
    GameBuildTarget selectedTarget;
    bool canBuild = false;
    bool isBuilding = false;
    bool canCancel = false;
    bool canRun = false;
    bool isApplicationRunning = false;
    bool isLogTruncated = false;
    std::string_view statusText;
    std::string_view logText;
    std::string_view outputDirectory;
    std::string_view startupScene;
    std::optional<std::uint32_t> exitCode;
};

/// @brief IDE / Script 操作の表示値。Source の解決や外部 Process 操作は Host へ要求する
struct EditorScriptContext final
{
    bool canOpenIde = false;
    bool canEditSource = false;
    bool canBuild = false;
    bool canDebugPlay = false;
    bool canCancel = false;
    bool isBusy = false;
    bool isWaitingForDebugger = false;
    bool isDebuggerAttached = false;
    std::uint32_t processId = 0;
    std::string_view statusText;
};

/// @brief Host が UI の構築中だけ渡す表示値。Texture ID は Native Descriptor ではなく論理参照
struct EditorUiContext final
{
    // Project 未選択でも File Menu を提供し、Scene に属する操作は抑止する
    bool isProjectOpen = true;
    bool canOpenProject = true;
    std::uint64_t sceneTextureId = 0;
    float displayWidth = 1280;
    float displayHeight = 720;
    std::string_view errorText;
    std::string_view noticeText;
    bool isSceneViewEnabled = true;
    // Editor の表示 Camera だけを切り替え、保存 Scene / 実行 World の Camera は変更しない
    std::optional<math::floatTransform> sceneViewCamera;
    math::floatTransform sceneCameraTransform{{0, 0, 3}};
    // true の間は背景操作を抑止し、未保存確認の選択を一度だけ返す
    bool isUnsavedConfirmationRequested = false;
    // Metadata は別 Process から受け取った所有値を draw 中だけ借用する
    std::span<const ScriptTypeDescriptor> scriptTypes;
    bool canPlay = false;
    bool isPlaying = false;
    bool isPlayBusy = false;
    bool canStop = false;
    std::uint64_t playSessionId = 0;
    // Play の設定表示は編集正本と分離し、確認済みの変更だけを反映する
    const SceneDescriptor *playScene = nullptr;
    EditorBuildContext build;
    // 保存済み File の Build は編集正本を破棄しないため、通常の Discard と文言を分ける
    bool isBuildConfirmationRequested = false;
    EditorScriptContext script;
};

/// @brief ImGui の Panel と入力中の表示値を所有する。Scene や Preview World は変更しない
/// Host が Main Thread で所有し、Current な ImGui Context 内の build_frame から draw を呼ぶ
class EditorUi final
{
    struct CreateToken final
    {
    };

  public:
    /// @brief Factory だけが未編集状態を構築する
    explicit EditorUi(CreateToken) noexcept;
    /// @brief Scene に依存しない UI 状態を生成する
    [[nodiscard]] static Result<std::unique_ptr<EditorUi>> create();
    /// @brief 入力中の編集 Buffer を解放する。ImGui Context より先に破棄する必要はない
    ~EditorUi();
    /// @brief 編集セッションの複製を禁止する
    EditorUi(const EditorUi &) = delete;
    /// @brief 編集セッションの複製を禁止する
    EditorUi &operator=(const EditorUi &) = delete;
    /// @brief 所有値の要求だけを返す。Document の次の変更より前に、この借用を終える
    [[nodiscard]] Result<std::vector<EditorAction>> draw(const SceneDocument &a_document,
                                                         const EditorUiContext &a_context);
    /// @brief 未選択時は nullptr を受け、仮 Document を作らず空の Editor と Project を開く操作を表示する
    /// Document / Context は呼出中だけ借用する。Current な ImGui Context と Main Thread を必要とする
    [[nodiscard]] Result<std::vector<EditorAction>> draw(const SceneDocument *a_document,
                                                         const EditorUiContext &a_context);
    /// @brief 入力 Buffer と確認応答を再同期し、次の操作・確認を受け付ける状態へ戻す
    /// Context 外から呼べる。表示中 Popup の回収は次の draw で行う
    void reset() noexcept;

  private:
    struct State;
    std::unique_ptr<State> m_state;
};
} // namespace cue
