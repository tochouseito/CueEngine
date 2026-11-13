#include <EditorHost/EditorHost.h>

#include <Profiler/TimingTrace.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <new>
#include <unordered_map>
#include <utility>

#include <imgui.h>

#include <Editor/EditorApplication.h>
#include <EditorHost/ImGuiPass.h>
#include <EditorHost/PerformanceSettingsEditing.h>
#include <EditorIde/ScriptCreation.h>
#include <EditorUi/EditorUi.h>
#include <EngineInstallation/EngineVersion.h>
#include <Foundation/ScopedFlag.h>
#include <Math/Projection.h>
#include <Platform/Diagnostics.h>
#include <Runtime/World.h>
#include <SceneRender/SceneRenderer.h>
#include <ScriptPlay/PlayClient.h>

#include "EditorGameSettings.h"
#include "EditorGitHead.h"
#include "EditorPlayServices.h"
#include "EditorScriptController.h"
#include "EditorWindowsServices.h"

namespace cue
{
namespace
{
/// @brief Sample がある CPU 区間の平均・p95・最大経過時間を ms で表示する
void show_timing(const char *a_name, const TimingStatistics &a_statistics)
{
    // 標本がある項目だけを時間値として表示する
    if (a_statistics.sampleCount)
    {
        ImGui::Text("%s: avg %.3f / p95 %.3f / max %.3f ms (%zu samples)", a_name,
                    std::chrono::duration<double, std::milli>(a_statistics.averageDuration).count(),
                    std::chrono::duration<double, std::milli>(a_statistics.p95Duration).count(),
                    std::chrono::duration<double, std::milli>(a_statistics.maxDuration).count(),
                    a_statistics.sampleCount);
    }
    else
    {
        ImGui::Text("%s: --", a_name);
    }
}

/// @brief Main が取得した計測 Snapshot を既定 Test Window に表示する
[[nodiscard]] Result<void> build_test_window(const FrameTimingInfo &a_frameTiming, const ImGuiTimingInfo &a_uiTiming,
                                             const MainFrameGraphPerformance &a_graphPerformance,
                                             const InputLatencyInfo &a_inputLatency, const Project *a_project,
                                             const Scene *a_scene, EditorLanguage a_language)
{
    const auto text = [a_language](const char *a_english, const char *a_japanese)
    { return editor_text(a_language, a_english, a_japanese); };
    // 初回の位置と寸法だけ指定し、以後の移動と Layout 保存を妨げない
    ImGui::SetNextWindowPos({32.0f, 32.0f}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize({640.0f, 480.0f}, ImGuiCond_FirstUseEver);
    if (ImGui::Begin(text("Test", "診断###Test")))
    {
        ImGui::Text(text("TEST", "診断情報"));
        // Project 設定は Host が所有し、UI へは不変の表示値だけを借用する
        if (a_project)
        {
            ImGui::Text(text("Project: %s", "プロジェクト: %s"), a_project->descriptor().name.c_str());
        }
        if (a_scene)
            ImGui::Text(text("Scene: %s (%zu objects)", "シーン: %s（%zu オブジェクト）"),
                        a_scene->descriptor().name.c_str(), a_scene->descriptor().objects.size());
        // 開始制限後も Render 完了時刻は揺れるため、直近の平均間隔から完了頻度を求める
        // 各 Frame の FPS を平均せず、Sample 数 / 合計経過時間と等価な値を使う
        const auto &intervals = a_frameTiming.frameInterval;
        // 平均 Frame 間隔の逆数から平均 FPS を求める
        const auto interval = std::chrono::duration<double>(intervals.averageDuration).count();
        if (intervals.sampleCount != 0 && interval > 0.0)
        {
            ImGui::Text(
                text("FrameController FPS: %.1f (avg %zu intervals)", "FrameController FPS: %.1f（%zu 区間の平均）"),
                1.0 / interval, intervals.sampleCount);
        }
        else
        {
            // 二つの Render 完了点が揃うまでは FPS を計算せず、起動直後の零除算を避ける
            ImGui::Text("FrameController FPS: --");
        }
        // 平均 FPS と同じ Render 完了間隔の最新 Sample から、瞬間的な揺れを表示する
        const auto lastInterval = std::chrono::duration<double>(intervals.lastDuration).count();
        if (intervals.sampleCount != 0 && lastInterval > 0.0)
        {
            ImGui::Text(text("FrameController FPS (instant): %.1f", "FrameController FPS（瞬間）: %.1f"),
                        1.0 / lastInterval);
        }
        else
        {
            ImGui::Text(text("FrameController FPS (instant): --", "FrameController FPS（瞬間）: --"));
        }
        // GPU 時間と混同しないよう CPU の経過時間であることと集計枠数を表示する
        ImGui::Text(text("CPU timings (last 120 samples)", "CPU 計測（直近 120 件）"));
        show_timing("Main", a_frameTiming.main);
        show_timing("Update", a_frameTiming.update);
        show_timing("Render", a_frameTiming.render);
        // 平均 FPS だけでは見えない Frame Pacing の揺れを実際の完了間隔で確認する
        show_timing(text("Frame interval", "フレーム間隔"), intervals);
        show_timing(text("FPS wait", "FPS 制限の待機"), a_frameTiming.limitWait);
        show_timing(text("Post-Present readiness wait", "Present 後の準備待ち"), a_frameTiming.presentationWait);
        show_timing(text("Cycle interval", "サイクル間隔"), a_frameTiming.cycleInterval);
        show_timing(text("Frame adopted -> Render start", "フレーム採用 → Render 開始"),
                    a_frameTiming.inputToRenderStart);
        show_timing(text("Frame adopted -> CPU Render complete", "フレーム採用 → CPU Render 完了"),
                    a_frameTiming.inputToRenderComplete);
        ImGui::TextUnformatted(text("Input event latency (last 120 reflected events, CPU only)",
                                    "入力遅延（直近 120 件の反映済みイベント、CPU のみ）"));
        ImGui::TextUnformatted(
            text("Origin: Window message dispatch; Present return is not screen display",
                 "計測起点: ウィンドウメッセージの配信。実際の表示には Present 復帰後の時間も含まれます。"));
        show_timing(text("Input -> Frame capture", "入力 → フレーム採取"), a_inputLatency.toFrameCapture);
        show_timing(text("Input -> Render start", "入力 → Render 開始"), a_inputLatency.toRenderStart);
        show_timing(text("Input -> Present call", "入力 → Present 呼び出し"), a_inputLatency.toPresentStart);
        show_timing(text("Input -> Present return", "入力 → Present 復帰"), a_inputLatency.toPresentReturn);
        show_timing(text("Input -> CPU Render complete", "入力 → CPU Render 完了"), a_inputLatency.toCpuComplete);
        ImGui::Text(text("Input observed / reflected / presented: %llu / %llu / %llu",
                         "入力の検出 / 反映 / 提出: %llu / %llu / %llu"),
                    static_cast<unsigned long long>(a_inputLatency.observedEvents),
                    static_cast<unsigned long long>(a_inputLatency.assignedEvents),
                    static_cast<unsigned long long>(a_inputLatency.presentedEvents));
        ImGui::Text(text("Input skipped / invalid: %llu / %llu; last event / frame / present: %llu / %llu / %llu",
                         "スキップ / 無効な入力: %llu / %llu、最新のイベント / フレーム / Present: %llu / %llu / %llu"),
                    static_cast<unsigned long long>(a_inputLatency.skippedEvents),
                    static_cast<unsigned long long>(a_inputLatency.invalidEvents),
                    static_cast<unsigned long long>(a_inputLatency.lastEventId),
                    static_cast<unsigned long long>(a_inputLatency.lastFrame),
                    static_cast<unsigned long long>(a_inputLatency.lastPresent));
        show_timing(text("UI build", "UI 構築"), a_uiTiming.uiBuild);
        show_timing(text("Snapshot copy", "スナップショットのコピー"), a_uiTiming.snapshotCopy);
        show_timing(text("ImGui lock wait", "ImGui ロック待ち"), a_uiTiming.contextWait);
        show_timing(text("ImGui GPU fence wait", "ImGui GPU フェンス待ち"), a_uiTiming.gpuWait);
        show_timing(text("Graph record", "グラフのコマンド記録"), a_graphPerformance.record);
        show_timing(text("Graph frame fence wait", "グラフのフレームフェンス待ち"), a_graphPerformance.frameWait);
        show_timing(text("Present total", "Present 全体"), a_graphPerformance.present);
        show_timing("Present API", a_graphPerformance.presentApi);
        show_timing(text("Presentation statistics", "表示統計の取得"), a_graphPerformance.presentationStatistics);
        show_timing(text("Main admission wait (per check)", "Main の開始待ち（確認単位）"),
                    a_graphPerformance.admissionWait);
        show_timing(text("Main GPU wait (per check)", "Main の GPU 待ち（確認単位）"), a_graphPerformance.mainGpuWait);
        show_timing(text("Main DXGI wait (per check)", "Main の DXGI 待ち（確認単位）"),
                    a_graphPerformance.mainPresentationWait);
        show_timing(text("Main resource wait (per check)", "Main のリソース待ち（確認単位）"),
                    a_graphPerformance.mainResourceWait);
        show_timing(text("GPU queue limit wait", "GPU キュー上限の待機"), a_graphPerformance.gpuQueueWait);
        show_timing(text("Pre-record readiness wait", "コマンド記録前の準備待ち"),
                    a_graphPerformance.presentationReadyWait);
        show_timing(text("Present return interval", "Present 復帰の間隔"), a_graphPerformance.presentInterval);
        show_timing(text("Display interval (DXGI derived)", "表示間隔（DXGI から算出）"),
                    a_graphPerformance.displayInterval);
        ImGui::Text(text("Unfinished GPU frames: %u / peak %u", "GPU 未完了フレーム数: %u / 最大 %u"),
                    a_graphPerformance.unfinishedGpuFrames, a_graphPerformance.peakUnfinishedGpuFrames);
        ImGui::Text(text("DXGI display timing: %s", "DXGI の表示時刻: %s"),
                    !a_graphPerformance.presentation.hasDisplayTiming ? text("unavailable", "取得不可")
                    : a_graphPerformance.presentation.isDisplayTimeEstimated
                        ? text("estimated from refresh", "リフレッシュ間隔から推定")
                        : text("QPC sample", "QPC の計測値"));
        ImGui::Text(text("GPU timings (last 120 completed samples)", "GPU 計測（直近 120 件の完了分）"));
        // GPU 時間は回収済み標本のある Pass だけを表示する
        for (const auto &pass : a_graphPerformance.gpuPasses)
        {
            if (pass.isAvailable)
            {
                show_timing(pass.name.c_str(), pass.statistics);
            }
            else
            {
                ImGui::Text("%s: --", pass.name.c_str());
            }
        }
    }
    ImGui::End();
    return Result<void>::success();
}

/// @brief Graph が Manager を直接所有せず、Editor の所有先から実記録を借用する
class EditorImGuiRenderer final : public IImGuiRenderer
{
  public:
    /// @brief EditorHost 内の安定した所有先を Graph 破棄まで借用する
    explicit EditorImGuiRenderer(const std::unique_ptr<ImGuiManager> &a_manager) noexcept : m_manager(&a_manager)
    {
    }

    /// @brief Graph 記録時に生成済み Manager を検証し、公式 DX12 記録入口へ委譲する
    [[nodiscard]] Result<void> record_draw_data(FrameGraphContext &a_context) override
    {
        return *m_manager ? (*m_manager)->record_draw_data(a_context)
                          : Result<void>::failure({ErrorCategory::InvalidState, "EditorImGuiRenderer.manager"});
    }

    /// @brief 今回の物理 FinalColor を Scene View の論理参照へ解決して記録する
    [[nodiscard]] Result<void> record_scene_draw_data(FrameGraphContext &a_context,
                                                      FrameGraphResourceHandle a_source) override
    {
        return *m_manager ? (*m_manager)->record_draw_data(a_context, a_source)
                          : Result<void>::failure({ErrorCategory::InvalidState, "EditorImGuiRenderer.manager"});
    }

  private:
    const std::unique_ptr<ImGuiManager> *m_manager;
};

} // namespace

/// @brief 編集正本と Panel の一時状態を所有し、Native Window は稼働中だけ借用する
struct EditorHost::EditingState final
{
    /// @brief Modal の応答まで所有し、File 選択時の借用を Frame 間へ持ち越さない
    struct PendingChange final
    {
        DocumentChangeKind kind;
        std::optional<Path> file;
    };
    std::unique_ptr<EditorApplication> application;
    std::unique_ptr<EditorUi> ui;
    Window *window = nullptr;
    std::vector<EditorAction> actions;
    std::string error;
    std::string notice;
    EditorLanguage language = EditorLanguage::English;
    bool isCloseConfirmed = false;
    bool isDiagnosticsVisible = false;
    std::optional<PendingChange> pendingChange;
    std::unique_ptr<PlayClient> play;
    // Play 時点の Scene 設定を保持し、実行中の変更を編集正本へ混ぜない
    std::optional<SceneDescriptor> playScene;
    std::unordered_map<std::string, std::size_t> playObjectIndices;
    std::optional<std::uint64_t> appliedViewTick;
    // Editor の Camera だけを上書きし、編集用 / 実行用 World の Camera は変更しない
    std::optional<math::floatTransform> sceneViewCamera;
    GameBuildTarget buildTarget;
    std::string buildStatusText;
    std::unique_ptr<EditorScriptController> scripts;
    std::string scriptStatusText;
    GameBuildConfiguration gameConfiguration = GameBuildConfiguration::Development;
    bool isGameBuildRequired = true;
    std::optional<std::string> gitHead;
    std::chrono::steady_clock::time_point nextGitCheck{};
    bool isGitReloadPending = false;
    std::optional<Path> sceneBeforeCheckout;
    // 構成ごとに実際に Build した HEAD を保持し、同じ出力 Path に残る旧 DLL を拒否する
    std::unordered_map<std::string, std::array<bool, 3>> staleBranchBuilds;
};

/// @brief 準備中の Project の住所を固定し、Controller の借用を commit の前後で維持する
struct EditorHost::PreparedProject final
{
    std::unique_ptr<Project> project;
    std::unique_ptr<EditorApplication> application;
    std::unique_ptr<EditorScriptController> scripts;
    GameBuildConfiguration gameConfiguration = GameBuildConfiguration::Development;
    std::string settingsError;
    std::optional<std::string> gitHead;
    bool isGitReload = false;
};

/// @brief Editor の表示設定を保持し、下位基盤へ Editor 型を渡さない
EditorHost::EditorHost(EditorHostConfig a_config)
    : m_imguiConfig(std::move(a_config.imgui)),
      m_buildUi(a_config.buildUi ? std::move(a_config.buildUi)
                                 : editorUiCallback{[this]() -> Result<void>
                                                    {
                                                        if (m_editing && !m_editing->isDiagnosticsVisible)
                                                            return Result<void>::success();
                                                        // UI 開始前に取得した同じ計測 Snapshot で FPS と区間を表示する
                                                        auto result = build_test_window(
                                                            m_frameTiming, m_uiTiming, m_graphPerformance,
                                                            m_inputLatency, project(), scene(),
                                                            m_editing ? m_editing->language : EditorLanguage::English);
                                                        return result.has_value() ? build_performance_settings_ui()
                                                                                  : std::move(result);
                                                    }}),
      m_ownerId(std::this_thread::get_id()), m_performancePolicy(a_config.performancePolicy),
      m_project(a_config.project ? std::make_unique<Project>(std::move(*a_config.project)) : nullptr),
      m_scene(std::move(a_config.initialScene)), m_playExecutable(std::move(a_config.playExecutable)),
      m_scriptModule(std::move(a_config.scriptModule)), m_initialGameConfiguration(a_config.gameConfiguration),
      m_gameBuildService(std::move(a_config.gameBuildService)),
      m_gameBuildUnavailableReason(std::move(a_config.gameBuildUnavailableReason)),
      m_ideService(std::move(a_config.ideService)), m_ideUnavailableReason(std::move(a_config.ideUnavailableReason)),
      m_enableProjectOpening(a_config.enableProjectOpening), m_projectDialog(std::move(a_config.projectDialog)),
      m_windows({std::move(a_config.window),
                 frame_config_for_buffering(a_config.frame, a_config.gpu.resourceFrameCount),
                 a_config.presentation,
                 prepare_graph(std::move(a_config.graph)),
                 {[this](Window &a_window) { return initialize_ui(a_window); },
                  {},
                  [this]() { return shutdown_ui(); },
                  [this](IBackend &a_backend, std::uint32_t a_cpuFrames, std::uint32_t a_gpuFrames)
                  {
                      // GPU Backend の生成後に CPU Snapshot の転送枠を用意する
                      auto result = m_imgui->initialize_renderer(a_backend, a_gpuFrames);
                      if (!result.has_value())
                          return result;
                      if (preview_world() || m_enableProjectOpening)
                      {
                          auto renderer = create_scene_renderer(a_backend);
                          if (!renderer.has_value())
                              return Result<void>::failure(*renderer.try_error());
                          m_sceneRenderer = renderer.take_value();
                      }
                      return m_imgui->enable_frame_transfer(a_cpuFrames);
                  },
                  [this](std::uint64_t a_frame, std::stop_token a_token) { return build_ui(a_frame, a_token); },
                  [this](std::uint64_t a_frame, std::stop_token a_token, const FrameCallback &a_record)
                  {
                      // この Frame の UI が記録されたかを描画 Scope の結果として保持する
                      const auto record = [&]()
                      { return m_imgui->render_frame(a_frame, a_token, a_record, &m_wasInputRendered); };
                      // Main の公開前に取消された Frame は Scene Snapshot の存在を要求しない
                      auto result = m_sceneRenderer && !a_token.stop_requested()
                                        ? m_sceneRenderer->render_frame(a_frame, record)
                                        : record();
                      m_inputRenderFrame = a_frame;
                      return result;
                  },
                  [this](IBackend &a_backend, std::uint32_t a_cpuFrames, std::uint32_t a_gpuFrames)
                  {
                      if (m_sceneRenderer)
                      {
                          auto discarded = m_sceneRenderer->discard_pending();
                          if (!discarded.has_value())
                              return discarded;
                      }
                      return m_imgui->reconfigure_renderer(a_backend, a_cpuFrames, a_gpuFrames);
                  },
                  std::move(a_config.simulationUpdate),
                  std::move(a_config.prepareFrame),
                  [this]() { return m_imgui->is_renderer_ready(); },
                  [this](std::vector<InputEventTiming> &a_events) { return m_imgui->take_input_events(a_events); },
                  [this](std::uint64_t a_frame) -> Result<bool>
                  {
                      return a_frame == m_inputRenderFrame
                                 ? Result<bool>::success(m_wasInputRendered)
                                 : Result<bool>::failure({ErrorCategory::InvalidState, "EditorHost.input_frame"});
                  },
                  std::move(a_config.createBackend),
                  [this]() -> Result<bool>
                  {
                      if (!m_editing || m_editing->isCloseConfirmed)
                          return Result<bool>::success(true);
                      // Windows Close は一度取り消して Frame を継続し、ImGui の応答後に再要求する
                      auto confirmed = begin_document_change(DocumentChangeKind::Exit);
                      if (!confirmed.has_value())
                      {
                          m_editing->error = confirmed.try_error()->operation;
                          return Result<bool>::success(false);
                      }
                      return confirmed;
                  }},
                 std::move(a_config.storage),
                 std::move(a_config.fileSystem),
                 a_config.logging,
                 a_config.gpu,
                 a_config.performancePolicy,
                 a_config.simulation,
                 a_config.clock})
{
}

/// @brief 下位の所有 Logger を構築 Thread へ借用する
ILogger *EditorHost::logger() const noexcept
{
    return m_windows.logger();
}

const Path *EditorHost::log_file_path() const noexcept
{
    return m_windows.log_file_path();
}

/// @brief 実行資源の停止後も残る共通 Profiler の集計を取得する
Result<ProfilerSnapshot> EditorHost::profiler_snapshot() const
{
    return m_windows.profiler_snapshot();
}

/// @brief Editor の表示を抽象 Pass として下位 Host へ渡し、明示された Pass は優先する
MainFrameGraphConfig EditorHost::prepare_graph(MainFrameGraphConfig a_config)
{
    auto configure = std::move(a_config.configure);
    a_config.configure = [this, configure = std::move(configure)](FrameGraph &a_graph,
                                                                  FrameGraphResourceHandle a_finalColor) -> Result<void>
    {
        if (configure)
        {
            auto configured = configure(a_graph, a_finalColor);
            if (!configured.has_value())
                return configured;
        }
        if (!m_sceneRenderer)
            return Result<void>::success();
        auto pass = m_sceneRenderer->create_pass();
        if (!pass.has_value())
            return Result<void>::failure(*pass.try_error());
        auto added = a_graph.add_pass(pass.take_value());
        return added.has_value() ? Result<void>::success() : Result<void>::failure(*added.try_error());
    };
    // 表示 Pass が未指定のときだけ Editor の ImGuiPass を注入する
    if (!a_config.displayPass && !a_config.displayPassFactory)
    {
        m_hasSceneView = true;
        // Resize 後の新しい Graph Handle を持つ Pass を作る。Manager と Texture Heap は継続所有する
        a_config.displayPassFactory = [this]()
        {
            return std::make_unique<ImGuiPass>(std::make_unique<EditorImGuiRenderer>(m_imgui),
                                               preview_world() != nullptr || m_enableProjectOpening);
        };
    }
    return a_config;
}

/// @brief WindowsHost と GPU の借用を止めてから UI の所有先を破棄する
EditorHost::~EditorHost()
{
    auto result = shutdown();
    if (!result.has_value())
    {
        report_log_error("CueEditorHost cleanup", *result.try_error(), LogLevel::Error);
    }
}

/// @brief Windows 実行基盤の一度だけの初期化と失敗回収を利用する
Result<void> EditorHost::initialize()
{
    if (std::this_thread::get_id() != m_ownerId)
        return Result<void>::failure({ErrorCategory::WrongThread, "EditorHost.initialize"});
    if (m_hasInitializeAttempt || m_isClosed || m_isInitializing || m_isStepping)
        return Result<void>::failure({ErrorCategory::InvalidState, "EditorHost.initialize"});
    ScopedFlag initializing(m_isInitializing);
    m_hasInitializeAttempt = true;
    if (m_initialGameConfiguration && game_build_configuration_name(*m_initialGameConfiguration).empty())
        return Result<void>::failure({ErrorCategory::InvalidArgument, "EditorHost.game_configuration"});
    if (m_scene || m_project || m_enableProjectOpening)
    {
        m_editing = std::make_unique<EditingState>();
        // 未選択時は UI の状態だけを持ち、Document / Scene / Preview World を生成しない
        if (m_scene || m_project)
        {
            EditorApplicationConfig config;
            config.generateId = create_editor_object_id;
            if (m_project)
            {
                config.contentRoot = m_project->content_directory();
                if (const auto *file = m_project->startup_scene_file())
                    config.sceneFile = *file;
            }
            auto created = EditorApplication::create(std::move(m_scene), std::move(config));
            if (!created.has_value())
                return Result<void>::failure(*created.try_error());
            m_editing->application = created.take_value();
        }
        auto ui = EditorUi::create();
        if (!ui.has_value())
            return Result<void>::failure(*ui.try_error());
        m_editing->ui = ui.take_value();
        // Native Target を初期選択にする。未対応環境を Windows として補完しない
        auto target = current_native_game_build_target();
        if (target.has_value())
            m_editing->buildTarget = target.take_value();
        else
            m_editing->buildStatusText = target.try_error()->operation;
        if (!m_gameBuildService)
            m_editing->buildStatusText = m_gameBuildUnavailableReason;
        m_scene.reset();
    }
    auto windows = m_windows.initialize();
    if (!windows.has_value() && m_editing)
    {
        auto stopped = m_editing->application ? m_editing->application->shutdown() : Result<void>::success();
        if (!stopped.has_value())
            report_log_error("CueEditorHost scene cleanup", *stopped.try_error(), LogLevel::Error);
        m_editing.reset();
    }
    if (windows.has_value() && m_editing)
    {
        if (m_initialGameConfiguration)
            m_editing->gameConfiguration = *m_initialGameConfiguration;
        else if (m_project)
        {
            auto saved = load_editor_game_configuration(*m_windows.file_system(), *m_project);
            if (saved.has_value())
                m_editing->gameConfiguration = *saved.try_value();
            else
                m_editing->error = saved.try_error()->operation;
        }
    }
    if (windows.has_value() && m_editing && m_project && m_gameBuildService && m_ideService)
    {
        auto controller = create_script_controller(*m_project);
        if (!controller.has_value())
            return Result<void>::failure(*controller.try_error());
        m_editing->scripts = controller.take_value();
    }
    if (windows.has_value() && m_project)
    {
        auto observed = observe_git_head();
        if (!observed.has_value())
            m_editing->error = observed.try_error()->operation;
    }
    if (windows.has_value() && m_editing && !m_editing->scripts)
        m_editing->scriptStatusText = m_ideUnavailableReason;
    if (windows.has_value() && m_editing && has_play_configuration())
    {
        // 下位 Host の FileSystem が利用可能になってから検証する。未 Build でも Editor は継続する
        auto prepared = prepare_play();
        if (prepared.has_value())
            prepared = m_editing->play->describe();
        if (!prepared.has_value())
            m_editing->error = prepared.try_error()->operation;
    }
    return windows;
}

const Project *EditorHost::project() const noexcept
{
    return std::this_thread::get_id() == m_ownerId ? m_project.get() : nullptr;
}

bool EditorHost::has_active_project_operations() const noexcept
{
    if (m_gameBuildService)
    {
        const auto &status = m_gameBuildService->status();
        if (status.state == GameBuildState::Configuring || status.state == GameBuildState::Building ||
            status.isApplicationRunning)
            return true;
    }
    if (m_ideService && m_ideService->status().state == IdeState::Running)
        return true;
    if (m_editing && m_editing->scripts && m_editing->scripts->status().isBusy)
        return true;
    if (m_editing && m_editing->play)
    {
        const auto state = m_editing->play->state();
        return state == PlayState::Describing || state == PlayState::Starting || state == PlayState::Running ||
               state == PlayState::Stopping || state == PlayState::WaitingForDebugger;
    }
    return false;
}

bool EditorHost::can_open_project() const noexcept
{
    return std::this_thread::get_id() == m_ownerId && !m_isClosed && !m_isInitializing && !m_isPreparingProject &&
           m_hasInitializeAttempt && m_editing && m_windows.file_system() && !m_pendingProject &&
           !m_editing->pendingChange && !m_editing->isGitReloadPending && !has_active_project_operations();
}

bool EditorHost::has_pending_project_confirmation() const noexcept
{
    return std::this_thread::get_id() == m_ownerId && m_editing && m_editing->pendingChange &&
           m_editing->pendingChange->kind == DocumentChangeKind::OpenProject;
}

Result<void> EditorHost::request_open_project(const Path &a_projectFile)
{
    if (std::this_thread::get_id() != m_ownerId)
        return Result<void>::failure({ErrorCategory::WrongThread, "EditorHost.project.owner"});
    if (!can_open_project())
        return Result<void>::failure({ErrorCategory::InvalidState, "EditorHost.project.busy"});
    return prepare_project_change(a_projectFile, false);
}

Result<void> EditorHost::prepare_project_change(const Path &a_projectFile, bool a_isGitReload)
{
    ScopedFlag preparing(m_isPreparingProject);
    try
    {
        auto &files = *m_windows.file_system();
        auto absolute = files.absolute_path(a_projectFile);
        if (!absolute.has_value())
            return Result<void>::failure(*absolute.try_error());
        auto gitHead = read_editor_git_head(files, absolute.try_value()->parent());
        if ((!gitHead.has_value() && gitHead.try_error()->operation == "EditorGit.index_locked") ||
            (a_isGitReload && (!gitHead.has_value() || *gitHead.try_value() != m_editing->gitHead)))
            return Result<void>::failure({ErrorCategory::InvalidState, "EditorGit.checkout_in_progress"});
        // 候補の設定と Scene を検証する間は旧 Project / World / Undo を変更しない
        auto project = Project::load(files, *absolute.try_value());
        if (!project.has_value())
            return Result<void>::failure(*project.try_error());
        if (project.try_value()->descriptor().engineVersion &&
            *project.try_value()->descriptor().engineVersion != CUE_ENGINE_VERSION)
            return Result<void>::failure({ErrorCategory::InvalidArgument, "EditorHost.project_engine_version"});
        std::optional<Scene> scene;
        EditorApplicationConfig config;
        config.generateId = create_editor_object_id;
        config.contentRoot = project.try_value()->content_directory();
        if (const auto *file = project.try_value()->startup_scene_file())
        {
            auto loaded = Scene::load(files, *file);
            if (!loaded.has_value())
                return Result<void>::failure(*loaded.try_error());
            scene = loaded.take_value();
            config.sceneFile = *file;
        }
        auto application = EditorApplication::create(std::move(scene), std::move(config));
        if (!application.has_value())
            return Result<void>::failure(*application.try_error());
        if (a_isGitReload && m_editing->sceneBeforeCheckout)
        {
            // Scene の削除や Content Root 変更では、先に検証した起動 Scene を使う
            auto opened = (*application.try_value())->open_scene(files, *m_editing->sceneBeforeCheckout);
            if (!opened.has_value() && opened.try_error()->operation != "Editor.scene_file.missing" &&
                opened.try_error()->operation != "Editor.scene_file.outside_content")
            {
                return opened;
            }
        }
        auto prepared = std::make_unique<PreparedProject>();
        prepared->isGitReload = a_isGitReload;
        if (gitHead.has_value())
            prepared->gitHead = gitHead.take_value();
        prepared->project = std::make_unique<Project>(project.take_value());
        prepared->application = application.take_value();
        auto savedConfiguration = load_editor_game_configuration(files, *prepared->project);
        if (savedConfiguration.has_value())
            prepared->gameConfiguration = *savedConfiguration.try_value();
        else
            prepared->settingsError = savedConfiguration.try_error()->operation;
        auto controller = create_script_controller(*prepared->project, false);
        if (!controller.has_value())
            return Result<void>::failure(*controller.try_error());
        prepared->scripts = controller.take_value();
        if (prepared->gitHead)
        {
            auto latest = read_editor_git_head(files, prepared->project->root());
            if (!latest.has_value() || *latest.try_value() != prepared->gitHead)
                return Result<void>::failure({ErrorCategory::InvalidState, "EditorGit.checkout_in_progress"});
        }
        if (a_isGitReload)
        {
            m_pendingProject = std::move(prepared);
            m_isProjectChangeConfirmed = true;
            return Result<void>::success();
        }
        // 未保存確認は既存の ImGui Modal を使い、応答まで候補を所有する
        m_pendingProject = std::move(prepared);
        auto confirmed = begin_document_change(DocumentChangeKind::OpenProject);
        if (!confirmed.has_value())
        {
            m_pendingProject.reset();
            return Result<void>::failure(*confirmed.try_error());
        }
        m_isProjectChangeConfirmed = *confirmed.try_value();
        return Result<void>::success();
    }
    catch (const std::bad_alloc &)
    {
        m_pendingProject.reset();
        return Result<void>::failure({ErrorCategory::PlatformFailure, "EditorHost.project.allocation"});
    }
}

Result<void> EditorHost::confirm_open_project(ProjectOpenSaveChoice a_choice)
{
    if (std::this_thread::get_id() != m_ownerId)
        return Result<void>::failure({ErrorCategory::WrongThread, "EditorHost.project.owner"});
    if (!has_pending_project_confirmation() || !m_pendingProject || m_isClosed || m_isPreparingProject)
        return Result<void>::failure({ErrorCategory::InvalidState, "EditorHost.project.confirmation"});
    m_editing->pendingChange.reset();
    m_editing->ui->reset();
    if (a_choice == ProjectOpenSaveChoice::Cancel)
    {
        m_pendingProject.reset();
        return Result<void>::success();
    }
    if (a_choice == ProjectOpenSaveChoice::Save)
    {
        // 同じ Project / Scene の再選択でも、保存前に準備した候補へ巻き戻さない
        // 保存 Dialog の再入を拒否し、成功後に保存済み File から候補を作り直す
        auto previousCandidate = std::move(m_pendingProject);
        {
            ScopedFlag confirming(m_isPreparingProject);
            auto saved = save_editor_scene(false);
            if (!saved.has_value() || !*saved.try_value())
                return saved.has_value() ? Result<void>::success() : Result<void>::failure(*saved.try_error());
        }
        return request_open_project(previousCandidate->project->file());
    }
    m_isProjectChangeConfirmed = true;
    return Result<void>::success();
}

Result<std::unique_ptr<EditorScriptController>> EditorHost::create_script_controller(const Project &a_project,
                                                                                     bool a_useOverrides)
{
    using controllerResult = Result<std::unique_ptr<EditorScriptController>>;
    if (!m_gameBuildService || !m_ideService)
        return controllerResult::success(nullptr);
    try
    {
        EditorScriptCallbacks callbacks;
        callbacks.configuration = [this]() { return game_configuration(); };
        callbacks.play = [this]() { return m_editing ? m_editing->play.get() : nullptr; };
        callbacks.reload = [this]() -> Result<void>
        {
            record_git_script_build();
            auto prepared = prepare_play();
            return prepared.has_value() ? m_editing->play->describe() : std::move(prepared);
        };
        callbacks.startDebug = [this](const Scene &a_scene) -> Result<void>
        {
            record_git_script_build();
            auto prepared = prepare_play();
            if (!prepared.has_value())
                return prepared;
            auto started = m_editing->play->start_debug(a_scene);
            if (started.has_value())
                m_editing->playScene = a_scene.descriptor();
            return started;
        };
        auto directory = m_windows.file_system()->executable_directory();
        if (directory.has_value())
        {
            auto executable = directory.try_value()->join("CuePlayHost.exe");
            if (executable.has_value())
                callbacks.playExecutable = executable.take_value();
        }
        if (a_useOverrides && m_playExecutable)
            callbacks.playExecutable = *m_playExecutable;
        return controllerResult::success(std::make_unique<EditorScriptController>(
            a_project, *m_windows.file_system(), *m_gameBuildService, *m_ideService, std::move(callbacks)));
    }
    catch (const std::bad_alloc &)
    {
        return controllerResult::failure({ErrorCategory::PlatformFailure, "EditorHost.project.controller_allocation"});
    }
}

Result<void> EditorHost::apply_project_change()
{
    m_isProjectChangeConfirmed = false;
    // 操作待ちの間に外部の要求が開始した場合も、旧 Project を保全して切替を中止する
    if (!m_pendingProject || has_active_project_operations())
    {
        m_pendingProject.reset();
        return Result<void>::failure({ErrorCategory::InvalidState, "EditorHost.project.busy"});
    }
    if (m_pendingProject->gitHead)
    {
        // 確認待ち中に別ブランチへ移った候補を公開しない
        auto head = read_editor_git_head(*m_windows.file_system(), m_pendingProject->project->root());
        if (!head.has_value() || *head.try_value() != m_pendingProject->gitHead)
        {
            if (m_pendingProject->isGitReload)
                m_editing->isGitReloadPending = true;
            m_pendingProject.reset();
            return Result<void>::failure({ErrorCategory::InvalidState, "EditorGit.checkout_in_progress"});
        }
    }
    if (m_gameBuildService)
    {
        auto reset = m_gameBuildService->reset_project();
        if (!reset.has_value())
        {
            m_pendingProject.reset();
            return reset;
        }
    }
    // GPU は独立 Snapshot を使用する。Project / World を参照する Controller だけを先に回収する
    auto &editing = *m_editing;
    editing.scripts.reset();
    editing.play.reset();
    editing.playScene.reset();
    editing.playObjectIndices.clear();
    editing.appliedViewTick.reset();
    editing.sceneViewCamera.reset();
    m_playExecutable.reset();
    m_scriptModule.reset();
    const bool isGitReload = m_pendingProject->isGitReload;
    editing.gitHead = std::move(m_pendingProject->gitHead);
    editing.sceneBeforeCheckout.reset();
    m_project = std::move(m_pendingProject->project);
    editing.application = std::move(m_pendingProject->application);
    editing.scripts = std::move(m_pendingProject->scripts);
    editing.gameConfiguration = m_pendingProject->gameConfiguration;
    editing.isGameBuildRequired = true;
    auto settingsError = std::move(m_pendingProject->settingsError);
    m_pendingProject.reset();
    editing.ui->reset();
    editing.actions.clear();
    editing.error = std::move(settingsError);
    editing.notice.clear();
    if (isGitReload)
        editing.notice =
            editor_text(editing.language, "Project reloaded after Git checkout. Unsaved edits were discarded.",
                        "Git の切り替え後にプロジェクトを再読み込みしました。未保存の編集は破棄されました。");
    editing.buildStatusText.clear();
    editing.scriptStatusText.clear();
    auto target = current_native_game_build_target();
    if (target.has_value())
        editing.buildTarget = target.take_value();
    if (!m_gameBuildService)
        editing.buildStatusText = m_gameBuildUnavailableReason;
    if (!editing.scripts)
        editing.scriptStatusText = m_ideUnavailableReason;
    auto title = set_editor_project_title(*editing.window, m_project->descriptor().name);
    if (!title.has_value())
        editing.error = title.try_error()->operation;
    // 起動時と同じ検証経路で新 Project の型情報を取得する。未 Build でも編集は継続する
    if (has_play_configuration() && !needs_git_script_build())
    {
        auto prepared = prepare_play();
        if (prepared.has_value())
            prepared = editing.play->describe();
        if (!prepared.has_value())
            editing.error = prepared.try_error()->operation;
    }
    return Result<void>::success();
}

Result<void> EditorHost::observe_git_head()
{
    if (!m_project || !m_editing || !m_windows.file_system())
        return Result<void>::success();
    auto head = read_editor_git_head(*m_windows.file_system(), m_project->root());
    if (!head.has_value())
        return Result<void>::failure(*head.try_error());
    auto &editing = *m_editing;
    if (*head.try_value() && editing.gitHead && *head.try_value() != editing.gitHead)
    {
        editing.isGitReloadPending = true;
        editing.staleBranchBuilds[m_project->file().utf8()].fill(true);
    }
    // Git 管理の開始時は現在のブランチを初回の比較基準にする
    if (*head.try_value())
        editing.gitHead = head.take_value();
    return Result<void>::success();
}

void EditorHost::poll_git_reload()
{
    if (!m_editing || !m_project || m_isClosed)
        return;
    auto &editing = *m_editing;
    const auto now = std::chrono::steady_clock::now();
    if (now < editing.nextGitCheck)
        return;
    editing.nextGitCheck = now + std::chrono::milliseconds(250);
    auto observed = observe_git_head();
    if (!observed.has_value() || !editing.isGitReloadPending)
        return;
    // 切替先の読込に失敗しても、切替元の編集を保存可能な状態へ戻さない
    if (editing.application)
    {
        if (const auto *file = editing.application->document().file())
            editing.sceneBeforeCheckout = *file;
        editing.application.reset();
        editing.pendingChange.reset();
        m_pendingProject.reset();
        m_isProjectChangeConfirmed = false;
        editing.actions.clear();
        editing.ui->reset();
        editing.error.clear();
        editing.notice = editor_text(editing.language, "Git checkout detected. Reloading project...",
                                     "Git の切り替えを検出しました。プロジェクトを再読み込み中...");
        auto title = set_editor_project_title(*editing.window, {});
        if (!title.has_value())
            editing.error = title.try_error()->operation;
        if (editing.scripts && editing.scripts->status().isBusy)
        {
            auto cancelled = editing.scripts->cancel();
            if (!cancelled.has_value())
                editing.error = cancelled.try_error()->operation;
        }
        auto stopped = stop_play();
        if (!stopped.has_value())
            editing.error = stopped.try_error()->operation;
        if (m_gameBuildService &&
            (m_gameBuildService->status().state == GameBuildState::Configuring ||
             m_gameBuildService->status().state == GameBuildState::Building))
        {
            auto cancelled = m_gameBuildService->cancel();
            if (!cancelled.has_value())
                editing.error = cancelled.try_error()->operation;
        }
    }
    if (has_active_project_operations())
    {
        if (m_gameBuildService && m_gameBuildService->status().isApplicationRunning)
            editing.notice = editor_text(editing.language, "Close the running game to finish reloading the project.",
                                         "実行中のゲームを閉じると、プロジェクトの再読み込みを再開します。");
        return;
    }
    auto prepared = prepare_project_change(m_project->file(), true);
    if (!prepared.has_value() && prepared.try_error()->operation == "EditorGit.checkout_in_progress")
        return;
    editing.isGitReloadPending = false;
    if (!prepared.has_value())
    {
        editing.notice.clear();
        const auto &operation = prepared.try_error()->operation;
        if (operation == "EditorHost.project_engine_version")
            editing.error = "Engine version changed. Open this project from CueHub.";
        else
            editing.error = "Could not reload project after Git checkout. " + operation;
    }
}

const Scene *EditorHost::scene() const noexcept
{
    if (std::this_thread::get_id() != m_ownerId)
        return nullptr;
    if ((m_enableProjectOpening && !m_project) || (m_editing && !m_editing->application))
        return nullptr;
    return m_editing && m_editing->application ? &m_editing->application->document().scene()
                                               : (m_scene ? &*m_scene : nullptr);
}

World *EditorHost::preview_world() noexcept
{
    return std::this_thread::get_id() == m_ownerId && m_editing && m_editing->application
               ? m_editing->application->preview_world()
               : nullptr;
}

std::optional<SceneInstanceHandle> EditorHost::preview_instance() const noexcept
{
    return std::this_thread::get_id() == m_ownerId && m_editing && m_editing->application
               ? m_editing->application->preview_instance()
               : std::optional<SceneInstanceHandle>{};
}

/// @brief 構築 Thread 上で Message と Frame の進行を委譲する
Result<bool> EditorHost::step()
{
    if (std::this_thread::get_id() != m_ownerId)
    {
        return Result<bool>::failure({ErrorCategory::WrongThread, "EditorHost.step"});
    }
    if (m_isStepping || m_isInitializing)
    {
        return Result<bool>::failure({ErrorCategory::InvalidState, "EditorHost.step"});
    }
    ScopedFlag stepping(m_isStepping);
    poll_git_reload();
    // Render が止まる最小化中も Pipe を排出し、外部 Build を描画 Frame に依存させない
    poll_game_build();
    poll_play();
    poll_script_operations();
    // 前 Frame の UI / Action 参照が失効した後で新しい編集正本を公開する
    // Render は所有 Snapshot だけを使うため、Window / Renderer / Frame ID を再生成しない
    if (m_isProjectChangeConfirmed)
    {
        auto changed = apply_project_change();
        if (!changed.has_value())
        {
            if (changed.try_error()->category == ErrorCategory::Fatal)
                return Result<bool>::failure(*changed.try_error());
            m_editing->error = changed.try_error()->operation;
        }
    }
    return m_windows.step();
}

/// @brief UI の Context を公開せず、描画 Data と Capture の概要を返す
Result<ImGuiFrameInfo> EditorHost::ui_frame_info() const
{
    if (std::this_thread::get_id() != m_ownerId)
    {
        return Result<ImGuiFrameInfo>::failure({ErrorCategory::WrongThread, "EditorHost.ui_frame_info"});
    }
    return m_imgui ? m_imgui->frame_info()
                   : Result<ImGuiFrameInfo>::failure({ErrorCategory::InvalidState, "EditorHost.ui_frame_info"});
}

/// @brief Windows 実行基盤が持つ CPU Frame の Snapshot を返す
Result<FrameProgress> EditorHost::frame_progress() const
{
    return m_windows.frame_progress();
}

/// @brief 下位 Runtime が保持する同期済み CPU 集計を取得する
Result<FrameTimingInfo> EditorHost::frame_timing_info() const
{
    return m_windows.frame_timing_info();
}

/// @brief Editor の所有 Context 内で集計した CPU 時間を公開する
Result<ImGuiTimingInfo> EditorHost::ui_timing_info() const
{
    if (std::this_thread::get_id() != m_ownerId)
    {
        return Result<ImGuiTimingInfo>::failure({ErrorCategory::WrongThread, "EditorHost.ui_timing_info"});
    }
    return m_imgui ? m_imgui->timing_info()
                   : Result<ImGuiTimingInfo>::failure({ErrorCategory::InvalidState, "EditorHost.ui_timing_info"});
}

/// @brief UI Callback 中に Render の Graph Lock を待たず、直前の Main Snapshot を複製する
Result<MainFrameGraphPerformance> EditorHost::graph_performance() const
{
    if (std::this_thread::get_id() != m_ownerId)
    {
        return Result<MainFrameGraphPerformance>::failure({ErrorCategory::WrongThread, "EditorHost.graph_performance"});
    }
    auto running = m_windows.frame_progress();
    if (!running.has_value())
    {
        return Result<MainFrameGraphPerformance>::failure(*running.try_error());
    }
    try
    {
        return Result<MainFrameGraphPerformance>::success(m_graphPerformance);
    }
    catch (const std::bad_alloc &)
    {
        return Result<MainFrameGraphPerformance>::failure(
            {ErrorCategory::PlatformFailure, "EditorHost.graph_performance.allocation"});
    }
}

/// @brief Owner Thread から転送枠の滞留と回収数を確認する
Result<ImGuiTransferInfo> EditorHost::ui_transfer_info() const
{
    if (std::this_thread::get_id() != m_ownerId)
    {
        return Result<ImGuiTransferInfo>::failure({ErrorCategory::WrongThread, "EditorHost.ui_transfer_info"});
    }
    return m_imgui ? m_imgui->transfer_info()
                   : Result<ImGuiTransferInfo>::failure({ErrorCategory::InvalidState, "EditorHost.ui_transfer_info"});
}

/// @brief 所有する Windows 実行基盤の停止と GPU 完了待ちを委譲する
Result<void> EditorHost::shutdown()
{
    if (std::this_thread::get_id() != m_ownerId)
    {
        return Result<void>::failure({ErrorCategory::WrongThread, "EditorHost.shutdown"});
    }
    if (m_isStepping || m_isInitializing)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "EditorHost.shutdown"});
    }
    m_isClosed = true;
    m_pendingProject.reset();
    m_isProjectChangeConfirmed = false;
    if (m_editing && m_editing->scripts)
    {
        auto cancelled = m_editing->scripts->cancel();
        if (!cancelled.has_value())
            report_log_error("Editor script cleanup", *cancelled.try_error(), LogLevel::Error);
    }
    auto ide = m_ideService ? m_ideService->shutdown() : Result<void>::success();
    // Build の子孫だけを停止する。独立起動した製品は継続し、監視 Handle を解放する
    auto build = m_gameBuildService ? m_gameBuildService->shutdown() : Result<void>::success();
    auto windows = m_windows.shutdown();
    if (!windows.has_value())
        return windows;
    // Worker と Pass が停止した後で、Main 所有の Preview World を回収する
    if (m_editing)
    {
        auto stopped = m_editing->application ? m_editing->application->shutdown() : Result<void>::success();
        m_editing.reset();
        return !stopped.has_value() ? std::move(stopped) : !build.has_value() ? std::move(build) : std::move(ide);
    }
    return !build.has_value() ? std::move(build) : std::move(ide);
}

