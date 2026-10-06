#include <EditorHost/ImGuiManager.h>

#include <cmath>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <new>
#include <optional>
#include <utility>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_internal.h>

#include <Platform/Diagnostics.h>
#include <Platform/Windows/WindowsPlatform.h>

#include "DX12ImGuiBackend.h"

// 公式 Header の指示に従い、Win32 型を公開 Header に漏らさず実装側で宣言する
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace cue
{
namespace
{
/// @brief API 呼出中だけ対象 Context を Current にし、呼出側の Context を復元する
class ScopedContext final
{
public:
    /// @brief 現在の Context を非所有で退避する
    explicit ScopedContext(ImGuiContext* a_context) noexcept : m_previous(ImGui::GetCurrentContext())
    {
        ImGui::SetCurrentContext(a_context);
    }

    /// @brief 借用期間の終わりに元の Context を Current に戻す
    ~ScopedContext()
    {
        ImGui::SetCurrentContext(m_previous);
    }

private:
    ImGuiContext* m_previous = nullptr;
};
} // namespace

class ImGuiManager::State final
{
public:
    ImGuiContext* context = nullptr;
    Window* window = nullptr;
    WindowsMessageHandlerToken handler;
    ImGuiManagerConfig config;
    ImGuiFrameInfo info;
    ImGuiContext* previousFrameContext = nullptr;
    ImGuiErrorRecoveryState recovery;
    std::unique_ptr<DX12ImGuiBackend> renderer;
    std::uint64_t lastRecordedFrame = 0;
    bool isWin32Initialized = false;
    bool isReady = false;
    bool isFrameOpen = false;
    bool isBuilding = false;
    bool isCpuAtlasBuilt = false;
};

/// @brief 全 Context 操作を作成した Thread に固定する
ImGuiManager::ImGuiManager(CreateToken) noexcept : m_ownerId(std::this_thread::get_id())
{
}

/// @brief Window を借用して CPU UI 基盤だけを構築する
Result<std::unique_ptr<ImGuiManager>> ImGuiManager::create(Window& a_window, ImGuiManagerConfig a_config)
{
    using ManagerResult = Result<std::unique_ptr<ImGuiManager>>;
    if (!std::isfinite(a_config.fontSize) || a_config.fontSize <= 0.0f || a_config.fontSize > 256.0f ||
        a_config.settingsFile.find('\0') != std::string::npos)
    {
        return ManagerResult::failure({ErrorCategory::InvalidArgument, "ImGuiManager.config"});
    }
    auto handle = borrow_windows_window_handle(a_window);
    if (!handle.has_value())
    {
        return ManagerResult::failure(*handle.try_error());
    }
    try
    {
        auto result = std::make_unique<ImGuiManager>(CreateToken{});
        result->m_state = std::make_unique<State>();
        auto& state = *result->m_state;
        state.window = &a_window;
        state.config = std::move(a_config);

        // CreateContext が Current を変更する場合も、生成処理の外へ漏らさない
        IMGUI_CHECKVERSION();
        auto* previous = ImGui::GetCurrentContext();
        state.context = ImGui::CreateContext();
        ImGui::SetCurrentContext(previous);
        if (!state.context)
        {
            return ManagerResult::failure({ErrorCategory::PlatformFailure, "ImGui.CreateContext"});
        }
        ScopedContext current(state.context);
        auto& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        if (state.config.isDockingEnabled)
        {
            io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
        }
        // Multi-Viewport は Window / GPU 資源の寿命が別途必要なため初期構成では有効にしない
        // 自動 File I/O を使わず、保存失敗を Result で返せるよう保存先を Manager が所有する
        io.IniFilename = nullptr;
        ImGui::StyleColorsDark();
        ImGui::GetStyle().FrameRounding = 4.0f;
        ImFontConfig font;
        font.SizePixels = state.config.fontSize;
        if (!io.Fonts->AddFontDefault(&font))
        {
            return ManagerResult::failure({ErrorCategory::PlatformFailure, "ImGuiManager.fonts"});
        }

        // 初回は Layout が存在しなくてもよい。既存 File の読込み失敗は初期化失敗にする
        if (!state.config.settingsFile.empty())
        {
            const auto path = std::filesystem::u8path(state.config.settingsFile);
            if (std::filesystem::exists(path))
            {
                std::ifstream input(path, std::ios::binary);
                if (!input)
                {
                    return ManagerResult::failure({ErrorCategory::PlatformFailure, "ImGuiManager.load_settings"});
                }
                const std::string settings(std::istreambuf_iterator<char>(input), {});
                if (input.bad())
                {
                    return ManagerResult::failure({ErrorCategory::PlatformFailure, "ImGuiManager.load_settings"});
                }
                ImGui::LoadIniSettingsFromMemory(settings.data(), settings.size());
            }
        }

        // 接続枠を先に確保し、二重生成失敗が既存 Backend の Window Property を変更しないようにする
        State* borrowed = &state;
        auto registered = register_windows_message_handler(
            a_window,
            [borrowed](const WindowsMessage& a_message)
            {
                if (!borrowed->isWin32Initialized)
                {
                    return WindowsMessageResult{};
                }
                // 他の Context が Current の場合も、この Window の Context へ入力を配送する
                ScopedContext active(borrowed->context);
                const auto value = ImGui_ImplWin32_WndProcHandler(static_cast<HWND>(a_message.window),
                                                                a_message.message, a_message.wParam, a_message.lParam);
                // Capture Flag は上位の Gameplay 入力に使う。Win32 の必須処理は Platform が維持する
                return WindowsMessageResult{value != 0, static_cast<std::intptr_t>(value)};
            });
        if (!registered.has_value())
        {
            return ManagerResult::failure(*registered.try_error());
        }
        state.handler = registered.take_value();
        if (!ImGui_ImplWin32_Init(handle.take_value()))
        {
            return ManagerResult::failure({ErrorCategory::PlatformFailure, "ImGui_ImplWin32_Init"});
        }
        state.isWin32Initialized = true;
        state.isReady = true;
        return ManagerResult::success(std::move(result));
    }
    catch (const std::bad_alloc&)
    {
        return ManagerResult::failure({ErrorCategory::PlatformFailure, "ImGuiManager.create.allocation"});
    }
    catch (const std::filesystem::filesystem_error& error)
    {
        return ManagerResult::failure({ErrorCategory::PlatformFailure, "ImGuiManager.settings", error.code().value()});
    }
}

/// @brief Context より先に入力接続を解除し、解除できない破棄を続けない
ImGuiManager::~ImGuiManager()
{
    auto result = shutdown();
    if (!result.has_value())
    {
        report_error("ImGuiManager cleanup", *result.try_error(), DiagnosticSeverity::Error);
        if (m_state && m_state->context)
        {
            std::terminate();
        }
    }
}

/// @brief ImGui を触る前に Thread と Lifecycle を検証する
Result<void> ImGuiManager::validate(const char* a_operation) const
{
    if (std::this_thread::get_id() != m_ownerId)
    {
        return Result<void>::failure({ErrorCategory::WrongThread, a_operation});
    }
    if (!m_state || !m_state->isReady)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, a_operation});
    }
    return Result<void>::success();
}

