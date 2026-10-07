#include <DX12/DX12DescriptorAllocator.h>
#include <DX12/DX12PipelineManager.h>
#include <DX12/DX12QueuePool.h>
#include <DX12/DX12RenderDevice.h>
#include <DX12/DX12SwapChain.h>
#include <DX12/DX12ViewManager.h>

#include <array>
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
    auto srvResult =
        cue::dx12::DX12DescriptorAllocator::create(*device->device(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, true);
    if (!srvResult.has_value())
    {
        return 12;
    }
    auto srvAllocator = srvResult.take_value();
    auto viewsResult = cue::dx12::DX12ViewManager::create(*device, *allocator, *srvAllocator);
    if (!viewsResult.has_value())
    {
        return 13;
    }
    auto views = viewsResult.take_value();
    auto pipelinesResult = cue::dx12::DX12PipelineManager::create(*device);
    if (!pipelinesResult.has_value())
    {
        return 90;
    }
    auto pipelines = pipelinesResult.take_value();
    const cue::dx12::DX12ResourceContext resources{*device, *views, *pipelines};
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
        auto swapResult =
            cue::dx12::DX12SwapChain::create(resources, std::move(lease), *handleResult.try_value(), config);
        if (!swapResult.has_value())
        {
            return 8;
        }
        auto swapChain = swapResult.take_value();
        auto *graphicsQueue = swapChain->graphics_queue();
        auto *originalBuffer = swapChain->back_buffer(0);
        const bool isTearingEnabled = swapChain->is_tearing_enabled();
        // 無効な寸法と同一寸法では旧 BackBuffer／RTV／Queue の所有状態を変えない
        if (swapChain->resize(0, 240).has_value() || swapChain->resize(320, 0).has_value() ||
            !swapChain->resize(320, 240).has_value() || swapChain->back_buffer(0) != originalBuffer ||
            swapChain->graphics_queue() != graphicsQueue)
        {
            return 14;
        }
        for (std::uint32_t index = 0; index < 2; ++index)
        {
            auto* buffer = swapChain->back_buffer(index);
            if (!buffer || buffer->GetDesc().Width != 320 || buffer->GetDesc().Height != 240 ||
                buffer->GetDesc().Format != config.format || !swapChain->rtv(index).has_value())
            {
                return 9;
            }
        }
        // VSync の有無を切り替えたそれぞれの SwapChain で連続 Resize と Present を行う
        // Queue Lease と Tearing の実際の許可状態は Native SwapChain の再利用中も保持する
        const std::array<std::array<std::uint32_t, 2>, 5> sizes{
            {{480, 270}, {123, 97}, {640, 360}, {64, 64}, {320, 240}}};
        for (const auto size : sizes)
        {
            if (!swapChain->resize(size[0], size[1]).has_value() || swapChain->graphics_queue() != graphicsQueue ||
                swapChain->is_tearing_enabled() != isTearingEnabled || swapChain->current_index() >= 2)
            {
                return 15;
            }
            for (std::uint32_t index = 0; index < 2; ++index)
            {
                auto *buffer = swapChain->back_buffer(index);
                if (!buffer || buffer->GetDesc().Width != size[0] || buffer->GetDesc().Height != size[1] ||
                    buffer->GetDesc().Format != config.format || !swapChain->rtv(index).has_value())
                {
                    return 16;
                }
            }
            if (!swapChain->present().has_value())
            {
                return 17;
            }
        }
        if (swapChain->back_buffer(2) || swapChain->rtv(2).has_value() || swapChain->current_index() >= 2 ||
            !swapChain->present().has_value() || !swapChain->shutdown().has_value() ||
            !swapChain->shutdown().has_value())
        {
            return 10;
        }
        if (swapChain->resize(320, 240).has_value() || swapChain->resize(640, 480).has_value())
        {
            return 18;
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
