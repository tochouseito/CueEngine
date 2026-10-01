#pragma once

#include <memory>

#include <Foundation/Result.h>
#include <RHI/Backend.h>

namespace cue::dx12
{
class DX12RenderDevice;
class DX12QueuePool;

/// @brief DX12 Device と後続の GPU 資源を一意所有する Backend
///
/// Device と QueuePool を所有する。生成、停止、破棄は同一 Thread から直列に行う
class DX12Backend final : public IBackend
{
    struct CreateToken final
    {
    };

public:
    /// @brief create が成功した Device と QueuePool の所有権を受け取る
    DX12Backend(CreateToken, std::unique_ptr<DX12RenderDevice> a_device,
                std::unique_ptr<DX12QueuePool> a_queuePool) noexcept;

    /// @brief Device と QueuePool を生成し、成功時だけ Backend を公開する
    [[nodiscard]] static Result<std::unique_ptr<DX12Backend>> create();

    /// @brief 明示停止されていない Queue の GPU 作業も待って解放する
    ~DX12Backend() override;

    /// @brief GPU 資源の所有権を複製させない
    DX12Backend(const DX12Backend&) = delete;
    /// @brief GPU 資源の所有権を複製させない
    DX12Backend& operator=(const DX12Backend&) = delete;

    /// @brief Queue を待ってから Device を解放し、停止後の再呼出しも成功する
    [[nodiscard]] Result<void> shutdown() override;

    /// @brief 稼働中だけ Device を借用し、停止後は nullptr を返す
    [[nodiscard]] IRenderDevice* get_render_device() noexcept override;

    /// @brief 稼働中だけ QueuePool を借用させる
    [[nodiscard]] IQueuePool* get_queue_pool() noexcept override;

private:
    std::unique_ptr<DX12RenderDevice> m_device;
    std::unique_ptr<DX12QueuePool> m_queuePool;
};
} // namespace cue::dx12
