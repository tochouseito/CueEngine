#pragma once

#include <Cue/Renderer/FrameGraph/FrameGraph.h>
#include <Cue/Renderer/RHI/GpuCommands.h>
#include <Cue/Renderer/RHI/Command.h>

namespace cue
{
/// @brief Pass 実行時の表示領域、Frame Slot と Command Context の借用を束ねる
struct FrameGraphContextDesc final
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t frameIndex = 0;
    ICommandContext* commandContext = nullptr;
    void* passStats = nullptr;
    GpuViewHandle surfaceColorView{};
    GpuViewHandle surfaceDepthView{};
};

/// @brief Pass 実行中だけ有効な Command Recorder と実行情報の借用を保持する
class FrameGraphContext final
{
public:
    /// @brief 呼出元が Recorder を所有する一 Pass の実行期間だけ借用する
    explicit FrameGraphContext(IGpuCommandRecorder& a_commands, FrameGraphContextDesc a_desc = {}) noexcept
        : m_commands(a_commands), m_desc(a_desc) {}

    /// @brief Barrier 適用後の Recorder を貸す
    [[nodiscard]] IGpuCommandRecorder& commands() const noexcept { return m_commands; }

    /// @brief Pass 対象の表示領域幅を返す
    [[nodiscard]] std::uint32_t width() const noexcept { return m_desc.width; }

    /// @brief Pass 対象の表示領域高さを返す
    [[nodiscard]] std::uint32_t height() const noexcept { return m_desc.height; }

    /// @brief Back Buffer の Frame Slot を返す
    [[nodiscard]] std::uint32_t frame_index() const noexcept { return m_desc.frameIndex; }

    /// @brief Recorder が借用する物理 Command Context を返す
    [[nodiscard]] ICommandContext* command_context() const noexcept { return m_desc.commandContext; }

    /// @brief 計測が有効なときだけ Pass 統計の借用先を返す
    [[nodiscard]] void* pass_stats() const noexcept { return m_desc.passStats; }

    /// @brief 現在 Frame Slot の Color Surface を描く View を借用する
    [[nodiscard]] GpuViewHandle surface_color_view() const noexcept { return m_desc.surfaceColorView; }

    /// @brief Resize 後の現行 Depth Surface View を借用する
    [[nodiscard]] GpuViewHandle surface_depth_view() const noexcept { return m_desc.surfaceDepthView; }

private:
    IGpuCommandRecorder& m_commands;
    FrameGraphContextDesc m_desc;
};

/// @brief Resource 宣言と Command 記録を分ける Pass の契約
class FrameGraphPass
{
public:
    virtual ~FrameGraphPass() = default;

    /// @brief Graph に登録する Pass 名を返す
    [[nodiscard]] virtual const char* name() const noexcept = 0;

    /// @brief Graph に Resource 使用と依存を宣言する
    [[nodiscard]] virtual Result<GraphPassHandle> setup(FrameGraphBuilder& a_builder) const = 0;

    /// @brief Graph の Barrier 適用後に命令を記録する
    [[nodiscard]] virtual Result<void> execute(FrameGraphContext& a_context) const = 0;
};
} // namespace cue