/// @brief CPU 基盤の生成後に Renderer を接続し、途中失敗でも Context を維持する
Result<void> ImGuiManager::initialize_renderer(IBackend &a_backend, std::uint32_t a_frameCount)
{
    auto valid = validate("ImGuiManager.initialize_renderer");
    if (!valid.has_value())
    {
        return valid;
    }
    if (m_state->renderer || m_state->isFrameOpen || m_state->isCpuAtlasBuilt)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "ImGuiManager.initialize_renderer"});
    }
    auto created = DX12ImGuiBackend::create(a_backend, *m_state->context, a_frameCount,
                                            m_state->config.rendererDescriptorCapacity);
    if (!created.has_value())
    {
        return Result<void>::failure(*created.try_error());
    }
    m_state->renderer = created.take_value();
    return Result<void>::success();
}

/// @brief 直前に確定した Frame を一度だけ記録し、Draw Data の借用を Frame 内に限定する
Result<void> ImGuiManager::record_draw_data(FrameGraphContext &a_context)
{
    auto valid = validate("ImGuiManager.record_draw_data");
    if (!valid.has_value())
    {
        return valid;
    }
    if (!m_state->renderer || m_state->isFrameOpen || m_state->info.frames == 0 ||
        m_state->lastRecordedFrame == m_state->info.frames)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "ImGuiManager.record_draw_data"});
    }
    ScopedContext current(m_state->context);
    auto *draw = ImGui::GetDrawData();
    if (!draw || !draw->Valid)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "ImGuiManager.draw_data"});
    }
    auto recorded = m_state->renderer->record(*draw, a_context);
    if (recorded.has_value())
    {
        m_state->lastRecordedFrame = m_state->info.frames;
    }
    return recorded;
}

