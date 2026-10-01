#include <DX12/DX12Backend.h>

#include <utility>

#include <DX12/DX12RenderDevice.h>
#include <DX12/DX12QueuePool.h>
#include <Platform/Diagnostics.h>

namespace cue::dx12
{
/// @brief 検証済み Device の所有権を Backend へ移す
DX12Backend::DX12Backend(CreateToken, std::unique_ptr<DX12RenderDevice> a_device,
                         std::unique_ptr<DX12QueuePool> a_queuePool) noexcept
    : m_device(std::move(a_device)), m_queuePool(std::move(a_queuePool))
{
}

/// @brief Device または QueuePool の生成に失敗した場合は Backend を公開しない
Result<std::unique_ptr<DX12Backend>> DX12Backend::create()
{
    auto deviceResult = DX12RenderDevice::create();
    if (!deviceResult.has_value())
    {
        return Result<std::unique_ptr<DX12Backend>>::failure(*deviceResult.try_error());
    }

    // Device に依存する Queue を全種類生成し、失敗時は部分所有を公開しない
    auto queuePoolResult = DX12QueuePool::create(**deviceResult.try_value());
    if (!queuePoolResult.has_value())
    {
        return Result<std::unique_ptr<DX12Backend>>::failure(*queuePoolResult.try_error());
    }

    return Result<std::unique_ptr<DX12Backend>>::success(
        std::make_unique<DX12Backend>(CreateToken{}, deviceResult.take_value(), queuePoolResult.take_value()));
}

/// @brief 明示停止されていない Queue も GPU 完了後に破棄する
DX12Backend::~DX12Backend()
{
    auto result = shutdown();
    if (!result.has_value())
    {
        report_error("DX12Backend.shutdown", *result.try_error(), DiagnosticSeverity::Error);
    }
}

/// @brief 新規貸出を止め、借用中の Queue の寿命を Lease に引き継ぐ
Result<void> DX12Backend::shutdown()
{
    // Pool 停止に失敗しても Backend の所有を解放し、残る Lease が Queue を保持する
    Result<void> stopResult = Result<void>::success();
    if (m_queuePool)
    {
        stopResult = m_queuePool->shutdown();
        m_queuePool.reset();
    }
    m_device.reset();
    return stopResult;
}

/// @brief Backend が稼働する間だけ Device を借用させる
IRenderDevice* DX12Backend::get_render_device() noexcept
{
    return m_device.get();
}

/// @brief Backend が所有する QueuePool の借用 Pointer を返す
IQueuePool* DX12Backend::get_queue_pool() noexcept
{
    return m_queuePool.get();
}
} // namespace cue::dx12
