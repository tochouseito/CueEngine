#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>

#include <EditorHost/ImGuiManager.h>
#include <EditorHost/ScriptStatus.h>
#include <EditorIde/IdeService.h>
#include <Project/Project.h>
#include <ProjectBuild/GameBuildService.h>
#include <Runtime/WorldHandle.h>
#include <Scene/Scene.h>
#include <ScriptPlay/PlayClient.h>
#include <WindowsHost/WindowsHost.h>

namespace cue
{
class World;
class ISceneRenderer;
class EditorScriptController;
/// @brief 未保存 Scene を含む Build の応答。保存済み File の利用は編集内容を破棄しない
enum class GameBuildSaveChoice
{
    Save,
    SavedFiles,
    Cancel
};
/// @brief Project 切替前の編集内容の扱い。応答は Owner Thread で一度だけ消費する
enum class ProjectOpenSaveChoice
{
    Save,
    Discard,
    Cancel
};
/// @brief Editor 用の Window、Frame と表示の起動設定を所有する
struct EditorHostConfig final
{
    WindowDescriptor window{"CueEngine Editor", {1280, 720}};
    // FPS / 待機 / 同期を指定する。CPU 枠、Worker と先行数は gpu.resourceFrameCount から導出する
    FrameControllerDesc frame{2, true, 60};
    // SwapChain の Buffer 数、表示同期、Clear 色の起動設定
    PresentationConfig presentation;
    // Editor が生成した表示 Pass は抽象型で Windows 実行基盤へ渡す
    MainFrameGraphConfig graph;
    ImGuiManagerConfig imgui;
    // Callback は ImGui Context が Current の Owner Thread 上で実行する。未指定なら Test / TEST と FrameController FPS
    // を表示する
    editorUiCallback buildUi;
    // Editor が生成する設定・ログ・Cache の保存先
    StoragePathsConfig storage;
    std::unique_ptr<IFileSystem> fileSystem;
    HostLoggingConfig logging;
    // GPU 資源の枠数と、未完了提出数の上限
    GpuFrameConfig gpu;
    // 設定 UI と実行時要求で受け付ける変更範囲
    PerformanceSettingsPolicy performancePolicy;
    // FPS 上限とは独立したゲーム更新の周期設定
    SimulationConfig simulation;
    simulationCallback simulationUpdate;
    frameSnapshotCallback prepareFrame;
    // Adapter 比較などで Backend の生成だけを差し替える。所有権と FileSystem の借用は WindowsHost と同じ
    std::function<Result<std::unique_ptr<IBackend>>(IFileSystem *, Clock &)> createBackend;
    // 未指定なら下位 Host が所有する。指定時は Host の停止完了まで借用する
    Clock *clock = nullptr;
    // 読み込み済み Project を移譲する。Library の既存利用・Test は未指定で利用できる
    std::optional<Project> project;
    // 引数なしの実 Editor は空の編集画面を用意する。従来の Library 診断 UI は既定で維持する
    bool enableProjectOpening = false;
    // 未指定なら Windows の Folder Dialog。Callback は Window の Owner Thread で呼出中だけ借用する
    std::function<Result<std::optional<Path>>(Window &, const Path *)> projectDialog;
    // 起動前に検証した保存用 Scene。Preview World は Editor の Owner Thread で生成する
    std::optional<Scene> initialScene;
    // Library / Test は両方を明示して Project 成果物の解決を上書きできる。片方だけの指定は拒否する
    // 未指定時は Project の Game Target と選択 Game 構成の検証済み Manifest から解決する
    std::optional<Path> playExecutable;
    std::optional<Path> scriptModule;
    // Library / Test の初期選択。未指定なら Project のローカル設定、未保存なら Development
    std::optional<GameBuildConfiguration> gameConfiguration;
    // Authoring 用の非同期 Build を所有する。未指定でも Scene 編集と Play は利用できる
    std::unique_ptr<IGameBuildService> gameBuildService;
    std::string gameBuildUnavailableReason;
    std::unique_ptr<IEditorIdeService> ideService;
    std::string ideUnavailableReason;
};

/// @brief Editor の起動入口として Windows の表示・Frame 実行基盤を一意所有する
///
/// 全操作と破棄は構築 Thread で行い、再入しない
/// ImGuiManager を所有し、WindowsHost と Renderer は Editor に依存しない
class EditorHost final
{
  public:
    /// @brief Editor の起動設定を Windows 実行基盤へ所有値で渡す
    explicit EditorHost(EditorHostConfig a_config = {});