/// @brief 生成済み GPU Backend の所有状態だけを返す
Result<ImGuiRendererInfo> ImGuiManager::renderer_info() const
{
    auto valid = validate("ImGuiManager.renderer_info");
    if (!valid.has_value())
    {
        return Result<ImGuiRendererInfo>::failure(*valid.try_error());
    }
    return m_state->renderer
               ? Result<ImGuiRendererInfo>::success(m_state->renderer->info())
               : Result<ImGuiRendererInfo>::failure({ErrorCategory::InvalidState, "ImGuiManager.renderer_info"});
}

/// @brief Message Pump 後の採用 Frame で Platform 入力を取り込み、UI の構築を開始する
Result<void> ImGuiManager::begin_frame()
{
    auto valid = validate("ImGuiManager.begin_frame");
    if (!valid.has_value())
    {
        return valid;
    }
    if (m_state->isFrameOpen || m_state->isBuilding)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "ImGuiManager.begin_frame"});
    }
    if (m_state->renderer)
    {
        auto renderer = m_state->renderer->new_frame();
        if (!renderer.has_value())
        {
            return renderer;
        }
    }
    m_state->previousFrameContext = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(m_state->context);
    // GPU 接続前に Legacy Atlas を焼かず、Renderer 未接続の CPU 利用だけ初回に生成する
    // 動的 Texture 対応 Backend は NewFrame 内の公式 Atlas 更新へ任せる
    if (!m_state->renderer && !m_state->isCpuAtlasBuilt)
    {
        if (!ImGui::GetIO().Fonts->Build())
        {
            ImGui::SetCurrentContext(m_state->previousFrameContext);
            m_state->previousFrameContext = nullptr;
            return Result<void>::failure({ErrorCategory::PlatformFailure, "ImGuiManager.fonts"});
        }
        m_state->isCpuAtlasBuilt = true;
    }
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    // 固定 Version が提供する回復 API で Callback 前の Begin / Style / Table Stack を保存する
    ImGui::ErrorRecoveryStoreState(&m_state->recovery);
    m_state->isFrameOpen = true;
    return Result<void>::success();
}

/// @brief Renderer に依存せず Draw Data を確定し、呼出元の Context を復元する
Result<void> ImGuiManager::end_frame()
{
    auto valid = validate("ImGuiManager.end_frame");
    if (!valid.has_value())
    {
        return valid;
    }
    if (!m_state->isFrameOpen || m_state->isBuilding || ImGui::GetCurrentContext() != m_state->context)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "ImGuiManager.end_frame"});
    }
    ImGui::Render();
    const auto* draw = ImGui::GetDrawData();
    const auto& io = ImGui::GetIO();
    ++m_state->info.frames;
    m_state->info.vertexCount = static_cast<std::uint32_t>(draw->TotalVtxCount);
    m_state->info.indexCount = static_cast<std::uint32_t>(draw->TotalIdxCount);
    m_state->info.wantsMouse = io.WantCaptureMouse;
    m_state->info.wantsKeyboard = io.WantCaptureKeyboard;
    m_state->info.wantsTextInput = io.WantTextInput;
    m_state->isFrameOpen = false;
    ImGui::SetCurrentContext(m_state->previousFrameContext);
    m_state->previousFrameContext = nullptr;
    return io.WantSaveIniSettings ? save_settings() : Result<void>::success();
}

/// @brief UI Callback をこの Context の開いた Frame に限定して実行する
Result<void> ImGuiManager::build_frame(const editorUiCallback& a_buildUi)
{
    auto started = begin_frame();
    if (!started.has_value())
    {
        return started;
    }
    m_state->isBuilding = true;
    Result<void> result = Result<void>::success();
    try
    {
        if (a_buildUi)
        {
            result = a_buildUi();
        }
    }
    catch (...)
    {
        result = Result<void>::failure({ErrorCategory::PlatformFailure, "ImGuiManager.ui_callback"});
    }
    m_state->isBuilding = false;
    if (result.has_value() && ImGui::GetCurrentContext() != m_state->context)
    {
        result = Result<void>::failure({ErrorCategory::InvalidState, "ImGuiManager.ui_context"});
    }
    if (!result.has_value())
    {
        cancel_frame();
        return result;
    }
    return end_frame();
}

