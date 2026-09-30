#pragma once

#include <memory>

#include <Foundation/Result.h>
#include <RHI/Backend.h>

namespace cue::dx12
{
class DX12RenderDevice;

/// @brief DX12 Device と後続の GPU 資源を一意所有する Backend
///
/// 現在は Device のみを所有する。生成、停止、破棄は同一 Thread から直列に行う
class DX12Backend final : public IBackend
{
    struct CreateToken final
    {
    };

public:
    /// @brief create が成功した Device の所有権を受け取る
    DX12Backend(CreateToken, std::unique_ptr<DX12RenderDevice> a_device) noexcept;

    /// @brief Device を生成し、成功時だけ Backend を公開する
    [[nodiscard]] static Result<std::unique_ptr<DX12Backend>> create();

    /// @brief 明示停止されていない Device も解放する
    ~DX12Backend() override;

    /// @brief GPU 資源の所有権を複製させない
    DX12Backend(const DX12Backend&) = delete;
    /// @brief GPU 資源の所有権を複製させない
    DX12Backend& operator=(const DX12Backend&) = delete;

    /// @brief Device を解放し、繰り返し呼ばれても成功する
    [[nodiscard]] Result<void> shutdown() override;

    /// @brief 稼働中だけ Device を借用し、停止後は nullptr を返す
    [[nodiscard]] IRenderDevice* get_render_device() noexcept override;

private:
    std::unique_ptr<DX12RenderDevice> m_device;
};
} // namespace cue::dx12