/// @brief 下位 Host が追跡した入力到達と UI Frame の対応を所有値で返す
Result<InputLatencyInfo> EditorHost::input_latency_info() const
{
    return m_windows.input_latency_info();
}

/// @brief Window の生成 Thread に UI の Owner を固定する
Result<void> EditorHost::initialize_ui(Window &a_window)
{
    // 下位 Host が所有する FileSystem と保存 Root を借用する
    auto *files = m_windows.file_system();
    const auto *paths = m_windows.storage_paths();
    if (!files || !paths)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "EditorHost.file_system"});
    }
    m_imguiConfig.fileSystem = files;
    m_imguiConfig.clock = m_windows.clock();
    // ImGui の相対設定 Path を Host の保存先へ解決する
    if (!m_imguiConfig.settingsFile.empty())
    {
        auto path = paths->dataRoot.join(m_imguiConfig.settingsFile);
        if (!path.has_value())
        {
            return Result<void>::failure(*path.try_error());
        }
        m_imguiConfig.settingsFile = path.try_value()->utf8();
    }
    // 共通の時計と Profiler を使って ImGui の所有者を作る
    auto result = ImGuiManager::create(a_window, std::move(m_imguiConfig), m_windows.profiler());
    if (!result.has_value())
    {
        return Result<void>::failure(*result.try_error());
    }
    m_imgui = result.take_value();
    if (m_editing)
    {
        m_editing->window = &a_window;
        // Project を開く前から、前回の Editor 言語で表示する
        auto language = load_editor_language(*files, paths->dataRoot);
        if (language.has_value())
            m_editing->language = language.take_value();
        else
            m_editing->error = language.try_error()->operation;
    }
    return Result<void>::success();
}

