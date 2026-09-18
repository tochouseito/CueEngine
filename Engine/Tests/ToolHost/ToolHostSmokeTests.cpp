#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Fatal.h>
#include <Cue/Foundation/Log.h>
#include <Cue/ToolHost/WindowsD3D12/ToolHost.h>

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include <imgui.h>

namespace
{
/// @brief Smoke Test中の回復不能状態を固定Exit Codeへ変換する
class TestFatalHandler final : public cue::FatalHandler
{
  public:
    /// @brief MessageなしFatalを固定Exit Codeへ変換する
    [[noreturn]] void terminate() noexcept override
    {
        std::_Exit(75);
    }

    /// @brief Message付きFatalを固定Exit Codeへ変換する
    [[noreturn]] void terminate(std::string_view) noexcept override
    {
        std::_Exit(76);
    }
};

/// @brief Tool HostへResizeと非表示を含む最小Offscreen Surface要求を3 Frame提供する
class SmokeClient final : public cue::tool_host::ToolHostClient
{
  public:
    /// @brief Smoke Client Stateの複製を禁止する
    SmokeClient(const SmokeClient &) = delete;
    /// @brief Smoke Client Stateの複製を禁止する
    SmokeClient &operator=(const SmokeClient &) = delete;
    /// @brief 描画可能なCameraとCube Snapshotを所有してFrame Counterを0から開始する
    explicit SmokeClient(cue::EmergencyHandler &a_emergencyHandler) noexcept
    {
        cue::Result<cue::renderer::DebugCamera> debugCamera =
            cue::renderer::DebugCamera::create_default(a_emergencyHandler);
        if (!debugCamera)
        {
            std::_Exit(77);
        }
        m_debugCamera.emplace(std::move(*debugCamera.try_value()));
        std::vector<cue::renderer::RenderMeshInstance> meshes{
            {cue::math::Transform{}, cue::renderer::RenderMesh::Cube}};
        m_snapshot = cue::renderer::RenderSnapshot(cue::renderer::MainCameraStatus::Ready, m_debugCamera->camera(),
                                                   std::move(meshes), 1U);
    }
    /// @brief 所有Resourceを持たないSmoke Clientを破棄する
    ~SmokeClient() override = default;

    /// @brief Host Backendへ送る最小ImGui Windowを構築する
    void draw_frame() noexcept override
    {
        ImGui::Begin("CueEngine Tool Host Smoke");
        ImGui::TextUnformatted("ImGui / Win32 / D3D12");
        if (m_surfaces[0U].textureId != 0U)
        {
            ImGui::Image(ImTextureRef(static_cast<ImTextureID>(m_surfaces[0U].textureId)), ImVec2(160.0F, 90.0F));
        }
        if (m_surfaces[1U].textureId != 0U)
        {
            ImGui::Image(ImTextureRef(static_cast<ImTextureID>(m_surfaces[1U].textureId)), ImVec2(160.0F, 90.0F));
        }
        ImGui::End();
        ++m_drawCount;
        if (m_drawCount == 1U)
        {
            m_requests[0U] = {256U, 144U, true};
            m_requests[1U] = {300U, 180U, true};
        }
        else if (m_drawCount == 2U)
        {
            m_requests = {};
        }
    }

    /// @brief 初期化済みWindowが最初のFrame前に通知されたことを記録する
    void window_ready(cue::Window &) noexcept override
    {
        m_windowWasReady = m_drawCount == 0U;
    }

    /// @brief 現在FrameのSurface要求を返す
    [[nodiscard]] cue::tool_host::ToolHostRenderSurfaceRequests render_surface_requests() const noexcept override
    {
        return m_requests;
    }

