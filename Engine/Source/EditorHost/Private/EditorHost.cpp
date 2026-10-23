#include <EditorHost/EditorHost.h>

#include <Foundation/TimingTrace.h>

#include <algorithm>
#include <chrono>
#include <new>
#include <utility>

#include <imgui.h>

#include <EditorHost/ImGuiPass.h>
#include <EditorHost/PerformanceSettingsEditing.h>
#include <Foundation/ScopedFlag.h>
#include <Platform/Diagnostics.h>

namespace cue
{
namespace
{
/// @brief Sample がある CPU 区間の平均・p95・最大経過時間を ms で表示する
void show_timing(const char *a_name, const TimingStatistics &a_statistics)
{
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
                                             const InputLatencyInfo &a_inputLatency)
{
    // 初回の位置と寸法だけ指定し、以後の移動と Layout 保存を妨げない
    ImGui::SetNextWindowPos({32.0f, 32.0f}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize({640.0f, 480.0f}, ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Test"))
    {
        ImGui::Text("TEST");
        // 開始制限後も Render 完了時刻は揺れるため、直近の平均間隔から完了頻度を求める
        // 各 Frame の FPS を平均せず、Sample 数 / 合計経過時間と等価な値を使う
        const auto &intervals = a_frameTiming.frameInterval;
        const auto interval = std::chrono::duration<double>(intervals.averageDuration).count();
        if (intervals.sampleCount != 0 && interval > 0.0)
        {
            ImGui::Text("FrameController FPS: %.1f (avg %zu intervals)", 1.0 / interval, intervals.sampleCount);
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
            ImGui::Text("FrameController FPS (instant): %.1f", 1.0 / lastInterval);
        }
        else
        {
            ImGui::Text("FrameController FPS (instant): --");
        }
        // GPU 時間と混同しないよう CPU の経過時間であることと集計枠数を表示する
        ImGui::Text("CPU timings (last 120 samples)");
        show_timing("Main", a_frameTiming.main);
        show_timing("Update", a_frameTiming.update);
        show_timing("Render", a_frameTiming.render);
        // 平均 FPS だけでは見えない Frame Pacing の揺れを実際の完了間隔で確認する
        show_timing("Frame interval", intervals);
        show_timing("FPS wait", a_frameTiming.limitWait);
        show_timing("Post-Present readiness wait", a_frameTiming.presentationWait);
        show_timing("Cycle interval", a_frameTiming.cycleInterval);
        show_timing("Frame adopted -> Render start", a_frameTiming.inputToRenderStart);
        show_timing("Frame adopted -> CPU Render complete", a_frameTiming.inputToRenderComplete);
        ImGui::TextUnformatted("Input event latency (last 120 reflected events, CPU only)");
        ImGui::TextUnformatted("Origin: Window message dispatch; Present return is not screen display");
        show_timing("Input -> Frame capture", a_inputLatency.toFrameCapture);
        show_timing("Input -> Render start", a_inputLatency.toRenderStart);
        show_timing("Input -> Present call", a_inputLatency.toPresentStart);
        show_timing("Input -> Present return", a_inputLatency.toPresentReturn);
        show_timing("Input -> CPU Render complete", a_inputLatency.toCpuComplete);
        ImGui::Text("Input observed / reflected / presented: %llu / %llu / %llu",
                    static_cast<unsigned long long>(a_inputLatency.observedEvents),
                    static_cast<unsigned long long>(a_inputLatency.assignedEvents),
                    static_cast<unsigned long long>(a_inputLatency.presentedEvents));
        ImGui::Text("Input skipped / invalid: %llu / %llu; last event / frame / present: %llu / %llu / %llu",
                    static_cast<unsigned long long>(a_inputLatency.skippedEvents),
                    static_cast<unsigned long long>(a_inputLatency.invalidEvents),
                    static_cast<unsigned long long>(a_inputLatency.lastEventId),
                    static_cast<unsigned long long>(a_inputLatency.lastFrame),
                    static_cast<unsigned long long>(a_inputLatency.lastPresent));
        show_timing("UI build", a_uiTiming.uiBuild);
        show_timing("Snapshot copy", a_uiTiming.snapshotCopy);
        show_timing("ImGui lock wait", a_uiTiming.contextWait);
        show_timing("ImGui GPU fence wait", a_uiTiming.gpuWait);
        show_timing("Graph record", a_graphPerformance.record);
        show_timing("Graph frame fence wait", a_graphPerformance.frameWait);
        show_timing("Present total", a_graphPerformance.present);
        show_timing("Present API", a_graphPerformance.presentApi);
        show_timing("Presentation statistics", a_graphPerformance.presentationStatistics);
        show_timing("Main admission wait (per check)", a_graphPerformance.admissionWait);
        show_timing("Main GPU wait (per check)", a_graphPerformance.mainGpuWait);
        show_timing("Main DXGI wait (per check)", a_graphPerformance.mainPresentationWait);
        show_timing("Main resource wait (per check)", a_graphPerformance.mainResourceWait);
        show_timing("GPU queue limit wait", a_graphPerformance.gpuQueueWait);
        show_timing("Pre-record readiness wait", a_graphPerformance.presentationReadyWait);
        show_timing("Present return interval", a_graphPerformance.presentInterval);
        show_timing("Display interval (DXGI derived)", a_graphPerformance.displayInterval);
        ImGui::Text("Unfinished GPU frames: %u / peak %u", a_graphPerformance.unfinishedGpuFrames,
                    a_graphPerformance.peakUnfinishedGpuFrames);
        ImGui::Text("DXGI display timing: %s", !a_graphPerformance.presentation.hasDisplayTiming ? "unavailable"
                    : a_graphPerformance.presentation.isDisplayTimeEstimated ? "estimated from refresh" : "QPC sample");
        ImGui::Text("GPU timings (last 120 completed samples)");
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

  private:
    const std::unique_ptr<ImGuiManager> *m_manager;
};

} // namespace

/// @brief Editor の表示設定を保持し、下位基盤へ Editor 型を渡さない
EditorHost::EditorHost(EditorHostConfig a_config)
    : m_imguiConfig(std::move(a_config.imgui)),
      m_buildUi(a_config.buildUi ? std::move(a_config.buildUi)
                                 : editorUiCallback{[this]() -> Result<void>
                                                    {
                                                        // UI 開始前に取得した同じ計測 Snapshot で FPS と区間を表示する
                                                        auto result =
                                                            build_test_window(m_frameTiming, m_uiTiming,
                                                                              m_graphPerformance, m_inputLatency);
                                                        return result.has_value() ? build_performance_settings_ui()
                                                                                  : std::move(result);
                                                    }}),
      m_ownerId(std::this_thread::get_id()), m_performancePolicy(a_config.performancePolicy),
      m_performanceSettingsFile(a_config.performanceSettingsFile),
      m_windows({std::move(a_config.window),
                 frame_config_for_buffering(a_config.frame, a_config.gpu.resourceFrameCount),
                 a_config.presentation,
                 prepare_graph(std::move(a_config.graph)),
                 {[this](Window &a_window) { return initialize_ui(a_window); },
                  {},
                  [this]() { return shutdown_ui(); },
                  [this](IBackend &a_backend, std::uint32_t a_cpuFrames, std::uint32_t a_gpuFrames)
                  {
                      auto result = m_imgui->initialize_renderer(a_backend, a_gpuFrames);
                      return result.has_value() ? m_imgui->enable_frame_transfer(a_cpuFrames) : std::move(result);
                  },
                  [this](std::uint64_t a_frame, std::stop_token a_token) { return build_ui(a_frame, a_token); },
                  [this](std::uint64_t a_frame, std::stop_token a_token, const FrameCallback &a_record)
                  {
                      auto result = m_imgui->render_frame(a_frame, a_token, a_record, &m_wasInputRendered);
                      m_inputRenderFrame = a_frame;
                      return result;
                  },
                  [this](IBackend &a_backend, std::uint32_t a_cpuFrames, std::uint32_t a_gpuFrames)
                  { return m_imgui->reconfigure_renderer(a_backend, a_cpuFrames, a_gpuFrames); },
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
                  std::move(a_config.createBackend)},
                 std::move(a_config.storage),
                 std::move(a_config.fileSystem),
                 a_config.logging,
                 a_config.gpu,
                 a_config.performancePolicy,
                 a_config.performanceSettingsFile,
                 a_config.simulation})
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

/// @brief Editor の表示を抽象 Pass として下位 Host へ渡し、明示された Pass は優先する
MainFrameGraphConfig EditorHost::prepare_graph(MainFrameGraphConfig a_config)
{
    if (!a_config.displayPass && !a_config.displayPassFactory)
    {
        // Resize 後の新しい Graph Handle を持つ Pass を作る。Manager と Texture Heap は継続所有する
        a_config.displayPassFactory = [this]()
        { return std::make_unique<ImGuiPass>(std::make_unique<EditorImGuiRenderer>(m_imgui)); };
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
    return m_windows.initialize();
}

/// @brief 構築 Thread 上で Message と Frame の進行を委譲する
Result<bool> EditorHost::step()
{
    if (std::this_thread::get_id() != m_ownerId)
    {
        return Result<bool>::failure({ErrorCategory::WrongThread, "EditorHost.step"});
    }
    if (m_isStepping)
    {
        return Result<bool>::failure({ErrorCategory::InvalidState, "EditorHost.step"});
    }
    ScopedFlag stepping(m_isStepping);
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
    if (m_isStepping)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "EditorHost.shutdown"});
    }
    return m_windows.shutdown();
}

/// @brief 下位 Host が追跡した入力到達と UI Frame の対応を所有値で返す
Result<InputLatencyInfo> EditorHost::input_latency_info() const
{
    return m_windows.input_latency_info();
}

/// @brief Window の生成 Thread に UI の Owner を固定する
Result<void> EditorHost::initialize_ui(Window &a_window)
{
    auto *files = m_windows.file_system();
    const auto *paths = m_windows.storage_paths();
    if (!files || !paths)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "EditorHost.file_system"});
    }
    m_imguiConfig.fileSystem = files;
    if (!m_imguiConfig.settingsFile.empty())
    {
        auto path = paths->dataRoot.join(m_imguiConfig.settingsFile);
        if (!path.has_value())
        {
            return Result<void>::failure(*path.try_error());
        }
        m_imguiConfig.settingsFile = path.try_value()->utf8();
    }
    auto result = ImGuiManager::create(a_window, std::move(m_imguiConfig));
    if (!result.has_value())
    {
        return Result<void>::failure(*result.try_error());
    }
    m_imgui = result.take_value();
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
    if (m_savesAfterApply && !m_windows.has_pending_performance_settings())
    {
        m_savesAfterApply = false;
        auto saved = save_performance_settings();
        m_settingsError = saved.has_value() ? std::string{} : saved.try_error()->operation;
    }
    // Render は Graph Lock から ImGui Context を借りるため、逆順で同時に保持しない
    // 計測 Snapshot を UI 開始前に取得し、Callback 内では所有値だけを表示する
    TimingTraceScope traceStatistics("Editor.Statistics");
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
    auto built = m_imgui->build_frame(m_buildUi);
    return built.has_value() ? m_imgui->publish_frame(a_frame, a_stopToken) : std::move(built);
}

