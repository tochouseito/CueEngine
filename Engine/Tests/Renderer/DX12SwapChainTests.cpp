#include <DX12/DX12DescriptorAllocator.h>
#include <DX12/DX12QueuePool.h>
#include <DX12/DX12RenderDevice.h>
#include <DX12/DX12SwapChain.h>

#include <cstdint>
#include <memory>
#include <utility>

#include <Platform/Windows/WindowsPlatform.h>

namespace
{
/// @brief WARP の Graphics Queue で Back Buffer 取得と二種類の Present 設定を検証する
int run_tests()
{
    auto systemResult = cue::create_windows_window_system();
    if (!systemResult.has_value())
    {
        return 1;
    }
    auto system = systemResult.take_value();
    auto windowResult = system->create_window({"CueEngine SwapChain Test", {320, 240}});
    if (!windowResult.has_value())
    {
        return 2;
    }
    auto window = windowResult.take_value();
    if (!window->show().has_value())
    {
        return 3;
    }
    auto handleResult = cue::borrow_windows_window_handle(*window);
    if (!handleResult.has_value())
    {
        return 4;
    }

    auto deviceResult = cue::dx12::DX12RenderDevice::create(cue::dx12::AdapterSelection::Warp);
    if (!deviceResult.has_value() || !(*deviceResult.try_value())->is_warp())
    {
        return 5;
    }
    auto device = deviceResult.take_value();
    auto allocatorResult = cue::dx12::DX12DescriptorAllocator::create(
        *device->device(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 4);
    if (!allocatorResult.has_value())
    {
        return 6;
    }
    auto allocator = allocatorResult.take_value();
    for (const bool isVSyncEnabled : {false, true})
    {
        auto queueResult = cue::dx12::DX12GpuCommandQueue::create(
            *device->device(), cue::QueueType::Graphics, 0);
        if (!queueResult.has_value())
        {
            return 7;
        }
        cue::queueLease lease(queueResult.take_value().release(), [](cue::IQueueContext* a_queue) {
            delete a_queue;
        });
        const cue::dx12::DX12SwapChainConfig config{
            320, 240, 2, DXGI_FORMAT_R8G8B8A8_UNORM, isVSyncEnabled, !isVSyncEnabled};
        auto swapResult = cue::dx12::DX12SwapChain::create(
            *device, std::move(lease), *allocator, *handleResult.try_value(), config);
        if (!swapResult.has_value())
        {
            return 8;
        }
        auto swapChain = swapResult.take_value();
        for (std::uint32_t index = 0; index < 2; ++index)
        {
            auto* buffer = swapChain->back_buffer(index);
            if (!buffer || buffer->GetDesc().Width != 320 || buffer->GetDesc().Height != 240 ||
                buffer->GetDesc().Format != config.format || !swapChain->rtv(index).has_value())
            {
                return 9;
            }
        }
        if (swapChain->back_buffer(2) || swapChain->rtv(2).has_value() ||
            swapChain->current_index() >= 2 || !swapChain->present().has_value() ||
            !swapChain->shutdown().has_value() || !swapChain->shutdown().has_value())
        {
            return 10;
        }
    }

    if (!window->destroy().has_value())
    {
        return 11;
    }
    window.reset();
    system.reset();
    return 0;
}
} // namespace

/// @brief 実 Window と WARP で SwapChain の最小寿命を検証する
int main()
{
    return run_tests();
}
