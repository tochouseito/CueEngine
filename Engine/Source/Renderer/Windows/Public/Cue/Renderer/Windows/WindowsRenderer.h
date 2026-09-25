#pragma once

#include <Cue/Foundation/Result.h>
#include <Cue/Platform/WindowEvent.h>

#include <memory>

namespace cue
{
/// @brief WindowsのSwap ChainとD3D12 Frame Resourceを一意所有する
///
/// Native HandleはWindowが生存する間だけ借用する。生成と停止はHostのMainThreadで行う
/// 描画操作は後続Issueで直列のRender Callbackへ接続する
class WindowsRenderer final
{
public:
    /// @brief Hardwareまたは明示的なWARP AdapterでGPU資源を作る
    ///
    /// a_nativeWindowはWindows Windowの有効なNative Handleを非所有で借用する
    /// 失敗時は部分生成資源を公開せず、操作名とHRESULTを返す
    [[nodiscard]] static Result<std::unique_ptr<WindowsRenderer>> create(void* a_nativeWindow,
                                                                           WindowSize a_clientSize,
                                                                           bool a_useWarp = false);

    /// @brief 明示停止されていないGPU資源も回収する
    ~WindowsRenderer();

    WindowsRenderer(const WindowsRenderer&) = delete;
    WindowsRenderer& operator=(const WindowsRenderer&) = delete;
    WindowsRenderer(WindowsRenderer&&) noexcept = default;
    WindowsRenderer& operator=(WindowsRenderer&&) = delete;

    /// @brief GPU完了を待ってFrame Resourceを解放する
    ///
    /// Render Callback停止後に呼ぶ。再呼出は成功する。失敗時も所有資源を回収する
    [[nodiscard]] Result<void> shutdown();

    /// @brief 選択したAdapterがWARPかを返す
    [[nodiscard]] bool is_warp() const noexcept;

private:
    class State;

    /// @brief 完全初期化したStateの所有権を受け取る
    explicit WindowsRenderer(std::unique_ptr<State> a_state) noexcept;

    std::unique_ptr<State> m_state;
};
} // namespace cue