/// @brief Graph が Manager を借用しなくなってから Context を停止する
Result<void> EditorHost::shutdown_ui()
{
    if (!m_imgui)
    {
        return Result<void>::success();
    }
    auto result = m_imgui->shutdown();
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

/// @brief 保存値と実効同期状態を混同せず、下位 Host の所有 Snapshot を返す
Result<PerformanceSettingsStatus> EditorHost::performance_settings_status() const
{
    return m_windows.performance_settings_status();
}

/// @brief 保存対象と実行構成が食い違わないよう適用待ちの保存を拒否する
Result<void> EditorHost::save_performance_settings()
{
    auto settings = m_windows.performance_settings();
    if (!settings.has_value())
    {
        return Result<void>::failure(*settings.try_error());
    }
    if (m_performanceSettingsFile.empty() || m_windows.has_pending_performance_settings())
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "EditorHost.performance_settings.save"});
    }
    auto path = m_windows.storage_paths()->dataRoot.join(m_performanceSettingsFile);
    return path.has_value() ? cue::save_performance_settings(*m_windows.file_system(), *path.try_value(),
                                                             *settings.try_value())
                            : Result<void>::failure(*path.try_error());
}

/// @brief 保存値を現在の開発者 Policy に照らしてから境界で適用する
Result<void> EditorHost::load_performance_settings()
{
    auto running = m_windows.performance_settings();
    if (!running.has_value())
    {
        return Result<void>::failure(*running.try_error());
    }
    if (m_performanceSettingsFile.empty())
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "EditorHost.performance_settings.load"});
    }
    auto path = m_windows.storage_paths()->dataRoot.join(m_performanceSettingsFile);
    if (!path.has_value())
    {
        return Result<void>::failure(*path.try_error());
    }
    auto loaded = cue::load_performance_settings(*m_windows.file_system(), *path.try_value());
    if (!loaded.has_value())
    {
        return Result<void>::failure(*loaded.try_error());
    }
    auto requested = request_performance_settings(*loaded.try_value());
    if (requested.has_value())
    {
        m_settingsDraft = *loaded.try_value();
    }
    return requested;
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
    if (!m_settingsDraft)
    {
        m_settingsDraft = *applied.try_value();
    }
    ImGui::Begin("Test");
    if (ImGui::CollapsingHeader("Performance settings"))
    {
        auto &settings = *m_settingsDraft;
        auto &frame = settings.frame;
        auto &gpu = settings.gpu;
        auto &present = settings.presentation;
        const bool isPending = m_windows.has_pending_performance_settings();
        ImGui::Text("Active request: %u FPS / buffering %u", applied.try_value()->frame.maxFps,
                    applied.try_value()->gpu.resourceFrameCount);
        ImGui::Text("Derived: workers %s / CPU slots %u / update lead %u",
                    applied.try_value()->frame.useWorkerThreads ? "ON" : "OFF",
                    applied.try_value()->frame.maxFramesInFlight, update_lead_frames(applied.try_value()->frame));
        ImGui::Text("Unfinished GPU limit: %u", applied.try_value()->gpu.maxFramesInFlight);
        ImGui::TextUnformatted(isPending ? "Applying at frame boundary..." : "Ready");
        auto status = m_windows.performance_settings_status();
        if (status.has_value())
        {
            const auto &requested = status.try_value()->requested.frame;
            const auto &effective = status.try_value()->effective.frame;
            ImGui::Text("Effective: workers %s / CPU slots %u / update lead %u",
                        effective.useWorkerThreads ? "ON" : "OFF", effective.maxFramesInFlight,
                        effective.maxUpdateLeadFrames.value_or(0));
            ImGui::Text("Spin requested: %.3f us / effective: %.3f us",
                        std::chrono::duration<double, std::micro>(requested.spinDuration).count(),
                        std::chrono::duration<double, std::micro>(effective.spinDuration).count());
            ImGui::Text("Prediction slack requested: %.3f us / effective: %.3f us",
                        std::chrono::duration<double, std::micro>(requested.predictionSlack).count(),
                        std::chrono::duration<double, std::micro>(effective.predictionSlack).count());
            const auto &queued = status.try_value()->effective.presentation.maxQueuedFrames;
            if (queued)
            {
                ImGui::Text("Display queue requested: %u / effective: %u",
                            status.try_value()->requested.presentation.maxQueuedFrames, *queued);
            }
            else
            {
                ImGui::Text("Display queue requested: %u / effective: inactive (Waitable OFF)",
                            status.try_value()->requested.presentation.maxQueuedFrames);
            }
            constexpr const char *k_modes[] = {"Fixed period", "Presentation ready", "Display prediction"};
            ImGui::Text("Sync requested: %s / effective: %s",
                        k_modes[static_cast<int>(status.try_value()->requested.frame.syncMode)],
                        k_modes[static_cast<int>(status.try_value()->effective.frame.syncMode)]);
            if (status.try_value()->syncFallback != FrameSyncFallback::None)
            {
                ImGui::TextUnformatted(status.try_value()->syncFallback == FrameSyncFallback::DisplayTimingUnavailable
                                           ? "Fallback: prediction needs VSync and fresh DXGI statistics"
                                           : "Fallback: waitable presentation unavailable");
            }
            if (status.try_value()->requested.presentation.isTearingAllowed &&
                !status.try_value()->effective.presentation.isTearingAllowed)
            {
                ImGui::TextUnformatted("Tearing requested but unsupported by DXGI");
            }
        }
        ImGui::BeginDisabled(isPending || !m_performancePolicy.usersMayEdit);
        if (ImGui::Button("Balanced"))
        {
            settings = performance_preset(PerformancePreset::Balanced);
        }
        ImGui::SameLine();
        if (ImGui::Button("Low latency"))
        {
            settings = performance_preset(PerformancePreset::LowLatency);
        }
        ImGui::SameLine();
        if (ImGui::Button("Throughput"))
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
        slider("FPS limit (0: unlimited)", frame.maxFps, m_performancePolicy.allowsUnlimitedFps ? 0 : 1,
               static_cast<int>(m_performancePolicy.maxFps));
        // 一つの枠数から CPU / GPU 容量、Worker と先行数を同時に更新する
        const auto maxBuffering = (std::min)({8u, m_performancePolicy.maxCpuFrames, m_performancePolicy.maxGpuFrames,
                                              (std::min)(m_performancePolicy.maxUpdateLeadFrames, 7u) + 1});
        auto buffering = gpu.resourceFrameCount;
        ImGui::BeginDisabled(maxBuffering == 0);
        if (slider("Buffering (frames)", buffering, 1, static_cast<int>((std::max)(maxBuffering, 1u))))
        {
            auto edited = set_frame_buffering(settings, buffering);
            if (!edited.has_value())
            {
                m_settingsError = edited.try_error()->operation;
            }
        }
        ImGui::EndDisabled();
        ImGui::Text("%s buffering: workers %s / CPU slots %u / update lead %u",
                    buffering == 1   ? "Single"
                    : buffering == 2 ? "Double"
                    : buffering == 3 ? "Triple"
                                     : "Multi-frame",
                    frame.useWorkerThreads ? "ON" : "OFF", frame.maxFramesInFlight, update_lead_frames(frame));
        slider("Unfinished GPU frames", gpu.maxFramesInFlight, 1, static_cast<int>(gpu.resourceFrameCount));
        slider("SwapChain buffers", present.bufferCount, 2, static_cast<int>(m_performancePolicy.maxSwapChainBuffers));
        ImGui::Checkbox("Waitable presentation", &present.isWaitableEnabled);
        ImGui::BeginDisabled(!performance_settings_controls(settings).canEditQueuedPresents);
        slider("Queued presents", present.maxQueuedFrames, 1, static_cast<int>(m_performancePolicy.maxQueuedFrames));
        ImGui::EndDisabled();
        if (!present.isWaitableEnabled)
        {
            ImGui::TextWrapped("Queued presents requires Waitable presentation; the request is retained");
        }
        if (ImGui::Checkbox("VSync", &present.isVSyncEnabled) && present.isVSyncEnabled)
        {
            present.isTearingAllowed = false;
        }
        if (ImGui::Checkbox("Allow tearing", &present.isTearingAllowed) && present.isTearingAllowed)
        {
            present.isVSyncEnabled = false;
        }
        ImGui::BeginDisabled(!performance_settings_controls(settings).canEditWaitMode);
        int wait = static_cast<int>(frame.waitMode);
        if (ImGui::Combo("Wait method", &wait, "Sleep\0Sleep + short spin\0"))
        {
            frame.waitMode = static_cast<FrameWaitMode>(wait);
        }
        ImGui::EndDisabled();
        if (frame.maxFps == 0)
        {
            ImGui::TextWrapped("FPS wait method is inactive with an unlimited FPS request");
        }
        ImGui::BeginDisabled(!performance_settings_controls(settings).canEditSpin);
        int spin = static_cast<int>(std::chrono::duration_cast<std::chrono::microseconds>(frame.spinDuration).count());
        if (ImGui::SliderInt("Spin (us)", &spin, 0, 5000))
        {
            frame.spinDuration = std::chrono::microseconds(spin);
        }
        ImGui::EndDisabled();
        if (!performance_settings_controls(settings).canEditSpin)
        {
            ImGui::TextWrapped("Spin requires an FPS limit and Sleep + short spin; the request is retained");
        }
        else
        {
            ImGui::TextWrapped("Spin is capped at 1/8 of the FPS period in both thread modes");
        }
        int sync = static_cast<int>(frame.syncMode);
        if (ImGui::Combo("Synchronization", &sync, "Fixed period\0Presentation ready\0Display prediction\0"))
        {
            frame.syncMode = static_cast<FrameSyncMode>(sync);
        }
        ImGui::BeginDisabled(!performance_settings_controls(settings).canEditPredictionSlack);
        int slack = static_cast<int>(std::chrono::duration_cast<std::chrono::microseconds>(frame.predictionSlack).count());
        if (ImGui::SliderInt("Prediction slack (us)", &slack, 0, 100000))
        {
            frame.predictionSlack = std::chrono::microseconds(slack);
        }
        ImGui::EndDisabled();
        if (!performance_settings_controls(settings).canEditPredictionSlack)
        {
            ImGui::TextWrapped(
                "Prediction slack requires Display prediction, Waitable and VSync; the request is retained");
        }
        if (ImGui::Button("Apply"))
        {
            auto requested = request_performance_settings(settings);
            m_settingsError = requested.has_value() ? std::string{} : requested.try_error()->operation;
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(m_performanceSettingsFile.empty());
        if (ImGui::Button("Apply and save"))
        {
            auto requested = request_performance_settings(settings);
            m_savesAfterApply = requested.has_value();
            m_settingsError = requested.has_value() ? std::string{} : requested.try_error()->operation;
        }
        ImGui::SameLine();
        if (ImGui::Button("Load saved"))
        {
            auto loaded = load_performance_settings();
            m_settingsError = loaded.has_value() ? std::string{} : loaded.try_error()->operation;
        }
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        if (!m_settingsError.empty())
        {
            ImGui::Text("Settings error: %s", m_settingsError.c_str());
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