/// @brief 中断した Frame を確定せず回収し、開始前の Context を復元する
void ImGuiManager::cancel_frame() noexcept
{
    if (m_state->isFrameOpen)
    {
        ImGui::SetCurrentContext(m_state->context);
        // Callback 失敗は Result で通知し、残された Stack の回収中だけ Assert を抑制する
        // 通常 UI 構築時の Assert 設定と ImGui の回復診断は維持する
        auto& io = ImGui::GetIO();
        const bool wasAssertEnabled = io.ConfigErrorRecoveryEnableAssert;
        io.ConfigErrorRecoveryEnableAssert = false;
        ImGui::ErrorRecoveryTryToRecoverState(&m_state->recovery);
        ImGui::EndFrame();
        io.ConfigErrorRecoveryEnableAssert = wasAssertEnabled;
        m_state->isFrameOpen = false;
        ImGui::SetCurrentContext(m_state->previousFrameContext);
        m_state->previousFrameContext = nullptr;
    }
}

/// @brief Context の内部 Pointer を公開せず Frame と Capture の Snapshot を返す
Result<ImGuiFrameInfo> ImGuiManager::frame_info() const
{
    auto valid = validate("ImGuiManager.frame_info");
    return valid.has_value() ? Result<ImGuiFrameInfo>::success(m_state->info)
                             : Result<ImGuiFrameInfo>::failure(*valid.try_error());
}

/// @brief Memory の Layout を一時 File へ書き、書込み成功後に保存先を置き換える
Result<void> ImGuiManager::save_settings()
{
    auto valid = validate("ImGuiManager.save_settings");
    if (!valid.has_value())
    {
        return valid;
    }
    if (m_state->config.settingsFile.empty())
    {
        return Result<void>::success();
    }
    ScopedContext current(m_state->context);
    try
    {
        const auto path = std::filesystem::u8path(m_state->config.settingsFile);
        if (path.has_parent_path())
        {
            std::filesystem::create_directories(path.parent_path());
        }
        auto temporary = path;
        temporary += L".tmp";
        std::size_t size = 0;
        const auto* data = ImGui::SaveIniSettingsToMemory(&size);
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output.write(data, static_cast<std::streamsize>(size));
        output.close();
        if (!output)
        {
            return Result<void>::failure({ErrorCategory::PlatformFailure, "ImGuiManager.save_settings.write"});
        }
        std::filesystem::rename(temporary, path);
        ImGui::GetIO().WantSaveIniSettings = false;
        return Result<void>::success();
    }
    catch (const std::filesystem::filesystem_error& error)
    {
        return Result<void>::failure({ErrorCategory::PlatformFailure, "ImGuiManager.save_settings", error.code().value()});
    }
    catch (const std::bad_alloc&)
    {
        return Result<void>::failure({ErrorCategory::PlatformFailure, "ImGuiManager.save_settings.allocation"});
    }
}

/// @brief Context を参照する Handler を先に失効させ、途中生成の Backend も回収する
Result<void> ImGuiManager::shutdown()
{
    if (std::this_thread::get_id() != m_ownerId)
    {
        return Result<void>::failure({ErrorCategory::WrongThread, "ImGuiManager.shutdown"});
    }
    if (!m_state || !m_state->context)
    {
        return Result<void>::success();
    }
    if (m_state->isBuilding)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "ImGuiManager.shutdown"});
    }
    // GPU 完了の証明に失敗した場合は Context、Heap と下位 Backend の借用を保全する
    if (m_state->renderer)
    {
        auto stopped = m_state->renderer->shutdown();
        if (!stopped.has_value())
        {
            return stopped;
        }
        m_state->renderer.reset();
    }
    if (m_state->handler.generation != 0)
    {
        auto removed = unregister_windows_message_handler(*m_state->window, m_state->handler);
        if (!removed.has_value())
        {
            return removed;
        }
        m_state->handler = {};
    }
    cancel_frame();
    // 保存失敗は通知するが、登録解除済みの Context と Win32 Backend の解放は続ける
    auto saved = m_state->isReady ? save_settings() : Result<void>::success();
    auto* previous = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(m_state->context);
    if (m_state->isWin32Initialized)
    {
        ImGui_ImplWin32_Shutdown();
        m_state->isWin32Initialized = false;
    }
    ImGui::DestroyContext(m_state->context);
    ImGui::SetCurrentContext(previous == m_state->context ? nullptr : previous);
    m_state->context = nullptr;
    m_state->window = nullptr;
    m_state->isReady = false;
    return saved;
}
} // namespace cue