    /// @brief 明示停止がない場合も所有する Windows 実行基盤を停止する
    ~EditorHost();

    EditorHost(const EditorHost &) = delete;
    EditorHost &operator=(const EditorHost &) = delete;

    /// @brief Window、Renderer と Runtime を生成して Editor 用 Window を表示する
    ///
    /// 一度だけ呼べる。失敗時は部分資源を回収し、同じ Host の再初期化は拒否する
    [[nodiscard]] Result<void> initialize();

    /// @brief 読み込み済み Project を構築 Thread で借用する。次の Project 切替成功または停止まで有効
    [[nodiscard]] const Project *project() const noexcept;
    /// @brief 読み込んだ保存用 Scene を Owner Thread で借用する。次の編集・Project 切替・停止で失効する
    [[nodiscard]] const Scene *scene() const noexcept;
    /// @brief Project と開始 Scene の候補を検証し、保存確認または次の step での切替を予約する
    /// Owner Thread 専用。Build / Play / IDE 稼働中は拒否し、失敗時は現在の Document を維持する
    [[nodiscard]] Result<void> request_open_project(const Path &a_projectFile);
    /// @brief 保留した Project 切替へ一度だけ応答する。保存失敗・取消では切替しない
    [[nodiscard]] Result<void> confirm_open_project(ProjectOpenSaveChoice a_choice);
    /// @brief 未保存確認が Project 切替の応答を待っているか返す。Owner Thread 以外では false
    [[nodiscard]] bool has_pending_project_confirmation() const noexcept;
    /// @brief Project 切替の開始可否を Owner Thread へ返す。外部 Process の停止は行わない
    [[nodiscard]] bool can_open_project() const noexcept;
    /// @brief 編集表示用 World を Owner Thread で借用する。次の Project 切替成功または shutdown で失効する
    /// Update / Render Worker から直接使わない。Play 用 World は後続で別に生成する
    [[nodiscard]] World *preview_world() noexcept;
    /// @brief 編集 Scene の実体を Owner Thread へ返す。Handle は次の Project 切替・停止までの非所有参照
    [[nodiscard]] std::optional<SceneInstanceHandle> preview_instance() const noexcept;

    /// @brief 未保存 Scene を別 Process で開始する。Owner Thread 専用、初期化後かつ他の操作が停止中に呼ぶ
    /// Game DLL は読み込まず、Play の変更を編集 Document に反映しない。失敗時は編集内容を維持する
    [[nodiscard]] Result<void> start_play();
    /// @brief Game の構成だけを切り替え、Project 別ローカル設定へ保存する。Owner Thread 専用
    /// Build / Play / IDE / 保存確認中は拒否する。保存失敗時は元の選択・接続を保全する
    [[nodiscard]] Result<void> request_game_configuration(GameBuildConfiguration a_configuration);
    /// @brief Play / Script Build の選択を Owner に返す。Owner 以外・未初期化は Development
    [[nodiscard]] GameBuildConfiguration game_configuration() const noexcept;
    /// @brief 操作と Project の状態から構成変更の可否を返す。File I/O は行わない
    [[nodiscard]] bool can_select_game_configuration() const noexcept;
    /// @brief 名前を検証して Source と登録表を公開する。Owner 専用、Scene / Undo は変更しない
    /// Play / Build / IDE / 確認中は拒否する。失敗時は既存 Source と登録表を保持する
    [[nodiscard]] Result<void> request_script_creation(std::string_view a_name);
    /// @brief 所有 Play の停止を予約する。完了まで step で回収し、無応答の子だけを期限後に終了する
    [[nodiscard]] Result<void> stop_play();
    /// @brief 所有 Play の現在状態を返す。Owner 以外・未接続では Idle
    [[nodiscard]] PlayState play_state() const noexcept;
    /// @brief 最新の実行描画値を Owner に借用する。次の step / 停止 / Project 切替で失効する
    [[nodiscard]] const PlayViewSnapshot *play_view_snapshot() const noexcept;
    /// @brief 実行中 Object の姿勢を次の更新前に変更する。編集正本 / Undo は変更しない
    [[nodiscard]] Result<void> request_play_transform(std::string_view a_objectId,
                                                      const math::floatTransform &a_transform);

