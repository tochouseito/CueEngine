#include <EditorHost/EditorHost.h>

#include <chrono>
#include <new>
#include <utility>

#include <imgui.h>

#include <EditorHost/ImGuiPass.h>
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
        ImGui::Text("%s: avg %.3f / p95 %.3f / max %.3f ms", a_name,
                    std::chrono::duration<double, std::milli>(a_statistics.averageDuration).count(),
                    std::chrono::duration<double, std::milli>(a_statistics.p95Duration).count(),
                    std::chrono::duration<double, std::milli>(a_statistics.maxDuration).count());
    }
    else
    {
        ImGui::Text("%s: --", a_name);
    }
}

/// @brief Main が取得した計測 Snapshot を既定 Test Window に表示する
[[nodiscard]] Result<void> build_test_window(const FrameProgress &a_progress, const FrameTimingInfo &a_frameTiming,
                                             const ImGuiTimingInfo &a_uiTiming,
                                             const MainFrameGraphPerformance &a_graphPerformance)
{
    // 初回の位置と寸法だけ指定し、以後の移動と Layout 保存を妨げない
    ImGui::SetNextWindowPos({32.0f, 32.0f}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize({640.0f, 480.0f}, ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Test"))
    {
        ImGui::Text("TEST");
        // Render Callback と FPS 上限待機を含む完了間隔を使い、ImGui の UI 構築頻度と区別する
        const auto interval = std::chrono::duration<double>(a_progress.lastFrameInterval).count();
        if (a_progress.renderedFrames >= 2 && interval > 0.0)
        {
            ImGui::Text("FrameController FPS: %.1f", 1.0 / interval);
        }
        else
        {
            // 二つの Render 完了点が揃うまでは FPS を計算せず、起動直後の零除算を避ける
            ImGui::Text("FrameController FPS: --");
        }
        // GPU 時間と混同しないよう CPU の経過時間であることと集計枠数を表示する
        ImGui::Text("CPU timings (last 120 samples)");
        show_timing("Main", a_frameTiming.main);
        show_timing("Update", a_frameTiming.update);
        show_timing("Render", a_frameTiming.render);
        show_timing("FPS wait", a_frameTiming.limitWait);
        show_timing("UI build", a_uiTiming.uiBuild);
        show_timing("Snapshot copy", a_uiTiming.snapshotCopy);
        show_timing("ImGui lock wait", a_uiTiming.contextWait);
        show_timing("ImGui GPU fence wait", a_uiTiming.gpuWait);
        show_timing("Graph record", a_graphPerformance.record);
        show_timing("Graph frame fence wait", a_graphPerformance.frameWait);
        show_timing("Present", a_graphPerformance.present);
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
                                                        // UI の Owner Thread から同期済み Snapshot を取得し、Worker
                                                        // の可変状態を直接参照しない
                                                        auto progress = m_windows.frame_progress();
                                                        if (!progress.has_value())
                                                        {
                                                            return Result<void>::failure(*progress.try_error());
                                                        }
                                                        return build_test_window(*progress.try_value(), m_frameTiming,
                                                                                 m_uiTiming, m_graphPerformance);
                                                    }}),
      m_ownerId(std::this_thread::get_id()),
      m_windows({std::move(a_config.window),
                 a_config.frame,
                 a_config.presentation,
                 prepare_graph(std::move(a_config.graph)),
                 {[this](Window &a_window) { return initialize_ui(a_window); },
                  {},
                  [this]() { return shutdown_ui(); },
                  [this](IBackend &a_backend, std::uint32_t a_frames)
                  {
                      auto result = m_imgui->initialize_renderer(a_backend, a_frames);
                      return result.has_value() ? m_imgui->enable_frame_transfer() : std::move(result);
                  },
                  [this](std::uint64_t a_frame, std::stop_token a_token) { return build_ui(a_frame, a_token); },
                  [this](std::uint64_t a_frame, std::stop_token a_token, const FrameCallback &a_record)
                  { return m_imgui->render_frame(a_frame, a_token, a_record); }}})
{
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
        report_error("CueEditorHost cleanup", *result.try_error(), DiagnosticSeverity::Error);
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

/// @brief Window の生成 Thread に UI の Owner を固定する
Result<void> EditorHost::initialize_ui(Window &a_window)
{
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
    if (a_stopToken.stop_requested())
    {
        return Result<void>::success();
    }
    // Render は Graph Lock から ImGui Context を借りるため、逆順で同時に保持しない
    // 計測 Snapshot を UI 開始前に取得し、Callback 内では所有値だけを表示する
    auto graphPerformance = m_windows.graph_performance();
    auto frameTiming = m_windows.frame_timing_info();
    auto uiTiming = m_imgui->timing_info();
    if (!graphPerformance.has_value() || !frameTiming.has_value() || !uiTiming.has_value())
    {
        return Result<void>::failure(!graphPerformance.has_value() ? *graphPerformance.try_error()
                                     : !frameTiming.has_value()    ? *frameTiming.try_error()
                                                                   : *uiTiming.try_error());
    }
    m_graphPerformance = graphPerformance.take_value();
    m_frameTiming = frameTiming.take_value();
    m_uiTiming = uiTiming.take_value();
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
} // namespace cue
