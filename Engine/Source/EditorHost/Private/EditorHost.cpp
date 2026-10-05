#include <EditorHost/EditorHost.h>

#include <utility>

#include <Platform/Diagnostics.h>

namespace cue
{
namespace
{
/// @brief 例外時も Step の再入検出状態を解除する
class ScopedStep final
{
public:
    /// @brief 非所有の実行中 Flag を Scope の間だけ立てる
    explicit ScopedStep(bool& a_flag) noexcept : m_flag(a_flag)
    {
        m_flag = true;
    }

    /// @brief Step の終了を記録する
    ~ScopedStep()
    {
        m_flag = false;
    }

private:
    bool& m_flag;
};
} // namespace

/// @brief Editor の表示設定を保持し、下位基盤へ Editor 型を渡さない
EditorHost::EditorHost(EditorHostConfig a_config)
    : m_imguiConfig(std::move(a_config.imgui)), m_buildUi(std::move(a_config.buildUi)),
      m_ownerId(std::this_thread::get_id()), m_useWorkerThreads(a_config.frame.useWorkerThreads),
      m_windows({std::move(a_config.window), a_config.frame, a_config.presentation, std::move(a_config.graph),
                 {[this](Window& a_window) { return initialize_ui(a_window); },
                  [this](std::uint64_t a_frame, std::stop_token a_token) { return update_ui(a_frame, a_token); },
                  [this]() { return shutdown_ui(); }}})
{
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
Result<void> EditorHost::initialize_ui(Window& a_window)
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
Result<void> EditorHost::update_ui(std::uint64_t, std::stop_token a_stopToken)
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