    /// @brief 選択対象を検証し、未保存なら ImGui 確認を予約する。Owner 専用、初期化後に呼ぶ
    /// 確認待ち・Build / Play / 製品の実行中は拒否する。保存前には外部 Process を開始しない
    [[nodiscard]] Result<void> request_game_build(GameBuildTarget a_target);
    /// @brief 保留した Build を一度だけ消費する。保存失敗・取消では開始せず、編集内容を保持する
    [[nodiscard]] Result<void> confirm_game_build(GameBuildSaveChoice a_choice);
    /// @brief 保留確認または実行中 Build を取り消す。製品アプリは停止しない。Owner 専用
    [[nodiscard]] Result<void> cancel_game_build();
    /// @brief 現在選択と一致する直前成功 Package を再検証して起動する。Owner 専用
    [[nodiscard]] Result<void> run_game();
    /// @brief 現在の表示値を借用する。Owner 専用、次の step / Build 操作で失効する
    [[nodiscard]] const GameBuildStatus *game_build_status() const noexcept;
    /// @brief Build の未保存確認が保留中か返す。Owner Thread 以外では false
    [[nodiscard]] bool has_pending_game_build_confirmation() const noexcept;
    /// @brief IDE Workspace の準備と起動を非待機で要求する。Owner Thread、初期化後専用
    [[nodiscard]] Result<void> open_ide();
    /// @brief Script の型 ID を Authoring Source へ解決して開く。Scene は変更しない
    [[nodiscard]] Result<void> edit_script(std::string_view a_typeId);
    /// @brief Native 構成のゲーム DLL を Build し、完了後だけ型情報を再読込する
    [[nodiscard]] Result<void> build_scripts();
    /// @brief 現在の Scene の所有 Snapshot で Debug Play を予約する。通常 Play と同時実行しない
    [[nodiscard]] Result<void> debug_play();
    /// @brief IDE / Script の保留要求と管理する Debug 開始を取り消す
    [[nodiscard]] Result<void> cancel_script_operation();
    /// @brief Owner Thread に現在値を借用する。次の step / 操作で失効する
    [[nodiscard]] const EditorScriptStatus *script_status() const noexcept;

    /// @brief 下位 Host の Logger を構築 Thread で借用する。停止完了で失効する
    [[nodiscard]] ILogger *logger() const noexcept;
    /// @brief 下位 Host が生成したログ File の Path を同じ借用期間で返す
    [[nodiscard]] const Path *log_file_path() const noexcept;

    /// @brief Window Message と Frame を進め、Close 要求時は false を返す
    ///
    /// 実行中だけ呼べる。失敗時も shutdown を呼び、停止を完了する
    [[nodiscard]] Result<bool> step();

    /// @brief 実行中の CPU Frame 進行状態を所有値で返す
    ///
    /// GPU 完了状態は表さない。起動前と停止後は InvalidState を返す
    [[nodiscard]] Result<FrameProgress> frame_progress() const;
    /// @brief Main / Update / Render / FPS 待機の直近 120 回の CPU 集計を返す
    [[nodiscard]] Result<FrameTimingInfo> frame_timing_info() const;
    /// @brief Window 入力到達から実際に UI へ反映した Frame の Present / CPU 完了までを返す
    [[nodiscard]] Result<InputLatencyInfo> input_latency_info() const;
    /// @brief UI 構築・Snapshot Copy・Context Lock 待機の CPU 集計を返す
    [[nodiscard]] Result<ImGuiTimingInfo> ui_timing_info() const;
    /// @brief UI 構築前に取得した Graph / Present / GPU 計測を返す。Callback 内でも Graph Lock を取得しない
    [[nodiscard]] Result<MainFrameGraphPerformance> graph_performance() const;