/// @brief Message Pump 後に採用された Frame だけ UI を構築して CPU 描画 Data を確定する
Result<void> EditorHost::build_ui(std::uint64_t a_frame, std::stop_token a_stopToken)
{
    TimingTraceScope traceScope("Editor.Build");
    if (a_stopToken.stop_requested())
    {
        return Result<void>::success();
    }
    // Render は Graph Lock から ImGui Context を借りるため、逆順で同時に保持しない
    // 計測 Snapshot を UI 開始前に取得し、Callback 内では所有値だけを表示する
    TimingTraceScope traceStatistics("Editor.Statistics");
    // Lock 取得順を変えず、UI に必要な統計を先に複写する
    auto graphPerformance = m_windows.graph_performance();
    auto frameTiming = m_windows.frame_timing_info();
    auto uiTiming = m_imgui->timing_info();
    auto inputLatency = m_windows.input_latency_info();
    if (!inputLatency.has_value())
    {
        return Result<void>::failure(*inputLatency.try_error());
    }
    m_inputLatency = inputLatency.take_value();
    if (!graphPerformance.has_value() || !frameTiming.has_value() || !uiTiming.has_value())
    {
        return Result<void>::failure(!graphPerformance.has_value() ? *graphPerformance.try_error()
                                     : !frameTiming.has_value()    ? *frameTiming.try_error()
                                                                   : *uiTiming.try_error());
    }
    m_graphPerformance = graphPerformance.take_value();
    m_frameTiming = frameTiming.take_value();
    m_uiTiming = uiTiming.take_value();
    traceStatistics.finish();
    // 子 Process の失敗は編集用 World に伝播させず、ImGui の Lock 外で回収する
    auto built = m_imgui->build_frame(
        [this]() -> Result<void>
        {
            // 旧診断 UI の操作も Modal 中に変更を要求しないよう無効化する
            ImGui::BeginDisabled(m_editing && m_editing->pendingChange.has_value());
            auto result = m_buildUi();
            ImGui::EndDisabled();
            if (!result.has_value() || !m_editing)
                return result;
            // UI は不変 Document を借り、操作要求だけを所有値として返す
            const auto display = ImGui::GetIO().DisplaySize;
            EditorUiContext context;
            context.language = m_editing->language;
            context.sceneTextureId = k_sceneViewTextureId;
            context.displayWidth = display.x;
            context.displayHeight = display.y;
            context.errorText = m_editing->error;
            context.noticeText = m_editing->notice;
            context.isSceneViewEnabled = m_hasSceneView;
            context.isProjectOpen = m_editing->application && (!m_enableProjectOpening || m_project);
            context.canOpenProject = can_open_project();
            context.isSceneViewEnabled = context.isSceneViewEnabled && context.isProjectOpen;
            context.isUnsavedConfirmationRequested = m_editing->pendingChange.has_value();
            context.isBuildConfirmationRequested = has_pending_game_build_confirmation();
            context.script.statusText = m_editing->scriptStatusText;
            if (!m_editing->scripts && context.script.statusText.empty())
                context.script.statusText = context.text("IDE service is not configured", "IDE 連携は設定されていません");
            context.script.configuration = m_editing->gameConfiguration;
            context.script.canSelectConfiguration = can_select_game_configuration();
            context.script.canCreate = can_select_game_configuration();
            context.script.isBuildRequired = has_play_configuration() && m_editing->isGameBuildRequired;
            context.build.platforms = supported_game_build_platforms();
            context.build.selectedTarget = m_editing->buildTarget;
            context.build.statusText = m_editing->buildStatusText;
            if (!m_gameBuildService && context.build.statusText.empty())
                context.build.statusText =
                    context.text("Build service is not configured", "ビルド機能は設定されていません");
            if (m_project && m_project->startup_scene_file())
                context.build.startupScene = m_project->startup_scene_file()->utf8();
            context.canPlay = has_play_configuration() && !needs_git_script_build();
            if (m_editing->play)
            {
                const auto state = m_editing->play->state();
                if (state != PlayState::Failed && state != PlayState::Describing)
                    context.scriptTypes = m_editing->play->types();
                context.isPlaying = state == PlayState::Starting || state == PlayState::Running ||
                                    state == PlayState::Stopping || state == PlayState::WaitingForDebugger;
                context.isPlayBusy = state == PlayState::Describing || state == PlayState::Starting ||
                                     state == PlayState::Stopping || state == PlayState::WaitingForDebugger;
                context.canStop = state == PlayState::Starting || state == PlayState::Running ||
                                  state == PlayState::WaitingForDebugger;
                context.script.isWaitingForDebugger = state == PlayState::WaitingForDebugger;
                context.script.isDebuggerAttached = m_editing->play->is_debugger_attached();
                context.script.processId = m_editing->play->process_id();
                context.playSessionId = m_editing->play->session_id();
                context.playScene = m_editing->playScene ? &*m_editing->playScene : nullptr;
            }
            context.sceneViewCamera = m_editing->sceneViewCamera;
            const auto *cameraScene = context.playScene;
            if (!cameraScene && m_editing->application)
                cameraScene = &m_editing->application->document().scene().descriptor();
            if (cameraScene)
                for (const auto &object : cameraScene->objects)
                    if (object.isActive && object.camera && object.transform)
                    {
                        context.sceneCameraTransform = *object.transform;
                        break;
                    }
            if (m_gameBuildService)
            {
                const auto &status = m_gameBuildService->status();
                const bool isBuilding =
                    status.state == GameBuildState::Configuring || status.state == GameBuildState::Building;
                context.build.isBuilding = isBuilding;
                context.build.canCancel = isBuilding;
                context.build.isApplicationRunning = status.isApplicationRunning;
                context.build.canBuild = m_project && m_project->descriptor().gameTarget &&
                                         m_project->startup_scene_file() && !isBuilding &&
                                         !status.isApplicationRunning && !context.isPlaying && !context.isPlayBusy;
                context.build.canRun = status.state == GameBuildState::Succeeded &&
                                       status.purpose == GameBuildPurpose::App &&
                                       status.target == m_editing->buildTarget && !status.isApplicationRunning &&
                                       !context.isPlaying && !context.isPlayBusy;
                context.build.logText = status.log;
                context.build.isLogTruncated = status.isLogTruncated;
                context.build.outputDirectory = status.outputDirectory.utf8();
                context.build.exitCode = status.exitCode;
                // Build の完了前に新しい Play / Metadata 子を起動しない
                context.canPlay = context.canPlay && !isBuilding;
            }
            if (m_editing->scripts)
            {
                const bool isBusy = m_editing->scripts->status().isBusy;
                const bool isBuilding =
                    m_gameBuildService && (m_gameBuildService->status().state == GameBuildState::Configuring ||
                                           m_gameBuildService->status().state == GameBuildState::Building);
                context.script.isBusy = isBusy;
                context.script.canOpenIde = !isBusy && !isBuilding && !context.build.isApplicationRunning;
                context.script.canEditSource = context.script.canOpenIde;
                context.script.canBuild = context.script.canOpenIde && !context.isPlaying && !context.isPlayBusy;
                context.script.canDebugPlay = context.script.canBuild;
                context.script.canCancel = isBusy;
                context.build.canBuild = context.build.canBuild && !isBusy;
                context.build.canRun = context.build.canRun && !isBusy;
                context.canPlay = context.canPlay && !isBusy;
            }
            auto actions =
                m_editing->ui->draw(m_editing->application ? &m_editing->application->document() : nullptr, context);
            if (!actions.has_value())
                return Result<void>::failure(*actions.try_error());
            m_editing->actions = actions.take_value();
            return Result<void>::success();
        });
    if (!built.has_value())
        return built;
    // Panel の借用と ImGui Lock が解放されてから、編集正本と Preview を確定する
    auto applied = apply_editor_actions();
    if (!applied.has_value())
        return applied;
    if (m_sceneRenderer)
    {
        SceneRenderSnapshot frameScene;
        const auto state = play_state();
        const bool isPlayActive = state == PlayState::Starting || state == PlayState::Running ||
                                  state == PlayState::Stopping || state == PlayState::WaitingForDebugger;
        if (isPlayActive)
        {
            // ゲーム Code を親へ持ち込まず、検証済みの独立値を既存 SceneRenderer に渡す
            if (const auto *view = play_view_snapshot())
                frameScene = view->render;
        }
        else if (auto *world = preview_world())
        {
            // Project 選択後だけ World を同期する。未選択 Frame は Camera / Mesh のない所有値を渡す
            auto updated = m_editing->application->synchronize_preview();
            if (!updated.has_value())
                return updated;
            auto snapshot = extract_render_scene(*world);
            if (!snapshot.has_value())
                return Result<void>::failure(*snapshot.try_error());
            frameScene = snapshot.take_value();
        }
        if (m_editing && m_editing->sceneViewCamera)
        {
            auto view = math::view_from_transform(*m_editing->sceneViewCamera);
            if (!view.has_value())
                return Result<void>::failure(*view.try_error());
            if (!frameScene.camera)
                frameScene.camera.emplace();
            frameScene.camera->view = view.take_value();
        }
        if (isPlayActive && m_editing && m_editing->window)
        {
            const auto size = m_editing->window->client_size();
            auto valid = validate_editor_play_view(frameScene, float((std::max)(size.width, 1u)) /
                                                                   float((std::max)(size.height, 1u)));
            if (!valid.has_value())
            {
                // 有限な入力同士でも積で Overflow する。ゲーム値の異常を Editor の描画失敗へ伝播させない
                m_editing->error = valid.try_error()->operation;
                auto stopped = stop_play();
                if (!stopped.has_value())
                    m_editing->error += ": " + stopped.try_error()->operation;
                frameScene = {};
            }
        }
        auto published = m_sceneRenderer->publish_frame(a_frame, std::move(frameScene));
        if (!published.has_value())
            return published;
    }
    return m_imgui->publish_frame(a_frame, a_stopToken);
}

