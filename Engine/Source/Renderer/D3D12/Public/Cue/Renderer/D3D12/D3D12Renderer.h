#pragma once

#include <Cue/Foundation/Result.h>
#include <Cue/Platform/WindowEvent.h>

#include <cstdint>
#include <memory>

namespace cue
{
/// @brief 適用済みSurfaceと成功したPresentの診断値を保持する
struct D3D12RendererProgress final
{
    WindowSize surfaceSize{};
    std::uint64_t presentedFrames = 0;
};

/// @brief Windows の表示資源と D3D12 の実行資源を内部 Owner に分けて一意所有する
///
/// Native Handle は Window が生存する間だけ借用する。生成と停止は Host の MainThread で行う
/// 描画は Runtime の直列 Render Callback から行い、停止前にその Worker を join する
class D3D12Renderer final
{
public:
    /// @brief Hardwareまたは明示的なWARP AdapterでGPU資源を作る
    ///
    /// a_nativeWindowはWindows Windowの有効なNative Handleを非所有で借用する
    /// 失敗時は部分生成資源を公開せず、操作名とHRESULTを返す
    [[nodiscard]] static Result<std::unique_ptr<D3D12Renderer>> create(void* a_nativeWindow,
                                                                           WindowSize a_clientSize,
                                                                           bool a_useWarp = false);

    /// @brief 明示停止されていないGPU資源も回収する
    ~D3D12Renderer();

    D3D12Renderer(const D3D12Renderer&) = delete;
    D3D12Renderer& operator=(const D3D12Renderer&) = delete;
    D3D12Renderer(D3D12Renderer&&) noexcept = default;
    D3D12Renderer& operator=(D3D12Renderer&&) = delete;

    /// @brief GPU完了を待ってFrame Resourceを解放する
    ///
    /// Render Callback停止後に呼ぶ。再呼出は成功する。GPU待機失敗時は所有資源を保持し再試行できる
    [[nodiscard]] Result<void> shutdown();

    /// @brief 指定FrameのClear、固定Mesh、Copy PassをSubmitしてPresentする
    ///
    /// 初回の呼出ThreadをRender Threadとし、以降は同じThreadから直列に呼ぶ
    /// 同一または古いFrame番号を拒否する。失敗後はHostが停止へ進む
    [[nodiscard]] Result<void> render_frame(std::uint64_t a_frame);

    /// @brief MainThreadから最新のWindow表示状態を渡す
    ///
    /// 呼出しはRender Callbackと並行できる。最小化中またはSizeが0の間は描画を保留し、復帰後に最新Sizeを適用する
    [[nodiscard]] Result<void> request_surface(WindowSize a_clientSize, bool a_isMinimized);

    /// @brief Render Threadが適用したSurfaceとPresent数のSnapshotを返す
    ///
    /// request_surfaceとrender_frameの並行中も呼べる。shutdownとの並行呼出は行わない
    [[nodiscard]] Result<D3D12RendererProgress> progress() const;

    /// @brief 選択したAdapterがWARPかを返す
    [[nodiscard]] bool is_warp() const noexcept;

private:
    class State;

    /// @brief 完全初期化したStateの所有権を受け取る
    explicit D3D12Renderer(std::unique_ptr<State> a_state) noexcept;

    std::unique_ptr<State> m_state;
};
} // namespace cue
