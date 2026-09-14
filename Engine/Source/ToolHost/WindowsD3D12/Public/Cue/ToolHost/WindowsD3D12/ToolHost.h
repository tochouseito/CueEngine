#pragma once

#include <Cue/Foundation/Result.h>
#include <Cue/Platform/WindowEvent.h>

#include <cstdint>
#include <string_view>

namespace cue
{
class AssertContext;
}

namespace cue::tool_host
{
/// @brief Windows D3D12 Tool Hostの回復可能な失敗分類
enum class ToolHostError : std::int64_t
{
    InvalidConfiguration = 1,
    WindowInitializationFailed = 2,
    D3d12InitializationFailed = 3,
    ImGuiInitializationFailed = 4,
    FenceSignalFailed = 5,
    FenceWaitFailed = 6,
    GpuCompletionUnavailable = 7,
    DeviceRemoved = 8,
    PresentFailed = 9,
    SwapChainResizeFailed = 10,
    FenceValueExhausted = 11,
};

/// @brief Tool Host Windowと自動Smoke終了条件を指定する
struct ToolHostDescriptor final
{
    std::string_view title;
    WindowSize clientSize;
    std::uint64_t maximumFrameCount;
    /// @brief Main Executableに埋め込まれたInteger Icon Resourceを選択し、0ではWindow Iconを設定しない
    /// @details 非0のResourceが存在しない場合はWindowInitializationFailedでHost起動を中止する
    std::uint16_t iconResourceId = 0U;
};

/// @brief Tool固有Presentationを共通Windows D3D12 Hostへ接続する
class ToolHostClient
{
  public:
    /// @brief Presentation Callback所有権の複製を禁止する
    ToolHostClient(const ToolHostClient &) = delete;
    /// @brief Presentation Callback所有権の複製を禁止する
    ToolHostClient &operator=(const ToolHostClient &) = delete;

    /// @brief 派生ClientをHostより先に破棄しない所有契約を提供する
    virtual ~ToolHostClient() = default;

    /// @brief 現在FrameのImGui Widgetを構築する
    virtual void draw_frame() noexcept = 0;

    /// @brief Native Window終了要求をTool固有の保存確認または終了状態へ変換する
    virtual void request_close() noexcept = 0;

    /// @brief PresentationがTool Session終了を要求したか返す
    [[nodiscard]] virtual bool should_close() const noexcept = 0;

  protected:
    /// @brief 非所有Presentation Callback境界だけを派生実装へ提供する
    ToolHostClient() noexcept = default;
};

/// @brief Windows Window、D3D12、ImGui Backendを所有してTool UI Loopを実行する
[[nodiscard]] Result<void> run_windows_d3d12_tool_host(const ToolHostDescriptor &a_descriptor, ToolHostClient &a_client,
                                                       const AssertContext &a_assertContext) noexcept;
} // namespace cue::tool_host