/// @brief 保存先選択と原子的保存を接続し、取消・公開失敗では終了を許可しない
Result<bool> EditorHost::save_editor_scene(bool a_chooseFile)
{
    if (!m_editing || !m_editing->application)
        return Result<bool>::failure({ErrorCategory::InvalidState, "EditorHost.save.no_project"});
    if (m_gameBuildService)
    {
        const auto state = m_gameBuildService->status().state;
        if (state == GameBuildState::Configuring || state == GameBuildState::Building)
            return Result<bool>::failure({ErrorCategory::InvalidState, "EditorHost.build.save_locked"});
    }
    auto &editing = *m_editing;
    auto &app = *editing.application;
    if (app.document().is_transform_editing())
    {
        auto committed = app.commit_transform_edit();
        if (!committed.has_value())
            return Result<bool>::failure(*committed.try_error());
        editing.ui->reset();
    }
    std::optional<Path> destination;
    if (!a_chooseFile && app.document().file())
        destination = *app.document().file();
    else
    {
        auto chosen = choose_scene_file(*editing.window, true, app.document().file(),
                                        m_project ? &m_project->content_directory() : nullptr, editing.language);
        if (!chosen.has_value())
            return Result<bool>::failure(*chosen.try_error());
        destination = chosen.take_value();
    }
    if (!destination)
        return Result<bool>::success(false);
    // 保存 Dialog の表示中にも外部で Checkout できるため、公開の直前に HEAD を確認する
    auto gitHead = observe_git_head();
    if (!gitHead.has_value() && (editing.gitHead || gitHead.try_error()->operation == "EditorGit.index_locked"))
        return Result<bool>::failure(*gitHead.try_error());
    if (editing.isGitReloadPending)
        return Result<bool>::failure({ErrorCategory::InvalidState, "EditorGit.reload_pending"});
    auto saved = app.save_scene(*m_windows.file_system(), *destination);
    if (!saved.has_value())
        return Result<bool>::failure(*saved.try_error());
    // 通常の保存成否は公開成功で判定し、電源断耐性の保証と混同しない
    editing.notice = editor_text(editing.language, "Scene saved", "シーンを保存しました");
    editing.ui->reset();
    return Result<bool>::success(true);
}

