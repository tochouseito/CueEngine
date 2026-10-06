#include <EditorHost/EditorHost.h>

#include <utility>

#include <imgui.h>

#include <EditorHost/ImGuiPass.h>
#include <Platform/Diagnostics.h>

namespace cue
{
namespace
{
/// @brief Context が Current の UI Frame 内で既定の Test Window だけを構築する
[[nodiscard]] Result<void> build_test_window()
{
    // 初回の位置と寸法だけ指定し、以後の移動と Layout 保存を妨げない
    ImGui::SetNextWindowPos({32.0f, 32.0f}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize({240.0f, 120.0f}, ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Test"))
    {
        ImGui::Text("TEST");
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

/// @brief 例外時も Step の再入検出状態を解除する
class ScopedStep final
{
  public:
    /// @brief 非所有の実行中 Flag を Scope の間だけ立てる
    explicit ScopedStep(bool &a_flag) noexcept : m_flag(a_flag)
    {
        m_flag = true;
    }

    /// @brief Step の終了を記録する
    ~ScopedStep()
    {
        m_flag = false;
    }

  private:
    bool &m_flag;
};
} // namespace

/// @brief Editor の表示設定を保持し、下位基盤へ Editor 型を渡さない
EditorHost::EditorHost(EditorHostConfig a_config)
    : m_imguiConfig(std::move(a_config.imgui)),
      m_buildUi(a_config.buildUi ? std::move(a_config.buildUi) : editorUiCallback{build_test_window}),
      m_ownerId(std::this_thread::get_id()), m_useWorkerThreads(a_config.frame.useWorkerThreads),
      m_windows({std::move(a_config.window),
                 a_config.frame,
                 a_config.presentation,
                 prepare_graph(std::move(a_config.graph)),
                 {[this](Window &a_window) { return initialize_ui(a_window); },
                  {},
                  [this]() { return shutdown_ui(); },
                  [this](IBackend &a_backend, std::uint32_t a_frames)
                  { return m_imgui->initialize_renderer(a_backend, a_frames); },
                  [this](std::uint64_t a_frame, std::stop_token a_token) { return build_ui(a_frame, a_token); }}})
{
}

/// @brief Editor の表示を抽象 Pass として下位 Host へ渡し、明示された Pass は優先する
MainFrameGraphConfig EditorHost::prepare_graph(MainFrameGraphConfig a_config)
{
    if (!a_config.displayPass)
    {
        a_config.displayPass = std::make_unique<ImGuiPass>(std::make_unique<EditorImGuiRenderer>(m_imgui));
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
    ScopedStep stepping(m_isStepping);
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

/// @brief Worker への Context 共有を拒否し、Window の生成後に UI の Owner を作る
Result<void> EditorHost::initialize_ui(Window &a_window)
{
    if (m_useWorkerThreads)
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "EditorHost.ui_owner_thread"});
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
Result<void> EditorHost::build_ui(std::uint64_t, std::stop_token a_stopToken)
{
    if (a_stopToken.stop_requested())
    {
        return Result<void>::success();
    }
    return m_imgui->build_frame(m_buildUi);
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
