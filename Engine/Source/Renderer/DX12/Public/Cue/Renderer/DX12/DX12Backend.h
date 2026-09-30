#pragma once

#include <Cue/Renderer/RHI/Backend.h>

#include <cstdint>
#include <memory>

namespace cue
{
/// @brief Windows の表示資源と DX12 の実行資源を内部 Owner に分けて一意所有する
///
/// Native Handle は Window が生存する間だけ借用する。生成と停止は Host の MainThread で行う
/// 描画は Runtime の直列 Render Callback から行い、停止前にその Worker を join する
class DX12Backend final : public IBackend
{
public:
    /// @brief Hardware を優先し、対応 Adapter がなければ WARP で GPU 資源を作る
    ///
    /// a_nativeWindowはWindows Windowの有効なNative Handleを非所有で借用する
    /// 失敗時は部分生成資源を公開せず、操作名とHRESULTを返す
    [[nodiscard]] static Result<std::unique_ptr<DX12Backend>> create(void* a_nativeWindow,
                                                                         WindowSize a_clientSize);

    /// @brief 明示停止されていないGPU資源も回収する
    ~DX12Backend() override;

    DX12Backend(const DX12Backend&) = delete;
    DX12Backend& operator=(const DX12Backend&) = delete;
    DX12Backend(DX12Backend&&) noexcept = default;
    DX12Backend& operator=(DX12Backend&&) = delete;

    /// @brief GPU完了を待ってFrame Resourceを解放する
    ///
    /// Render Callback停止後に呼ぶ。再呼出は成功する。GPU待機失敗時は所有資源を保持し再試行できる
    [[nodiscard]] Result<void> shutdown() override;

    /// @brief 指定FrameのClear、固定Mesh、Copy PassをSubmitしてPresentする
    ///
    /// 初回の呼出ThreadをRender Threadとし、以降は同じThreadから直列に呼ぶ
    /// 同一または古いFrame番号を拒否する。失敗後はHostが停止へ進む
    [[nodiscard]] Result<void> render_frame(std::uint64_t a_frame) override;

    /// @brief 呼出側が組み立てた Graph を現行 Surface に結び付けて実行する
    [[nodiscard]] Result<void> render_graph_frame(std::uint64_t a_frame,
                                                   FrameGraph& a_graph) override;

    /// @brief MainThreadから最新のWindow表示状態を渡す
    ///
    /// 呼出しはRender Callbackと並行できる。最小化中またはSizeが0の間は描画を保留し、復帰後に最新Sizeを適用する
    [[nodiscard]] Result<void> request_surface(WindowSize a_clientSize, bool a_isMinimized) override;

    /// @brief Render Threadが適用したSurfaceとPresent数のSnapshotを返す
    ///
    /// request_surfaceとrender_frameの並行中も呼べる。shutdownとの並行呼出は行わない
    [[nodiscard]] Result<BackendProgress> progress() const override;

    /// @brief Backend 所有の Buffer Manager を借用する
    [[nodiscard]] IBufferManager* get_buffer_manager() noexcept override;

    /// @brief Backend 所有の Texture Manager を借用する
    [[nodiscard]] ITextureManager* get_texture_manager() noexcept override;

    /// @brief Backend 所有の View Manager を借用する
    [[nodiscard]] IViewManager* get_view_manager() noexcept override;

    /// @brief Backend 所有の Queue Pool を借用する
    [[nodiscard]] IQueuePool* get_queue_pool() noexcept override;

    /// @brief Backend 所有の Render Device を借用する
    [[nodiscard]] IRenderDevice* get_render_device() noexcept override;

    /// @brief Backend 所有の Pipeline Manager を借用する
    [[nodiscard]] IPipelineManager* get_pipeline_manager() noexcept override;

    /// @brief Backend 所有の Graphics Command Pool を借用する
    [[nodiscard]] ICommandPool* get_command_pool() noexcept override;

    /// @brief 選択したAdapterがWARPかを返す
    [[nodiscard]] bool is_warp() const noexcept;

private:
    class State;

    /// @brief 完全初期化したStateの所有権を受け取る
    explicit DX12Backend(std::unique_ptr<State> a_state) noexcept;

    std::unique_ptr<State> m_state;
};
} // namespace cue