    /// @brief 全計測の所有 Snapshot を取得する。Owner 専用で停止後も最終値を取得できる
    [[nodiscard]] Result<ProfilerSnapshot> profiler_snapshot() const;

    /// @brief ゲームの公開範囲を検証して Main 境界での適用を要求する。UI Callback 内も可
    [[nodiscard]] Result<void> request_performance_settings(PerformanceSettings a_settings);
    /// @brief 適用済み設定の所有値を返す。Owner Thread 専用
    [[nodiscard]] Result<PerformanceSettings> performance_settings() const;
    /// @brief 要求と Native の実効値、表示統計不足による Fallback を取得する
    [[nodiscard]] Result<PerformanceSettingsStatus> performance_settings_status() const;
    /// @brief 要求の適用待ちを Owner Thread へ返す
    [[nodiscard]] bool has_pending_performance_settings() const noexcept;
    /// @brief 開発者の固定 / 可変更新方針を安全な Frame 境界で変更する
    [[nodiscard]] Result<void> request_simulation_config(SimulationConfig a_config);
    /// @brief Render Frame に対応する確定済み補間情報を Owner Thread へ返す
    [[nodiscard]] Result<FrameSimulationInfo> frame_simulation_info(std::uint64_t a_frame) const;

    /// @brief 確定済み UI Frame の描画数と入力 Capture を Owner Thread へ返す
    [[nodiscard]] Result<ImGuiFrameInfo> ui_frame_info() const;

    /// @brief CPU 描画 Snapshot の受渡し数を Owner Thread へ返す
    [[nodiscard]] Result<ImGuiTransferInfo> ui_transfer_info() const;

    /// @brief Frame 実行と GPU を停止してから Window を破棄する
    ///
    /// 複数回呼べる。UI 停止失敗では Window を保持し、次の呼出しで回収を再試行する
    [[nodiscard]] Result<void> shutdown();

  private:
    /// @brief Window の生存中に Context と Win32 入力接続を所有する
    [[nodiscard]] Result<void> initialize_ui(Window &a_window);

    /// @brief 採用 Frame の MainThread 段階で UI を構築し、Update 前に描画 Data を確定する
    [[nodiscard]] Result<void> build_ui(std::uint64_t a_frameIndex, std::stop_token a_stopToken);

    /// @brief UI の借用を終えた後、所有値の編集要求を順に確定する
    [[nodiscard]] Result<void> apply_editor_actions();
    /// @brief 非待機で Play 子 Process の状態と確認済み Property を回収する。失敗は Editor の通知に留める
    void poll_play();
    /// @brief 最小化中も Build の出力・終了を回収し、失敗を Editor 内の通知へ留める
    void poll_game_build();
    /// @brief Build / Play の観測後に IDE と Script の状態遷移を進める
    void poll_script_operations();
    /// @brief 保存確認を終えた選択対象を非待機で開始する。UI / File の借用を保持しない
    [[nodiscard]] Result<void> start_game_build();
    /// @brief 停止済みの接続を破棄し、開始時点の Project 成果物を再検証して PlayClient を準備する
    /// 失敗時は古い Metadata を残さない。稼働中の接続に対しては変更せず再入を拒否する
    [[nodiscard]] Result<void> prepare_play();
    /// @brief 未 Build の Project でも再試行操作を表示できるよう、接続設定の有無だけを返す
    [[nodiscard]] bool has_play_configuration() const noexcept;
    /// @brief 確認後に再開する Document 操作を識別する
    enum class DocumentChangeKind
    {
        OpenProject,
        NewScene,
        OpenScene,
        Exit,
        BuildGame,
    };
    /// @brief 編集を確定し、未保存なら操作を保留する。true はそのまま実行できることを示す
    [[nodiscard]] Result<bool> begin_document_change(DocumentChangeKind a_kind, std::optional<Path> a_file = {});
    /// @brief 確認済みの操作を実行する。失敗時も保留要求は再利用しない
    [[nodiscard]] Result<void> execute_document_change(DocumentChangeKind a_kind, const Path *a_file);
    /// @brief 保存先の選択と保存を実行し、File が公開できたか返す
    [[nodiscard]] Result<bool> save_editor_scene(bool a_chooseFile);
    /// @brief 選択・検証済みの候補を UI / Action の借用がない step 境界で公開する
    [[nodiscard]] Result<void> apply_project_change();
    /// @brief World と独立した Build / Play / IDE の稼働だけを Owner Thread で確認する
    [[nodiscard]] bool has_active_project_operations() const noexcept;
    /// @brief Project の安定した所有先を借用し、まだ要求を実行しない Script 接続を作る
    [[nodiscard]] Result<std::unique_ptr<EditorScriptController>> create_script_controller(const Project &a_project,
                                                                                           bool a_useOverrides = true);

