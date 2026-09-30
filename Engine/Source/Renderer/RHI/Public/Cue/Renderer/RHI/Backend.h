#pragma once

#include <Cue/Foundation/Result.h>
#include <Cue/Platform/WindowEvent.h>
#include <Cue/Renderer/RHI/BufferManager.h>
#include <Cue/Renderer/RHI/Command.h>
#include <Cue/Renderer/RHI/TextureManager.h>
#include <Cue/Renderer/RHI/ViewManager.h>
#include <Cue/Renderer/RHI/Queue.h>
#include <Cue/Renderer/RHI/RenderDevice.h>
#include <Cue/Renderer/RHI/PipelineManager.h>

#include <cstdint>

namespace cue
{
class FrameGraph;
/// @brief 適用済み Surface と成功した Present の診断値を保持する
struct BackendProgress final
{
    WindowSize surfaceSize{};
    std::uint64_t presentedFrames = 0;
};

/// @brief Host が Platform 固有実装を知らずに描画と停止を進める契約
/// @details Window と Backend の生成は具体実装の static create が担う。Render は単一 Thread から直列に呼ぶ
class IBackend
{
public:
    virtual ~IBackend() = default;

    /// @brief GPU 作業の完了後に所有資源を解放する。失敗時は再試行できる所有状態を保つ
    [[nodiscard]] virtual Result<void> shutdown() = 0;

    /// @brief 指定 Frame の記録、Submit、Present を進める
    [[nodiscard]] virtual Result<void> render_frame(std::uint64_t a_frame) = 0;

    /// @brief 外部 Pass と Resource Binding を持つ Graph を Submit して表示する
    [[nodiscard]] virtual Result<void> render_graph_frame(std::uint64_t a_frame,
                                                           FrameGraph& a_graph) = 0;

    /// @brief Window の最新表示状態を反映する。Render との並行呼出を実装側で同期する
    [[nodiscard]] virtual Result<void> request_surface(WindowSize a_clientSize, bool a_isMinimized) = 0;

    /// @brief 適用済み Surface と Present 数の Snapshot を返す
    [[nodiscard]] virtual Result<BackendProgress> progress() const = 0;

    /// @brief Backend が生存する間だけ Buffer Manager を借用する。停止後は nullptr を返す
    [[nodiscard]] virtual IBufferManager* get_buffer_manager() noexcept = 0;

    /// @brief Backend が生存する間だけ Texture Manager を借用する。停止後は nullptr を返す
    [[nodiscard]] virtual ITextureManager* get_texture_manager() noexcept = 0;

    /// @brief Backend が生存する間だけ View Manager を借用する。停止後は nullptr を返す
    [[nodiscard]] virtual IViewManager* get_view_manager() noexcept = 0;

    /// @brief Backend が生存する間だけ Queue Pool を借用する。停止後は nullptr を返す
    [[nodiscard]] virtual IQueuePool* get_queue_pool() noexcept = 0;

    /// @brief Backend が生存する間だけ Render Device を借用する。停止後は nullptr を返す
    [[nodiscard]] virtual IRenderDevice* get_render_device() noexcept = 0;

    /// @brief Backend が生存する間だけ Pipeline Manager を借用する。停止後は nullptr を返す
    [[nodiscard]] virtual IPipelineManager* get_pipeline_manager() noexcept = 0;

    /// @brief Backend が生存する間だけ Graphics Command Pool を借用する。停止後は nullptr を返す
    [[nodiscard]] virtual ICommandPool* get_command_pool() noexcept = 0;
};
} // namespace cue