/// @brief Document を捨てる操作を保留し、Modal の応答を待つ間も通常の Frame を継続する
Result<bool> EditorHost::begin_document_change(DocumentChangeKind a_kind, std::optional<Path> a_file)
{
    if (!m_editing)
        return Result<bool>::success(true);
    // 二度目の Close や同じ Frame の要求で、最初に保留した操作を上書きしない
    if (m_editing->pendingChange)
        return Result<bool>::success(false);
    // 未選択画面は保存対象を持たず、最初の Project をそのまま開く
    if (!m_editing->application)
        return Result<bool>::success(true);
    auto &app = *m_editing->application;
    if (app.document().is_transform_editing())
    {
        auto committed = app.commit_transform_edit();
        if (!committed.has_value())
            return Result<bool>::failure(*committed.try_error());
        // Title Bar からの Close も UI の入力状態を確定し、取消後に古い Draft を残さない
        m_editing->ui->reset();
    }
    if (!app.document().is_dirty())
        return Result<bool>::success(true);
    m_editing->pendingChange = EditingState::PendingChange{a_kind, std::move(a_file)};
    return Result<bool>::success(false);
}

/// @brief 確認済みの Scene 切替または終了予約を、ImGui の借用がない地点で実行する
Result<void> EditorHost::execute_document_change(DocumentChangeKind a_kind, const Path *a_file)
{
    auto &editing = *m_editing;
    if (a_kind == DocumentChangeKind::OpenProject)
    {
        if (!m_pendingProject)
            return Result<void>::failure({ErrorCategory::InvalidState, "EditorHost.project.not_prepared"});
        m_isProjectChangeConfirmed = true;
        return Result<void>::success();
    }
    if (a_kind == DocumentChangeKind::BuildGame)
        return start_game_build();
    if (a_kind == DocumentChangeKind::Exit)
    {
        auto requested = request_editor_close(*editing.window);
        if (requested.has_value())
            editing.isCloseConfirmed = true;
        return requested;
    }
    if (a_kind == DocumentChangeKind::OpenScene && !a_file)
        return Result<void>::failure({ErrorCategory::InvalidState, "EditorHost.open_scene.file"});
    if (!editing.application)
        return Result<void>::failure({ErrorCategory::InvalidState, "EditorHost.scene.no_project"});
    auto changed = a_kind == DocumentChangeKind::OpenScene
                       ? editing.application->open_scene(*m_windows.file_system(), *a_file)
                       : editing.application->new_scene();
    if (changed.has_value())
    {
        editing.sceneViewCamera.reset();
        editing.ui->reset();
        // 保存してから新規作成した場合も、新しい Document の未保存表示を隠さない
        editing.notice.clear();
    }
    return changed;
}

const GameBuildStatus *EditorHost::game_build_status() const noexcept
{
    return std::this_thread::get_id() == m_ownerId && m_gameBuildService ? &m_gameBuildService->status() : nullptr;
}

const EditorScriptStatus *EditorHost::script_status() const noexcept
{
    return std::this_thread::get_id() == m_ownerId && m_editing && m_editing->scripts ? &m_editing->scripts->status()
                                                                                      : nullptr;
}

Result<void> EditorHost::open_ide()
{
    if (std::this_thread::get_id() != m_ownerId)
        return Result<void>::failure({ErrorCategory::WrongThread, "EditorHost.ide.owner"});
    if (m_isClosed || !m_editing || !m_editing->application || !m_editing->scripts || m_editing->isGitReloadPending ||
        m_editing->pendingChange || m_pendingProject)
        return Result<void>::failure({ErrorCategory::InvalidState, "EditorHost.ide.unavailable"});
    return m_editing->scripts->open_folder();
}

Result<void> EditorHost::edit_script(std::string_view a_typeId)
{
    if (std::this_thread::get_id() != m_ownerId)
        return Result<void>::failure({ErrorCategory::WrongThread, "EditorHost.ide.owner"});
    if (m_isClosed || !m_editing || !m_editing->application || !m_editing->scripts || m_editing->isGitReloadPending ||
        m_editing->pendingChange || m_pendingProject)
        return Result<void>::failure({ErrorCategory::InvalidState, "EditorHost.ide.unavailable"});
    return m_editing->scripts->open_script(a_typeId);
}

Result<void> EditorHost::build_scripts()
{
    if (std::this_thread::get_id() != m_ownerId)
        return Result<void>::failure({ErrorCategory::WrongThread, "EditorHost.ide.owner"});
    if (m_isClosed || !m_editing || !m_editing->application || !m_editing->scripts || m_editing->isGitReloadPending ||
        m_editing->pendingChange || m_pendingProject)
        return Result<void>::failure({ErrorCategory::InvalidState, "EditorHost.ide.unavailable"});
    return m_editing->scripts->build_scripts();
}

Result<void> EditorHost::debug_play()
{
    if (std::this_thread::get_id() != m_ownerId)
        return Result<void>::failure({ErrorCategory::WrongThread, "EditorHost.ide.owner"});
    if (m_isClosed || !m_editing || !m_editing->application || !m_editing->scripts || m_editing->isGitReloadPending ||
        m_editing->pendingChange || m_pendingProject)
        return Result<void>::failure({ErrorCategory::InvalidState, "EditorHost.ide.unavailable"});
    auto &app = *m_editing->application;
    if (app.document().is_transform_editing())
    {
        auto committed = app.commit_transform_edit();
        if (!committed.has_value())
            return committed;
        m_editing->ui->reset();
    }
    return m_editing->scripts->debug_play(app.document().scene());
}

Result<void> EditorHost::cancel_script_operation()
{
    if (std::this_thread::get_id() != m_ownerId)
        return Result<void>::failure({ErrorCategory::WrongThread, "EditorHost.ide.owner"});
    if (m_isClosed || !m_editing || !m_editing->scripts)
        return Result<void>::failure({ErrorCategory::InvalidState, "EditorHost.ide.unavailable"});
    return m_editing->scripts->cancel();
}

void EditorHost::poll_script_operations()
{
    if (!m_editing || !m_editing->scripts || m_isClosed)
        return;
    auto progressed = m_editing->scripts->poll();
    if (!progressed.has_value())
        m_editing->error = progressed.try_error()->operation;
    const auto &status = m_editing->scripts->status();
    m_editing->scriptStatusText = m_editing->scripts->status_message(m_editing->language);
    if (status.error)
        m_editing->scriptStatusText += ": " + status.error->operation;
}

bool EditorHost::has_pending_game_build_confirmation() const noexcept
{
    return std::this_thread::get_id() == m_ownerId && m_editing && m_editing->pendingChange &&
           m_editing->pendingChange->kind == DocumentChangeKind::BuildGame;
}

Result<void> EditorHost::request_game_build(GameBuildTarget a_target)
{
    if (std::this_thread::get_id() != m_ownerId)
        return Result<void>::failure({ErrorCategory::WrongThread, "EditorHost.build.owner"});
    if (!m_editing || !m_editing->application || m_editing->isGitReloadPending || m_isClosed ||
        !m_windows.file_system() || !m_project || !m_project->descriptor().gameTarget ||
        !m_project->startup_scene_file() || !m_gameBuildService)
        return Result<void>::failure({ErrorCategory::InvalidState, "EditorHost.build.unavailable"});
    if (m_editing->pendingChange || m_pendingProject)
        return Result<void>::failure({ErrorCategory::InvalidState, "EditorHost.build.confirmation_pending"});
    if (m_editing->scripts && m_editing->scripts->status().isBusy)
        return Result<void>::failure({ErrorCategory::InvalidState, "EditorHost.ide.busy"});
    auto validated = validate_game_build_target(a_target);
    if (!validated.has_value())
        return validated;
    const auto &status = m_gameBuildService->status();
    if (status.state == GameBuildState::Configuring || status.state == GameBuildState::Building ||
        status.isApplicationRunning)
        return Result<void>::failure({ErrorCategory::InvalidState, "EditorHost.build.busy"});
    if (m_editing->play)
    {
        const auto state = m_editing->play->state();
        if (state == PlayState::Describing || state == PlayState::Starting || state == PlayState::Running ||
            state == PlayState::Stopping || state == PlayState::WaitingForDebugger)
            return Result<void>::failure({ErrorCategory::InvalidState, "EditorHost.build.play_busy"});
    }
    // 確認中に Target を変更しない。再要求は上で拒否し、最初の要求を保全する
    m_editing->buildTarget = std::move(a_target);
    auto confirmed = begin_document_change(DocumentChangeKind::BuildGame);
    if (!confirmed.has_value())
        return Result<void>::failure(*confirmed.try_error());
    return *confirmed.try_value() ? start_game_build() : Result<void>::success();
}

Result<void> EditorHost::start_game_build()
{
    if (!m_editing || !m_gameBuildService || !m_project)
        return Result<void>::failure({ErrorCategory::InvalidState, "EditorHost.build.unavailable"});
    // 保存済みの Project startupScene を配布する。編集中 Scene を勝手に起動 Scene へ置き換えない
    auto started = m_gameBuildService->start(*m_project, m_editing->buildTarget);
    if (started.has_value())
    {
        m_editing->error.clear();
        m_editing->notice = editor_text(m_editing->language, "Build started from saved project files",
                                        "保存済みファイルでビルドを開始しました");
    }
    return started;
}

Result<void> EditorHost::confirm_game_build(GameBuildSaveChoice a_choice)
{
    if (std::this_thread::get_id() != m_ownerId)
        return Result<void>::failure({ErrorCategory::WrongThread, "EditorHost.build.owner"});
    if (!has_pending_game_build_confirmation() || m_isClosed)
        return Result<void>::failure({ErrorCategory::InvalidState, "EditorHost.build.no_confirmation"});
    if (a_choice != GameBuildSaveChoice::Save && a_choice != GameBuildSaveChoice::SavedFiles &&
        a_choice != GameBuildSaveChoice::Cancel)
        return Result<void>::failure({ErrorCategory::InvalidArgument, "EditorHost.build.save_choice"});
    // 取消や保存失敗でも応答は消費する。後の Frame で古い Build を再開しない
    m_editing->pendingChange.reset();
    m_editing->ui->reset();
    if (a_choice == GameBuildSaveChoice::Cancel)
        return Result<void>::success();
    if (a_choice == GameBuildSaveChoice::Save)
    {
        auto saved = save_editor_scene(false);
        if (!saved.has_value())
            return Result<void>::failure(*saved.try_error());
        if (!*saved.try_value())
            return Result<void>::success();
    }
    return start_game_build();
}

Result<void> EditorHost::cancel_game_build()
{
    if (std::this_thread::get_id() != m_ownerId)
        return Result<void>::failure({ErrorCategory::WrongThread, "EditorHost.build.owner"});
    if (!m_editing || m_isClosed || !m_gameBuildService)
        return Result<void>::failure({ErrorCategory::InvalidState, "EditorHost.build.unavailable"});
    if (has_pending_game_build_confirmation())
        return confirm_game_build(GameBuildSaveChoice::Cancel);
    if (m_editing->scripts && m_editing->scripts->status().isBusy)
        return cancel_script_operation();
    return m_gameBuildService->cancel();
}

Result<void> EditorHost::run_game()
{
    if (std::this_thread::get_id() != m_ownerId)
        return Result<void>::failure({ErrorCategory::WrongThread, "EditorHost.build.owner"});
    if (!m_editing || !m_editing->application || !m_project || m_isClosed || !m_gameBuildService ||
        m_editing->pendingChange || m_pendingProject || m_editing->isGitReloadPending)
        return Result<void>::failure({ErrorCategory::InvalidState, "EditorHost.build.unavailable"});
    const auto &status = m_gameBuildService->status();
    if (status.state != GameBuildState::Succeeded || status.purpose != GameBuildPurpose::App ||
        status.target != m_editing->buildTarget || status.isApplicationRunning ||
        (m_editing->scripts && m_editing->scripts->status().isBusy))
        return Result<void>::failure({ErrorCategory::InvalidState, "EditorHost.build.selection_changed"});
    if (m_editing->play)
    {
        const auto state = m_editing->play->state();
        if (state == PlayState::Describing || state == PlayState::Starting || state == PlayState::Running ||
            state == PlayState::Stopping || state == PlayState::WaitingForDebugger)
            return Result<void>::failure({ErrorCategory::InvalidState, "EditorHost.build.play_busy"});
    }
    return m_gameBuildService->launch();
}