    /// @brief Pass 破棄後に Handler を解除し、Window 破棄前に Context を解放する
    [[nodiscard]] Result<void> shutdown_ui();

    /// @brief 表示 Pass 未指定時に Manager を借用する ImGuiPass を生成する
    [[nodiscard]] MainFrameGraphConfig prepare_graph(MainFrameGraphConfig a_config);

    /// @brief 公開範囲と要求 / 適用状態を既定 UI に表示する
    [[nodiscard]] Result<void> build_performance_settings_ui();

    ImGuiManagerConfig m_imguiConfig;
    editorUiCallback m_buildUi;
    std::thread::id m_ownerId;
    bool m_isStepping = false;
    // Graph Lock と ImGui Context Lock の取得順を逆転させず、Main の UI 構築前に更新する
    MainFrameGraphPerformance m_graphPerformance;
    FrameTimingInfo m_frameTiming;
    InputLatencyInfo m_inputLatency;
    ImGuiTimingInfo m_uiTiming;
    PerformanceSettingsPolicy m_performancePolicy;
    // 実行値と分離して UI の未適用の編集内容を保持する
    std::optional<PerformanceSettings> m_settingsDraft;
    std::string m_settingsError;
    // Render の記録 Scope と Present 後の入力計測だけが同じ Thread で読み書きする
    // Worker の再構築は Join 後に行い、Main の ImGui Context Lock を再取得しない
    std::uint64_t m_inputRenderFrame = 0;
    bool m_wasInputRendered = false;
    // 表示構成を明示した利用者には、未接続の Scene Texture を持つ既定 UI を注入しない
    bool m_hasSceneView = false;
    // WindowsHost を先に破棄し、Callback と Pass の参照先を最後まで生存させる
    std::unique_ptr<ImGuiManager> m_imgui;
    std::unique_ptr<ISceneRenderer> m_sceneRenderer;
    // 下位 Host の Callback と UI が参照するため WindowsHost より長く保持する
    std::unique_ptr<Project> m_project;
    std::optional<Scene> m_scene;
    std::optional<Path> m_playExecutable;
    std::optional<Path> m_scriptModule;
    std::optional<GameBuildConfiguration> m_initialGameConfiguration;
    std::unique_ptr<IGameBuildService> m_gameBuildService;
    std::string m_gameBuildUnavailableReason;
    std::unique_ptr<IEditorIdeService> m_ideService;
    std::string m_ideUnavailableReason;
    bool m_enableProjectOpening = false;
    bool m_isPreparingProject = false;
    std::function<Result<std::optional<Path>>(Window &, const Path *)> m_projectDialog;
    // Project の住所を維持したまま Controller と Document をまとめて公開する
    struct PreparedProject;
    std::unique_ptr<PreparedProject> m_pendingProject;
    bool m_isProjectChangeConfirmed = false;
    // Editor / EditorUi の型と Native Dialog 状態は公開 Header に漏らさない
    struct EditingState;
    std::unique_ptr<EditingState> m_editing;
    bool m_hasInitializeAttempt = false;
    bool m_isInitializing = false;
    bool m_isClosed = false;
    WindowsHost m_windows;
};
} // namespace cue
