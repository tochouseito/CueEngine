#include <DX12/DX12Backend.h>

#include <utility>

#include <DX12/DX12RenderDevice.h>

namespace cue::dx12
{
/// @brief 検証済み Device の所有権を Backend へ移す
DX12Backend::DX12Backend(CreateToken, std::unique_ptr<DX12RenderDevice> a_device) noexcept
    : m_device(std::move(a_device))
{
}

/// @brief Device 生成に失敗した場合は Backend を公開しない
Result<std::unique_ptr<DX12Backend>> DX12Backend::create()
{
    auto deviceResult = DX12RenderDevice::create();
    if (!deviceResult.has_value())
    {
        return Result<std::unique_ptr<DX12Backend>>::failure(*deviceResult.try_error());
    }

    // 完成した Device だけを Backend に移し、失敗時の部分所有を作らない
    return Result<std::unique_ptr<DX12Backend>>::success(
        std::make_unique<DX12Backend>(CreateToken{}, deviceResult.take_value()));
}

/// @brief 明示停止されていない Device も破棄する
DX12Backend::~DX12Backend() = default;

/// @brief Device の所有権を解放し、借用 Pointer を失効させる
Result<void> DX12Backend::shutdown()
{
    m_device.reset();
    return Result<void>::success();
}

/// @brief Backend が稼働する間だけ Device を借用させる
IRenderDevice* DX12Backend::get_render_device() noexcept
{
    return m_device.get();
}
} // namespace cue::dx12
