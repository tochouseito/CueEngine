#pragma once

#include <Foundation/Result.h>
#include <RHI/RenderDevice.h>

namespace cue
{
/// @brief Renderer Backend が生成した Device と後続の GPU 資源を一意所有する契約
///
/// 呼出側が所有し、利用者より長く生存させる。生成、停止、破棄は同一 Thread から直列に行う
class IBackend
{
public:
    /// @brief 派生 Backend を基底 Pointer から安全に破棄する
    virtual ~IBackend() = default;

    /// @brief Backend の所有権を複製させない
    IBackend(const IBackend&) = delete;
    /// @brief Backend の所有権を複製させない
    IBackend& operator=(const IBackend&) = delete;

    /// @brief GPU 資源を解放し、以後の Device 借用を無効にする
    ///
    /// 複数回呼べる。失敗時は Error を返し、呼出側は破棄処理を続ける
    [[nodiscard]] virtual Result<void> shutdown() = 0;

    /// @brief Backend が稼働する間だけ Device を借用し、停止後は nullptr を返す
    [[nodiscard]] virtual IRenderDevice* get_render_device() noexcept = 0;

protected:
    /// @brief 具体 Backend の生成経路だけが基底契約を構築する
    IBackend() = default;
};
} // namespace cue
