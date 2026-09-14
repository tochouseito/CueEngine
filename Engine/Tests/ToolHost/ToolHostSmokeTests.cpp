#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Fatal.h>
#include <Cue/Foundation/Log.h>
#include <Cue/ToolHost/WindowsD3D12/ToolHost.h>

#include <cstdint>
#include <cstdlib>
#include <memory>
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

/// @brief Tool Hostへ最小ImGui Draw Dataを2 Frame提供する
class SmokeClient final : public cue::tool_host::ToolHostClient
{
  public:
    /// @brief Smoke Client Stateの複製を禁止する
    SmokeClient(const SmokeClient &) = delete;
    /// @brief Smoke Client Stateの複製を禁止する
    SmokeClient &operator=(const SmokeClient &) = delete;
    /// @brief Frame Counterを0から開始する
    SmokeClient() noexcept = default;
    /// @brief 所有Resourceを持たないSmoke Clientを破棄する
    ~SmokeClient() override = default;

    /// @brief Host Backendへ送る最小ImGui Windowを構築する
    void draw_frame() noexcept override
    {
        ImGui::Begin("CueEngine Tool Host Smoke");
        ImGui::TextUnformatted("ImGui / Win32 / D3D12");
        ImGui::End();
        ++m_drawCount;
    }

    /// @brief 初期化済みWindowが最初のFrame前に通知されたことを記録する
    void window_ready(cue::Window &) noexcept override
    {
        m_windowWasReady = m_drawCount == 0U;
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

  private:
    std::uint32_t m_drawCount = 0;
    bool m_windowWasReady = false;
};
} // namespace

/// @brief Win32 Window、D3D12、ImGui Backend、有限Fence Drainを実Frameで検証する
int main()
{
    TestFatalHandler handler;
    std::vector<std::unique_ptr<cue::LogSink>> sinks;
    cue::Logger logger(handler, std::move(sinks));
    cue::AssertContext context(logger, handler);
    SmokeClient client;
    const cue::tool_host::ToolHostDescriptor descriptor{"Cue Tool Host Smoke", {640U, 360U}, 2U};
    cue::Result<void> result = cue::tool_host::run_windows_d3d12_tool_host(descriptor, client, context);
    return result && client.window_was_ready() && client.draw_count() == 2U ? 0 : 1;
}