void EditorHost::poll_game_build()
{
    if (!m_gameBuildService || !m_editing || m_isClosed)
        return;
    auto polled = m_gameBuildService->poll();
    if (!polled.has_value())
        m_editing->error = polled.try_error()->operation;
    const auto &status = m_gameBuildService->status();
    const bool isConfigureOnly = status.purpose == GameBuildPurpose::IdeProject;
    const char *label = editor_text(m_editing->language, "Ready", "準備完了");
    switch (status.state)
    {
    case GameBuildState::Idle:
        break;
    case GameBuildState::Configuring:
        label = editor_text(m_editing->language, "Configuring CMake...", "CMake を構成中...");
        break;
    case GameBuildState::Building:
        label = status.purpose == GameBuildPurpose::ScriptModule
                    ? editor_text(m_editing->language, "Building scripts...", "スクリプトをビルド中...")
                    : editor_text(m_editing->language, "Building and packaging game...",
                                  "ゲームをビルド・パッケージ化中...");
        break;
    case GameBuildState::Succeeded:
        label = isConfigureOnly
                    ? editor_text(m_editing->language, "CMake configuration succeeded", "CMake の構成が完了しました")
                    : editor_text(m_editing->language, "Build succeeded", "ビルドが完了しました");
        break;
    case GameBuildState::Failed:
        label = isConfigureOnly
                    ? editor_text(m_editing->language, "CMake configuration failed", "CMake の構成に失敗しました")
                    : editor_text(m_editing->language, "Build failed", "ビルドに失敗しました");
        break;
    case GameBuildState::Cancelled:
        label = isConfigureOnly
                    ? editor_text(m_editing->language, "CMake configuration cancelled", "CMake の構成を中止しました")
                    : editor_text(m_editing->language, "Build cancelled", "ビルドを中止しました");
        break;
    }
    m_editing->buildStatusText = label;
    if (status.error)
        m_editing->buildStatusText += ": " + status.error->operation;
    if (status.applicationExitCode)
        m_editing->buildStatusText +=
            std::string(editor_text(m_editing->language, " / Game exited: ", " / ゲームの終了コード: ")) +
            std::to_string(*status.applicationExitCode);
}

bool EditorHost::has_play_configuration() const noexcept
{
    if (m_pendingProject || !m_editing || !m_editing->application || m_editing->isGitReloadPending)
        return false;
    return m_playExecutable.has_value() || m_scriptModule.has_value() ||
           (m_project && m_project->descriptor().gameTarget.has_value());
}

bool EditorHost::needs_git_script_build() const noexcept
{
    if (!m_editing || !m_project)
        return false;
    const auto found = m_editing->staleBranchBuilds.find(m_project->file().utf8());
    return found != m_editing->staleBranchBuilds.end() &&
           found->second[static_cast<std::size_t>(m_editing->gameConfiguration)];
}

void EditorHost::record_git_script_build() noexcept
{
    if (!m_project || !m_editing)
        return;
    const auto found = m_editing->staleBranchBuilds.find(m_project->file().utf8());
    if (found != m_editing->staleBranchBuilds.end())
    {
        found->second[static_cast<std::size_t>(m_editing->gameConfiguration)] = false;
        if (std::none_of(found->second.begin(), found->second.end(), [](bool a_stale) { return a_stale; }))
            m_editing->staleBranchBuilds.erase(found);
    }
}

GameBuildConfiguration EditorHost::game_configuration() const noexcept
{
    return std::this_thread::get_id() == m_ownerId && m_editing ? m_editing->gameConfiguration
                                                                : GameBuildConfiguration::Development;
}

bool EditorHost::can_select_game_configuration() const noexcept
{
    return can_open_project() && m_editing->application && m_project && m_project->descriptor().gameTarget.has_value();
}

Result<void> EditorHost::request_script_creation(std::string_view a_name)
{
    if (std::this_thread::get_id() != m_ownerId)
        return Result<void>::failure({ErrorCategory::WrongThread, "EditorHost.create_script.owner"});
    if (!can_select_game_configuration() || !m_windows.file_system())
        return Result<void>::failure({ErrorCategory::InvalidState, "EditorHost.create_script.busy"});
    // 作成の確定と構成生成を分け、生成失敗後も保存した Source から再試行できるようにする
    auto notice = std::string(editor_text(m_editing->language, "Created Source/", "作成: Source/")) +
                  std::string(a_name) +
                  editor_text(m_editing->language, ".h and .cpp - Build Scripts required",
                              ".h と .cpp。スクリプトのビルドが必要です。");
    auto created = create_script_source(*m_windows.file_system(), *m_project, a_name);
    if (!created.has_value())
        return Result<void>::failure(*created.try_error());
    // 新しい型を含まない DLL の Metadata を Inspector に再利用しない
    m_editing->play.reset();
    m_editing->playScene.reset();
    m_editing->playObjectIndices.clear();
    m_editing->appliedViewTick.reset();
    m_editing->isGameBuildRequired = true;
    m_editing->error.clear();
    m_editing->notice = std::move(notice);
    if (!m_gameBuildService)
    {
        m_editing->error = editor_text(m_editing->language, "Script saved. CMake configuration unavailable",
                                       "スクリプトを保存しました。CMake の構成は利用できません。");
        return Result<void>::success();
    }
    auto target = current_native_game_build_target();
    if (target.has_value())
        target.try_value()->configuration = m_editing->gameConfiguration;
    // Script 用 Workspace を再生成し、Inspector へ型を反映する DLL Build は既存操作へ委ねる
    auto configured = target.has_value()
                          ? m_gameBuildService->start(*m_project, *target.try_value(), GameBuildPurpose::IdeProject)
                          : Result<void>::failure(*target.try_error());
    if (!configured.has_value())
        m_editing->error =
            std::string(editor_text(m_editing->language, "Script saved. CMake configuration could not start: ",
                                    "スクリプトを保存しました。CMake の構成を開始できませんでした: ")) +
            configured.try_error()->operation;
    return Result<void>::success();
}

Result<void> EditorHost::request_game_configuration(GameBuildConfiguration a_configuration)
{
    if (std::this_thread::get_id() != m_ownerId)
        return Result<void>::failure({ErrorCategory::WrongThread, "EditorHost.game_configuration.owner"});
    if (game_build_configuration_name(a_configuration).empty())
        return Result<void>::failure({ErrorCategory::InvalidArgument, "EditorHost.game_configuration"});
    if (!can_select_game_configuration())
        return Result<void>::failure({ErrorCategory::InvalidState, "EditorHost.game_configuration.busy"});
    if (a_configuration == m_editing->gameConfiguration)
        return Result<void>::success();
    // 保存成功を先に確認し、File 失敗では選択済み DLL と型情報をそのまま使える状態を維持する
    auto saved = save_editor_game_configuration(*m_windows.file_system(), *m_project, a_configuration);
    if (!saved.has_value())
        return saved;
    m_editing->gameConfiguration = a_configuration;
    m_editing->error.clear();
    m_editing->ui->reset();
    // 他構成の Metadata / DLL を再利用せず、未 Build ではその構成の Build Scripts を案内する
    auto prepared = prepare_play();
    if (prepared.has_value())
        prepared = m_editing->play->describe();
    if (!prepared.has_value())
        m_editing->notice =
            std::string(editor_text(m_editing->language, "Game: ", "ゲーム構成: ")) +
            std::string(game_build_configuration_name(a_configuration)) +
            editor_text(m_editing->language, " - Build Scripts required", "。スクリプトのビルドが必要です。");
    else
        m_editing->notice = std::string(editor_text(m_editing->language, "Game: ", "ゲーム構成: ")) +
                            std::string(game_build_configuration_name(a_configuration));
    return Result<void>::success();
}

Result<void> EditorHost::start_play()
{
    if (std::this_thread::get_id() != m_ownerId)
        return Result<void>::failure({ErrorCategory::WrongThread, "EditorHost.play.owner_thread"});
    if (m_isClosed || m_isInitializing || !m_editing || !m_editing->window || !m_editing->application ||
        m_editing->pendingChange || m_pendingProject || m_editing->isGitReloadPending ||
        (m_editing->scripts && m_editing->scripts->status().isBusy))
        return Result<void>::failure({ErrorCategory::InvalidState, "EditorHost.play.unavailable"});
    if (m_gameBuildService)
    {
        const auto state = m_gameBuildService->status().state;
        if (state == GameBuildState::Configuring || state == GameBuildState::Building ||
            m_gameBuildService->status().isApplicationRunning)
            return Result<void>::failure({ErrorCategory::InvalidState, "EditorHost.build.busy"});
    }
    auto prepared = prepare_play();
    if (!prepared.has_value())
        return prepared;
    auto candidate = m_editing->application->document().scene().descriptor();
    auto started = m_editing->play->start(m_editing->application->document().scene());
    if (started.has_value())
    {
        m_editing->playScene = std::move(candidate);
        m_editing->error.clear();
        m_editing->ui->reset();
    }
    return started;
}

Result<void> EditorHost::stop_play()
{
    if (std::this_thread::get_id() != m_ownerId)
        return Result<void>::failure({ErrorCategory::WrongThread, "EditorHost.play.owner_thread"});
    return m_editing && m_editing->play ? m_editing->play->stop() : Result<void>::success();
}

PlayState EditorHost::play_state() const noexcept
{
    return std::this_thread::get_id() == m_ownerId && m_editing && m_editing->play ? m_editing->play->state()
                                                                                   : PlayState::Idle;
}

const PlayViewSnapshot *EditorHost::play_view_snapshot() const noexcept
{
    return std::this_thread::get_id() == m_ownerId && m_editing && m_editing->play ? m_editing->play->view_snapshot()
                                                                                   : nullptr;
}

Result<void> EditorHost::request_play_transform(std::string_view a_objectId, const math::floatTransform &a_transform)
{
    if (std::this_thread::get_id() != m_ownerId)
        return Result<void>::failure({ErrorCategory::WrongThread, "EditorHost.play.owner_thread"});
    if (!m_editing || !m_editing->play || m_isClosed)
        return Result<void>::failure({ErrorCategory::InvalidState, "EditorHost.play.not_running"});
    auto requested = m_editing->play->set_transform(a_objectId, a_transform);
    return requested.has_value() ? Result<void>::success() : Result<void>::failure(*requested.try_error());
}

Result<void> EditorHost::prepare_play()
{
    if (!m_editing)
        return Result<void>::failure({ErrorCategory::InvalidState, "EditorHost.play.document"});
    if (!m_editing->application || m_editing->isGitReloadPending || needs_git_script_build())
        return Result<void>::failure({ErrorCategory::InvalidState, "EditorGit.build_scripts_required"});
    if (m_editing->play)
    {
        const auto state = m_editing->play->state();
        if (state == PlayState::Describing || state == PlayState::Starting || state == PlayState::Running ||
            state == PlayState::Stopping || state == PlayState::WaitingForDebugger)
            return Result<void>::failure({ErrorCategory::InvalidState, "EditorHost.play.busy"});
    }
    // 検証失敗で以前の DLL / Metadata を再利用しない。稼働している子は上の判定で保全する
    m_editing->play.reset();
    m_editing->playScene.reset();
    m_editing->playObjectIndices.clear();
    m_editing->appliedViewTick.reset();
    m_editing->isGameBuildRequired = true;
    auto *files = m_windows.file_system();
    if (!files)
        return Result<void>::failure({ErrorCategory::InvalidState, "EditorHost.play.file_system"});
    try
    {
        auto config = resolve_editor_play_config(*files, m_project ? &*m_project : nullptr, m_playExecutable,
                                                 m_scriptModule, m_editing->gameConfiguration);
        if (!config.has_value())
            return Result<void>::failure(*config.try_error());
        auto created = PlayClient::create(config.take_value());
        if (!created.has_value())
            return Result<void>::failure(*created.try_error());
        m_editing->play = created.take_value();
        m_editing->isGameBuildRequired = false;
        return Result<void>::success();
    }
    catch (...)
    {
        return Result<void>::failure({ErrorCategory::PlatformFailure, "EditorHost.play.prepare_exception"});
    }
}

void EditorHost::poll_play()
{
    if (!m_editing || !m_editing->play)
        return;
    auto &editing = *m_editing;
    auto &play = *editing.play;
    auto polled = play.poll();
    if (!polled.has_value())
        editing.error = polled.try_error()->operation;
    if (play.state() == PlayState::Failed)
        editing.error = std::string(play.error());
    // 確認応答だけを Play の表示値へ反映し、保存・Undo の正本へ流入させない
    for (const auto &ack : play.take_acknowledgements())
    {
        if (!ack.isAccepted)
        {
            editing.error = ack.error;
            continue;
        }
        if (!editing.playScene)
            continue;
        for (auto &object : editing.playScene->objects)
            if (object.objectId == ack.objectId)
                for (auto &script : object.scripts)
                    if (script.instanceId == ack.scriptInstanceId)
                    {
                        const auto found =
                            std::find_if(script.properties.begin(), script.properties.end(), [&](const auto &a_value)
                                         { return a_value.propertyId == ack.value.propertyId; });
                        if (found == script.properties.end())
                            script.properties.push_back(ack.value);
                        else
                            *found = ack.value;
                    }
    }
    for (const auto &ack : play.take_transform_acknowledgements())
        if (!ack.isAccepted)
            editing.error = ack.error;
    // Pose の表示値を実行側から更新する。SceneDocument / Preview World / 保存 Data は変更しない
    if (const auto *view = play.view_snapshot(); view && editing.playScene && editing.appliedViewTick != view->tick)
    {
        if (editing.playObjectIndices.empty())
        {
            const auto &objects = editing.playScene->objects;
            editing.playObjectIndices.reserve(objects.size());
            for (std::size_t index = 0; index < objects.size(); ++index)
                editing.playObjectIndices.emplace(objects[index].objectId, index);
        }
        for (const auto &pose : view->transforms)
            if (const auto found = editing.playObjectIndices.find(pose.objectId);
                found != editing.playObjectIndices.end())
                editing.playScene->objects[found->second].transform = pose.transform;
        editing.appliedViewTick = view->tick;
    }
    if ((play.state() == PlayState::Stopped || play.state() == PlayState::Failed) && editing.playScene)
    {
        editing.playScene.reset();
        editing.playObjectIndices.clear();
        editing.appliedViewTick.reset();
        // Play 中の Widget の Draft を次の編集 / 再 Play へ持ち越さない
        editing.ui->reset();
    }
}