    /// @brief Hostが要求通りのSurface世代または非表示Viewを通知したことを検証する
    void render_surfaces_ready(cue::tool_host::ToolHostRenderSurfaceViews a_surfaces) noexcept override
    {
        m_surfaces = a_surfaces;
        if (m_drawCount == 0U)
        {
            m_surfaceWasValid = a_surfaces[0U].textureId != 0U && a_surfaces[0U].width == 320U &&
                                a_surfaces[0U].height == 180U && a_surfaces[1U].textureId != 0U &&
                                a_surfaces[1U].width == 200U && a_surfaces[1U].height == 120U;
        }
        else if (m_drawCount == 1U)
        {
            m_resizeWasValid = a_surfaces[0U].textureId != 0U && a_surfaces[0U].width == 256U &&
                               a_surfaces[0U].height == 144U && a_surfaces[1U].textureId != 0U &&
                               a_surfaces[1U].width == 300U && a_surfaces[1U].height == 180U;
        }
        else if (m_drawCount == 2U)
        {
            m_hiddenWasValid = a_surfaces[0U].textureId == 0U && a_surfaces[0U].width == 0U &&
                               a_surfaces[0U].height == 0U && a_surfaces[1U].textureId == 0U &&
                               a_surfaces[1U].width == 0U && a_surfaces[1U].height == 0U;
        }
    }

    /// @brief GameViewとDebugViewへ同じCube Snapshotを異なるCameraで描画させる
    [[nodiscard]] cue::tool_host::ToolHostRenderFrameView render_frame_view() const noexcept override
    {
        return {&m_snapshot, &m_debugCamera->camera(), &m_debugCamera->camera()};
    }

    /// @brief 有限Frame Smokeでは予期しないNative Window終了要求を状態へ反映しない
    void request_close() noexcept override
    {
    }

    /// @brief 最大Frame条件だけで終了するためClient起因の終了を要求しない
    [[nodiscard]] bool should_close() const noexcept override
    {
        return false;
    }

    /// @brief HostがClientを描画したFrame数を返す
    [[nodiscard]] std::uint32_t draw_count() const noexcept
    {
        return m_drawCount;
    }

    /// @brief Window通知が最初の描画より前に届いたか返す
    [[nodiscard]] bool window_was_ready() const noexcept
    {
        return m_windowWasReady;
    }

    /// @brief Surface生成、Resize、非表示の全観測が成功したか返す
    [[nodiscard]] bool surface_lifecycle_was_valid() const noexcept
    {
        return m_surfaceWasValid && m_resizeWasValid && m_hiddenWasValid;
    }

  private:
    cue::tool_host::ToolHostRenderSurfaceRequests m_requests{{{320U, 180U, true}, {200U, 120U, true}}};
    cue::tool_host::ToolHostRenderSurfaceViews m_surfaces;
    std::optional<cue::renderer::DebugCamera> m_debugCamera;
    cue::renderer::RenderSnapshot m_snapshot;
    std::uint32_t m_drawCount = 0;
    bool m_windowWasReady = false;
    bool m_surfaceWasValid = false;
    bool m_resizeWasValid = false;
    bool m_hiddenWasValid = false;
};

/// @brief 指定Adapter方針でTool Host Surface Lifecycleを実Frame検証する
[[nodiscard]] int run_smoke(cue::tool_host::ToolHostAdapterPreference a_preference,
                            const cue::AssertContext &a_context) noexcept
{
    SmokeClient client(a_context.fatal_handler());
    const cue::tool_host::ToolHostDescriptor descriptor{"Cue Tool Host Smoke", {640U, 360U}, 3U, 0U, a_preference};
    cue::Result<void> result = cue::tool_host::run_windows_d3d12_tool_host(descriptor, client, a_context);
    if (!result)
    {
        return 1;
    }
    if (!client.window_was_ready())
    {
        return 2;
    }
    if (client.draw_count() != 3U)
    {
        return 3;
    }
    return client.surface_lifecycle_was_valid() ? 0 : 4;
}
} // namespace

/// @brief Win32 Window、D3D12、ImGui Backend、有限Fence Drainを実Frameで検証する
int main(int a_argumentCount, char **a_arguments)
{
    TestFatalHandler handler;
    std::vector<std::unique_ptr<cue::LogSink>> sinks;
    cue::Logger logger(handler, std::move(sinks));
    cue::AssertContext context(logger, handler);
    const bool useWarp = a_argumentCount == 2 && std::string_view(a_arguments[1]) == "--warp";
    return run_smoke(useWarp ? cue::tool_host::ToolHostAdapterPreference::Warp
                             : cue::tool_host::ToolHostAdapterPreference::HardwarePreferred,
                     context);
}