/// @brief UI の所有値要求を Main 上で順に処理し、回復可能な操作失敗は通知に留める
Result<void> EditorHost::apply_editor_actions()
{
    if (!m_editing)
        return Result<void>::success();
    auto &editing = *m_editing;
    auto *app = editing.application.get();
    for (const auto &action : editing.actions)
    {
        const bool isConfirmation = action.kind == EditorActionKind::ConfirmSaveChanges ||
                                    action.kind == EditorActionKind::ConfirmDiscardChanges ||
                                    action.kind == EditorActionKind::CancelDocumentChange;
        // 同じ Frame で先に採用した切替要求を、後続の編集要求で変更しない
        if (editing.isGitReloadPending || ((editing.pendingChange || m_pendingProject) && !isConfirmation))
            continue;
        // 次の操作へ移ったら古い保存通知を消し、現在の Dirty 表示へ戻す
        editing.notice.clear();
        auto result = [&]() -> Result<void>
        {
            // 未選択画面でも文書に依存しない操作は受け付ける
            if (!app && action.kind != EditorActionKind::OpenProject && action.kind != EditorActionKind::Exit &&
                action.kind != EditorActionKind::ToggleDiagnostics && action.kind != EditorActionKind::SetLanguage)
                return Result<void>::failure({ErrorCategory::InvalidState, "EditorHost.action.no_project"});
            const bool isPlaying = editing.play && (editing.play->state() == PlayState::Starting ||
                                                    editing.play->state() == PlayState::Running ||
                                                    editing.play->state() == PlayState::Stopping ||
                                                    editing.play->state() == PlayState::WaitingForDebugger);
            // Play は起動時の Snapshot を所有する。実行中の通常編集を混ぜない
            if (isPlaying && action.kind != EditorActionKind::Stop &&
                action.kind != EditorActionKind::SetPlayTransform &&
                action.kind != EditorActionKind::SetSceneViewCamera &&
                action.kind != EditorActionKind::ResetSceneViewCamera &&
                action.kind != EditorActionKind::SetScriptProperty && action.kind != EditorActionKind::SelectObject &&
                action.kind != EditorActionKind::ToggleDiagnostics && action.kind != EditorActionKind::Exit &&
                action.kind != EditorActionKind::OpenIde && action.kind != EditorActionKind::OpenScriptSource &&
                action.kind != EditorActionKind::CancelScriptOperation &&
                action.kind != EditorActionKind::SetLanguage && !isConfirmation)
                return Result<void>::failure({ErrorCategory::InvalidState, "EditorHost.play.edit_locked"});
            switch (action.kind)
            {
            case EditorActionKind::SetLanguage:
            {
                const auto *paths = m_windows.storage_paths();
                auto *files = m_windows.file_system();
                if (!paths || !files)
                    return Result<void>::failure({ErrorCategory::InvalidState, "EditorHost.file_system"});
                // 保存に成功した言語を次 Frame から使用する
                auto saved = save_editor_language(*files, paths->dataRoot, action.language);
                if (saved.has_value())
                    editing.language = action.language;
                return saved;
            }
            case EditorActionKind::SetGameConfiguration:
                return request_game_configuration(action.gameConfiguration);
            case EditorActionKind::CreateScript:
                return request_script_creation(action.scriptName);
            case EditorActionKind::SetPlayTransform:
                return request_play_transform(action.objectId, action.transform);
            case EditorActionKind::SetSceneViewCamera:
            {
                auto view = math::view_from_transform(action.transform);
                if (!view.has_value())
                    return Result<void>::failure(*view.try_error());
                editing.sceneViewCamera = action.transform;
                return Result<void>::success();
            }
            case EditorActionKind::ResetSceneViewCamera:
                editing.sceneViewCamera.reset();
                return Result<void>::success();
            case EditorActionKind::OpenProject:
            {
                if (!can_open_project())
                    return Result<void>::failure({ErrorCategory::InvalidState, "EditorHost.project.busy"});
                auto chosen = m_projectDialog
                                  ? m_projectDialog(*editing.window, m_project ? &m_project->root() : nullptr)
                                  : choose_project_folder(*editing.window, m_project ? &m_project->root() : nullptr,
                                                          editing.language);
                if (!chosen.has_value())
                    return Result<void>::failure(*chosen.try_error());
                if (!*chosen.try_value())
                    return Result<void>::success();
                auto file = find_project_file(*m_windows.file_system(), **chosen.try_value());
                return file.has_value() ? request_open_project(*file.try_value())
                                        : Result<void>::failure(*file.try_error());
            }
            case EditorActionKind::OpenIde:
                return open_ide();
            case EditorActionKind::OpenScriptSource:
                return edit_script(action.scriptTypeId);
            case EditorActionKind::BuildScripts:
                return build_scripts();
            case EditorActionKind::DebugPlay:
                return debug_play();
            case EditorActionKind::CancelScriptOperation:
                return cancel_script_operation();
            case EditorActionKind::SetGameBuildTarget:
            {
                if (m_gameBuildService)
                {
                    const auto &status = m_gameBuildService->status();
                    if (status.state == GameBuildState::Configuring || status.state == GameBuildState::Building ||
                        status.isApplicationRunning)
                        return Result<void>::failure({ErrorCategory::InvalidState, "EditorHost.build.busy"});
                }
                // 設定 Draft は未選択 Architecture を保持できる。開始時に完全検証する
                editing.buildTarget = action.buildTarget;
                return Result<void>::success();
            }
            case EditorActionKind::BuildGame:
                return request_game_build(editing.buildTarget);
            case EditorActionKind::CancelGameBuild:
                return cancel_game_build();
            case EditorActionKind::RunGame:
                return run_game();
            case EditorActionKind::ReloadScripts:
            case EditorActionKind::Play:
            case EditorActionKind::Stop:
            {
                auto requested = [&]() -> Result<void>
                {
                    if (action.kind == EditorActionKind::Stop)
                    {
                        if (editing.scripts &&
                            (editing.scripts->status().isBusy || (editing.play && editing.play->is_debugging())))
                        {
                            auto cancelled = editing.scripts->cancel();
                            if (!cancelled.has_value())
                                return cancelled;
                        }
                        return editing.play ? editing.play->stop() : Result<void>::success();
                    }
                    if (editing.scripts && editing.scripts->status().isBusy)
                        return Result<void>::failure({ErrorCategory::InvalidState, "EditorHost.ide.busy"});
                    if (m_gameBuildService)
                    {
                        const auto state = m_gameBuildService->status().state;
                        if (state == GameBuildState::Configuring || state == GameBuildState::Building)
                            return Result<void>::failure({ErrorCategory::InvalidState, "EditorHost.build.busy"});
                    }
                    if (action.kind == EditorActionKind::ReloadScripts)
                    {
                        // Reload の直前も成果物を再検証し、古い Hash / Path を再利用しない
                        auto prepared = prepare_play();
                        if (!prepared.has_value())
                            return prepared;
                        return editing.play->describe();
                    }
                    return start_play();
                }();
                if (!requested.has_value())
                {
                    auto error = *requested.try_error();
                    // ゲーム DLL・子 Process の Fatal は Editor 自体の Fatal にしない
                    error.category = ErrorCategory::PlatformFailure;
                    return Result<void>::failure(std::move(error));
                }
                return requested;
            }
            case EditorActionKind::AddScript:
            case EditorActionKind::RemoveScript:
            case EditorActionKind::SetScriptEnabled:
            case EditorActionKind::SetScriptProperty:
            {
                if (isPlaying)
                {
                    auto requested =
                        editing.play->set_property(action.objectId, action.scriptInstanceId, action.property);
                    if (!requested.has_value())
                    {
                        auto error = *requested.try_error();
                        error.category = ErrorCategory::PlatformFailure;
                        return Result<void>::failure(std::move(error));
                    }
                    return Result<void>::success();
                }
                const auto &objects = app->document().scene().descriptor().objects;
                const auto object = std::find_if(objects.begin(), objects.end(), [&](const auto &a_object)
                                                 { return a_object.objectId == action.objectId; });
                if (object == objects.end())
                    return Result<void>::failure({ErrorCategory::InvalidArgument, "EditorHost.script.object"});
                auto scripts = object->scripts;
                if (action.kind == EditorActionKind::AddScript)
                {
                    if (!editing.play)
                        return Result<void>::failure({ErrorCategory::InvalidState, "EditorHost.script.types"});
                    const auto &types = editing.play->types();
                    const auto type = std::find_if(types.begin(), types.end(), [&](const auto &a_type)
                                                   { return a_type.typeId == action.scriptTypeId; });
                    if (type == types.end())
                        return Result<void>::failure({ErrorCategory::InvalidArgument, "EditorHost.script.type"});
                    auto id = create_editor_object_id();
                    if (!id.has_value())
                        return Result<void>::failure(*id.try_error());
                    ScriptDescriptor script;
                    script.instanceId = id.take_value();
                    script.typeId = type->typeId;
                    script.schemaVersion = type->schemaVersion;
                    for (const auto &property : type->properties)
                        script.properties.push_back(
                            {property.propertyId, property.kind, property.defaultNumber, property.defaultBoolean});
                    scripts.push_back(std::move(script));
                }
                else
                {
                    const auto script = std::find_if(scripts.begin(), scripts.end(), [&](const auto &a_script)
                                                     { return a_script.instanceId == action.scriptInstanceId; });
                    if (script == scripts.end())
                        return Result<void>::failure({ErrorCategory::InvalidArgument, "EditorHost.script.instance"});
                    if (action.kind == EditorActionKind::RemoveScript)
                        scripts.erase(script);
                    else if (action.kind == EditorActionKind::SetScriptEnabled)
                        script->isEnabled = action.isEnabled;
                    else
                    {
                        const auto property = std::find_if(
                            script->properties.begin(), script->properties.end(), [&](const auto &a_property)
                            { return a_property.propertyId == action.property.propertyId; });
                        if (property == script->properties.end())
                            script->properties.push_back(action.property);
                        else
                            *property = action.property;
                    }
                }
                return app->set_scripts(action.objectId, std::move(scripts));
            }
            case EditorActionKind::NewScene:
            case EditorActionKind::OpenScene:
            case EditorActionKind::Exit:
            {
                std::optional<Path> file;
                if (action.kind == EditorActionKind::OpenScene)
                {
                    auto chosen =
                        choose_scene_file(*editing.window, false, app->document().file(),
                                          m_project ? &m_project->content_directory() : nullptr, editing.language);
                    if (!chosen.has_value())
                        return Result<void>::failure(*chosen.try_error());
                    file = chosen.take_value();
                    if (!file)
                        return Result<void>::success();
                }
                const auto kind = action.kind == EditorActionKind::NewScene    ? DocumentChangeKind::NewScene
                                  : action.kind == EditorActionKind::OpenScene ? DocumentChangeKind::OpenScene
                                                                               : DocumentChangeKind::Exit;
                auto confirmed = begin_document_change(kind, file);
                if (!confirmed.has_value())
                    return Result<void>::failure(*confirmed.try_error());
                if (!*confirmed.try_value())
                    return Result<void>::success();
                return execute_document_change(kind, file ? &*file : nullptr);
            }
            case EditorActionKind::ConfirmSaveChanges:
            case EditorActionKind::ConfirmDiscardChanges:
            case EditorActionKind::CancelDocumentChange:
            {
                if (!editing.pendingChange)
                    return Result<void>::success();
                if (editing.pendingChange->kind == DocumentChangeKind::OpenProject)
                    return confirm_open_project(
                        action.kind == EditorActionKind::ConfirmSaveChanges      ? ProjectOpenSaveChoice::Save
                        : action.kind == EditorActionKind::ConfirmDiscardChanges ? ProjectOpenSaveChoice::Discard
                                                                                 : ProjectOpenSaveChoice::Cancel);
                if (editing.pendingChange->kind == DocumentChangeKind::BuildGame)
                    return confirm_game_build(
                        action.kind == EditorActionKind::ConfirmSaveChanges      ? GameBuildSaveChoice::Save
                        : action.kind == EditorActionKind::ConfirmDiscardChanges ? GameBuildSaveChoice::SavedFiles
                                                                                 : GameBuildSaveChoice::Cancel);
                // 応答を一回だけ消費する。保存取消・失敗でも古い要求を後から実行しない
                auto pending = std::move(*editing.pendingChange);
                editing.pendingChange.reset();
                // 次の Pump が直ちに Close を受けても、新しい Modal を表示できるよう応答状態を解放する
                editing.ui->reset();
                if (action.kind == EditorActionKind::CancelDocumentChange)
                    return Result<void>::success();
                if (action.kind == EditorActionKind::ConfirmSaveChanges)
                {
                    auto saved = save_editor_scene(false);
                    if (!saved.has_value())
                        return Result<void>::failure(*saved.try_error());
                    if (!*saved.try_value())
                        return Result<void>::success();
                }
                return execute_document_change(pending.kind, pending.file ? &*pending.file : nullptr);
            }
            case EditorActionKind::SaveScene:
            case EditorActionKind::SaveSceneAs:
            {
                auto saved = save_editor_scene(action.kind == EditorActionKind::SaveSceneAs);
                return saved.has_value() ? Result<void>::success() : Result<void>::failure(*saved.try_error());
            }
            case EditorActionKind::AddCube:
            {
                auto added = app->add_cube();
                return added.has_value() ? Result<void>::success() : Result<void>::failure(*added.try_error());
            }
            case EditorActionKind::SelectObject:
                return app->select_object(action.objectId);
            case EditorActionKind::BeginTransformEdit:
                return app->begin_transform_edit(action.objectId);
            case EditorActionKind::PreviewTransform:
                return app->preview_transform(action.transform);
            case EditorActionKind::CommitTransformEdit:
                return app->commit_transform_edit();
            case EditorActionKind::CancelTransformEdit:
                return app->cancel_transform_edit();
            case EditorActionKind::Undo:
                return app->undo();
            case EditorActionKind::Redo:
                return app->redo();
            case EditorActionKind::ToggleDiagnostics:
                editing.isDiagnosticsVisible = !editing.isDiagnosticsVisible;
                return Result<void>::success();
            }
            return Result<void>::failure({ErrorCategory::InvalidArgument, "EditorHost.editor_action"});
        }();
        if (!result.has_value())
        {
            // 致命的状態だけは上位へ返し、入力値・File の失敗では現 Document を維持する
            if (result.try_error()->category == ErrorCategory::Fatal)
                return result;
            editing.error = result.try_error()->operation;
            editing.notice.clear();
            if (app && app->document().is_transform_editing())
            {
                auto canceled = app->cancel_transform_edit();
                if (!canceled.has_value())
                    return canceled;
            }
            editing.ui->reset();
            break;
        }
        editing.error.clear();
    }
    editing.actions.clear();
    return Result<void>::success();
}

/// @brief Graph が Manager を借用しなくなってから Context を停止する
Result<void> EditorHost::shutdown_ui()
{
    if (m_sceneRenderer)
    {
        auto stopped = m_sceneRenderer->shutdown();
        if (!stopped.has_value())
            return stopped;
        m_sceneRenderer.reset();
    }
    if (!m_imgui)
    {
        return Result<void>::success();
    }
    auto result = m_imgui->shutdown();
    // Context が既に失効した場合も Manager の所有権を回収する
    if (result.has_value() || !m_imgui->frame_info().has_value())
    {
        m_imgui.reset();
    }
    return result;
}
/// @brief 検証済み要求を下位 Host へ渡し、UI 構築中に Worker を停止しない
Result<void> EditorHost::request_performance_settings(PerformanceSettings a_settings)
{
    return m_windows.request_performance_settings(a_settings);
}

/// @brief 下位 Host の適用済み設定だけを参照する
Result<PerformanceSettings> EditorHost::performance_settings() const
{
    return m_windows.performance_settings();
}

/// @brief 要求値と実効同期状態を混同せず、下位 Host の所有 Snapshot を返す
Result<PerformanceSettingsStatus> EditorHost::performance_settings_status() const
{
    return m_windows.performance_settings_status();
}

/// @brief 適用途中の重複変更を UI が抑制できるようにする
bool EditorHost::has_pending_performance_settings() const noexcept
{
    return m_windows.has_pending_performance_settings();
}

/// @brief 性能設定の編集値と実行値を分離し、Apply によって変更境界へ要求する
Result<void> EditorHost::build_performance_settings_ui()
{
    auto applied = m_windows.performance_settings();
    if (!applied.has_value())
    {
        return Result<void>::failure(*applied.try_error());
    }
    // 編集用の値は最初の表示時に実行中の値から作る
    if (!m_settingsDraft)
    {
        m_settingsDraft = *applied.try_value();
    }
    const auto language = m_editing ? m_editing->language : EditorLanguage::English;
    const auto text = [language](const char *a_english, const char *a_japanese)
    { return editor_text(language, a_english, a_japanese); };
    ImGui::Begin(text("Test", "診断###Test"));
    if (ImGui::CollapsingHeader(text("Performance settings", "パフォーマンス設定###Performance settings")))
    {
        auto &settings = *m_settingsDraft;
        auto &frame = settings.frame;
        auto &gpu = settings.gpu;
        auto &present = settings.presentation;
        const bool isPending = m_windows.has_pending_performance_settings();
        ImGui::Text(text("Active request: %u FPS / buffering %u", "現在の要求値: %u FPS / バッファリング %u"),
                    applied.try_value()->frame.maxFps, applied.try_value()->gpu.resourceFrameCount);
        ImGui::Text(text("Derived: workers %s / CPU slots %u / update lead %u",
                         "要求から算出: ワーカー %s / CPU 枠 %u / 更新先行数 %u"),
                    applied.try_value()->frame.useWorkerThreads ? "ON" : "OFF",
                    applied.try_value()->frame.maxFramesInFlight, update_lead_frames(applied.try_value()->frame));
        ImGui::Text(text("Unfinished GPU limit: %u", "GPU 未完了フレーム上限: %u"),
                    applied.try_value()->gpu.maxFramesInFlight);
        ImGui::TextUnformatted(isPending ? text("Applying at frame boundary...", "フレーム境界での適用を待機中...")
                                         : text("Ready", "準備完了"));
        // 要求値と実際に採用された同期条件を別々に表示する
        auto status = m_windows.performance_settings_status();
        if (status.has_value())
        {
            const auto &requested = status.try_value()->requested.frame;
            const auto &effective = status.try_value()->effective.frame;
            ImGui::Text(text("Effective: workers %s / CPU slots %u / update lead %u",
                             "実行中: ワーカー %s / CPU 枠 %u / 更新先行数 %u"),
                        effective.useWorkerThreads ? "ON" : "OFF", effective.maxFramesInFlight,
                        effective.maxUpdateLeadFrames.value_or(0));
            ImGui::Text(
                text("Spin requested: %.3f us / effective: %.3f us", "スピン待機の要求値: %.3f us / 適用値: %.3f us"),
                std::chrono::duration<double, std::micro>(requested.spinDuration).count(),
                std::chrono::duration<double, std::micro>(effective.spinDuration).count());
            ImGui::Text(text("Prediction slack requested: %.3f us / effective: %.3f us",
                             "表示予測の余裕時間の要求値: %.3f us / 適用値: %.3f us"),
                        std::chrono::duration<double, std::micro>(requested.predictionSlack).count(),
                        std::chrono::duration<double, std::micro>(effective.predictionSlack).count());
            const auto &queued = status.try_value()->effective.presentation.maxQueuedFrames;
            if (queued)
            {
                ImGui::Text(
                    text("Display queue requested: %u / effective: %u", "表示キュー上限の要求値: %u / 適用値: %u"),
                    status.try_value()->requested.presentation.maxQueuedFrames, *queued);
            }
            else
            {
                ImGui::Text(text("Display queue requested: %u / effective: inactive (Waitable OFF)",
                                 "表示キュー上限の要求値: %u / 適用なし（待機可能な表示が無効）"),
                            status.try_value()->requested.presentation.maxQueuedFrames);
            }
            const char *modes[] = {text("Fixed period", "固定間隔"), text("Presentation ready", "表示準備待ち"),
                                   text("Display prediction", "表示時刻を予測")};
            ImGui::Text(text("Sync requested: %s / effective: %s", "同期の要求値: %s / 適用値: %s"),
                        modes[static_cast<int>(status.try_value()->requested.frame.syncMode)],
                        modes[static_cast<int>(status.try_value()->effective.frame.syncMode)]);
            if (status.try_value()->syncFallback != FrameSyncFallback::None)
            {
                ImGui::TextUnformatted(
                    status.try_value()->syncFallback == FrameSyncFallback::DisplayTimingUnavailable
                        ? text("Fallback: prediction needs VSync and fresh DXGI statistics",
                               "表示時刻の予測には VSync と最新の DXGI "
                               "統計が必要なため、代替の同期方法を使用しています。")
                        : text("Fallback: waitable presentation unavailable",
                               "待機可能な表示を利用できないため、代替の同期方法を使用しています。"));
            }
            if (status.try_value()->requested.presentation.isTearingAllowed &&
                !status.try_value()->effective.presentation.isTearingAllowed)
            {
                ImGui::TextUnformatted(text("Tearing requested but unsupported by DXGI",
                                            "ティアリングを許可する要求は DXGI でサポートされていません。"));
            }
        }
        ImGui::BeginDisabled(isPending || !m_performancePolicy.usersMayEdit);
        // Preset は編集値へ反映し、実行構成の変更は Apply まで保留する
        if (ImGui::Button(text("Balanced", "バランス###Balanced")))
        {
            settings = performance_preset(PerformancePreset::Balanced);
        }
        ImGui::SameLine();
        if (ImGui::Button(text("Low latency", "低遅延###Low latency")))
        {
            settings = performance_preset(PerformancePreset::LowLatency);
        }
        ImGui::SameLine();
        if (ImGui::Button(text("Throughput", "スループット###Throughput")))
        {
            settings = performance_preset(PerformancePreset::Throughput);
        }
        // Policy を越える入力は要求時にも検証する。Widget の値だけを信用しない
        auto slider = [](const char *a_label, std::uint32_t &a_value, int a_min, int a_max)
        {
            int value = static_cast<int>(a_value);
            if (ImGui::SliderInt(a_label, &value, a_min, a_max))
            {
                a_value = static_cast<std::uint32_t>(value);
                return true;
            }
            return false;
        };
        slider(text("FPS limit (0: unlimited)", "FPS 上限（0 で無制限）###FPS limit (0: unlimited)"), frame.maxFps,
               m_performancePolicy.allowsUnlimitedFps ? 0 : 1, static_cast<int>(m_performancePolicy.maxFps));
        // 一つの枠数から CPU / GPU 容量、Worker と先行数を同時に更新する
        // CPU・GPU・先行数の Policy が許す共通の枠数上限を求める
        const auto maxBuffering = (std::min)({8u, m_performancePolicy.maxCpuFrames, m_performancePolicy.maxGpuFrames,
                                              (std::min)(m_performancePolicy.maxUpdateLeadFrames, 7u) + 1});
        auto buffering = gpu.resourceFrameCount;
        ImGui::BeginDisabled(maxBuffering == 0);
        if (slider(text("Buffering (frames)", "バッファリング（フレーム数）###Buffering (frames)"), buffering, 1,
                   static_cast<int>((std::max)(maxBuffering, 1u))))
        {
            // Buffering 変更から CPU Snapshot・Worker・Update 先行数を一括導出する
            auto edited = set_frame_buffering(settings, buffering);
            if (!edited.has_value())
            {
                m_settingsError = edited.try_error()->operation;
            }
        }
        ImGui::EndDisabled();
        ImGui::Text(text("%s buffering: workers %s / CPU slots %u / update lead %u",
                         "%sバッファリング: ワーカー %s / CPU 枠 %u / 更新先行数 %u"),
                    buffering == 1   ? text("Single", "シングル")
                    : buffering == 2 ? text("Double", "ダブル")
                    : buffering == 3 ? text("Triple", "トリプル")
                                     : text("Multi-frame", "複数フレームの"),
                    frame.useWorkerThreads ? "ON" : "OFF", frame.maxFramesInFlight, update_lead_frames(frame));
        slider(text("Unfinished GPU frames", "GPU 未完了フレーム上限###Unfinished GPU frames"), gpu.maxFramesInFlight,
               1, static_cast<int>(gpu.resourceFrameCount));
        slider(text("SwapChain buffers", "スワップチェーンのバッファー数###SwapChain buffers"), present.bufferCount, 2,
               static_cast<int>(m_performancePolicy.maxSwapChainBuffers));
        ImGui::Checkbox(text("Waitable presentation", "待機可能な表示###Waitable presentation"),
                        &present.isWaitableEnabled);
        ImGui::BeginDisabled(!performance_settings_controls(settings).canEditQueuedPresents);
        slider(text("Queued presents", "表示キュー上限###Queued presents"), present.maxQueuedFrames, 1,
               static_cast<int>(m_performancePolicy.maxQueuedFrames));
        ImGui::EndDisabled();
        if (!present.isWaitableEnabled)
        {
            ImGui::TextWrapped(text("Queued presents requires Waitable presentation; the request is retained",
                                    "表示キュー上限の適用には、待機可能な表示が必要です。要求値は保持されます。"));
        }
        // VSync を有効にすると両立できない Tearing を解除する
        if (ImGui::Checkbox("VSync", &present.isVSyncEnabled) && present.isVSyncEnabled)
        {
            present.isTearingAllowed = false;
        }
        // Tearing を有効にすると VSync を解除する
        if (ImGui::Checkbox(text("Allow tearing", "ティアリングを許可###Allow tearing"), &present.isTearingAllowed) &&
            present.isTearingAllowed)
        {
            present.isVSyncEnabled = false;
        }
        ImGui::BeginDisabled(!performance_settings_controls(settings).canEditWaitMode);
        int wait = static_cast<int>(frame.waitMode);
        if (ImGui::Combo(text("Wait method", "待機方法###Wait method"), &wait,
                         text("Sleep\0Sleep + short spin\0", "スリープ\0スリープ＋短いスピン待機\0")))
        {
            frame.waitMode = static_cast<FrameWaitMode>(wait);
        }
        ImGui::EndDisabled();
        // FPS 無制限では時間制限用の待機設定を未適用として表示する
        if (frame.maxFps == 0)
        {
            ImGui::TextWrapped(text("FPS wait method is inactive with an unlimited FPS request",
                                    "FPS が無制限のときは FPS 制限の待機方法を適用しません。"));
        }
        ImGui::BeginDisabled(!performance_settings_controls(settings).canEditSpin);
        int spin = static_cast<int>(std::chrono::duration_cast<std::chrono::microseconds>(frame.spinDuration).count());
        if (ImGui::SliderInt(text("Spin (us)", "スピン待機（us）###Spin (us)"), &spin, 0, 5000))
        {
            frame.spinDuration = std::chrono::microseconds(spin);
        }
        ImGui::EndDisabled();
        if (!performance_settings_controls(settings).canEditSpin)
        {
            ImGui::TextWrapped(
                text("Spin requires an FPS limit and Sleep + short spin; the request is retained",
                     "スピン待機の適用には、FPS 上限と「スリープ＋短いスピン待機」が必要です。要求値は保持されます。"));
        }
        else
        {
            ImGui::TextWrapped(text("Spin is capped at 1/8 of the FPS period in both thread modes",
                                    "スピン待機の上限は、どのスレッド構成でも FPS 間隔の 1/8 です。"));
        }
        int sync = static_cast<int>(frame.syncMode);
        if (ImGui::Combo(text("Synchronization", "同期方法###Synchronization"), &sync,
                         text("Fixed period\0Presentation ready\0Display prediction\0",
                              "固定間隔\0表示準備待ち\0表示時刻を予測\0")))
        {
            frame.syncMode = static_cast<FrameSyncMode>(sync);
        }
        ImGui::BeginDisabled(!performance_settings_controls(settings).canEditPredictionSlack);
        int slack =
            static_cast<int>(std::chrono::duration_cast<std::chrono::microseconds>(frame.predictionSlack).count());
        if (ImGui::SliderInt(text("Prediction slack (us)", "表示予測の余裕時間（us）###Prediction slack (us)"), &slack,
                             0, 100000))
        {
            frame.predictionSlack = std::chrono::microseconds(slack);
        }
        ImGui::EndDisabled();
        if (!performance_settings_controls(settings).canEditPredictionSlack)
        {
            ImGui::TextWrapped(
                text("Prediction slack requires Display prediction, Waitable and VSync; the request is retained",
                     "余裕時間の適用には、表示時刻の予測、待機可能な表示、VSync が必要です。要求値は保持されます。"));
        }
        // 編集値を検証し、次の安全な Main 境界での適用を要求する
        if (ImGui::Button(text("Apply", "適用###Apply")))
        {
            auto requested = request_performance_settings(settings);
            m_settingsError = requested.has_value() ? std::string{} : requested.try_error()->operation;
        }
        ImGui::EndDisabled();
        if (!m_settingsError.empty())
        {
            ImGui::Text(text("Settings error: %s", "設定エラー: %s"), m_settingsError.c_str());
        }
    }
    ImGui::End();
    return Result<void>::success();
}
/// @brief ユーザーの FPS / 表示設定と独立した開発者の更新設定を下位 Host へ渡す
Result<void> EditorHost::request_simulation_config(SimulationConfig a_config)
{
    return m_windows.request_simulation_config(a_config);
}

/// @brief Frame ごとに保存した不変の Tick と補間係数を取得する
Result<FrameSimulationInfo> EditorHost::frame_simulation_info(std::uint64_t a_frame) const
{
    return m_windows.frame_simulation_info(a_frame);
}
} // namespace cue
