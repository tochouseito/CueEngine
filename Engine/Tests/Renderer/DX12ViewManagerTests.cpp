#include <DX12/DX12ViewManager.h>

#include <DX12/DX12GpuResource.h>
#include <DX12/DX12RenderDevice.h>

namespace
{
/// @brief WARP で検証失敗時の Slot 保全と世代、Mip 範囲、Device 所属を確認する
int run_tests()
{
    using namespace cue;
    using namespace cue::dx12;
    auto deviceResult = DX12RenderDevice::create(AdapterSelection::Warp);
    auto foreignResult = DX12RenderDevice::create(AdapterSelection::HardwarePreferred);
    if (!deviceResult.has_value() || !foreignResult.has_value())
    {
        return 1;
    }
    auto device = deviceResult.take_value();
    auto foreign = foreignResult.take_value();
    // 同じ Adapter の Device は再利用されるため、Hardware がある環境だけ異なる Device を検証する
    const bool hasForeignDevice = foreign->device() != device->device();
    auto rtvResult = DX12DescriptorAllocator::create(*device->device(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1);
    auto srvResult =
        DX12DescriptorAllocator::create(*device->device(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, true);
    auto foreignHeapResult = DX12DescriptorAllocator::create(*foreign->device(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1);
    if (!rtvResult.has_value() || !srvResult.has_value() || !foreignHeapResult.has_value())
    {
        return 2;
    }
    auto rtv = rtvResult.take_value();
    auto srv = srvResult.take_value();
    auto foreignHeap = foreignHeapResult.take_value();
    if ((hasForeignDevice && DX12ViewManager::create(*device, *foreignHeap, *srv).has_value()) ||
        DX12ViewManager::create(*device, *srv, *rtv).has_value())
    {
        return 3;
    }
    auto managerResult = DX12ViewManager::create(*device, *rtv, *srv);
    if (!managerResult.has_value())
    {
        return 4;
    }
    auto manager = managerResult.take_value();
    GpuTexture2DDesc desc{8, 8};
    desc.mipLevels = 4;
    desc.isRenderTarget = true;
    auto textureResult = DX12GpuResource::create_texture2d(*device->device(), desc);
    auto foreignTextureResult = DX12GpuResource::create_texture2d(*foreign->device(), desc);
    auto bufferResult = DX12GpuResource::create_buffer(*device->device(), {64, GpuMemoryUsage::Default});
    auto plainResult = DX12GpuResource::create_texture2d(*device->device(), {8, 8});
    if (!textureResult.has_value() || !foreignTextureResult.has_value() || !bufferResult.has_value() ||
        !plainResult.has_value())
    {
        return 5;
    }
    auto texture = textureResult.take_value();
    auto foreignTexture = foreignTextureResult.take_value();
    auto buffer = bufferResult.take_value();
    auto plain = plainResult.take_value();

    // 不正な入力が容量 1 の Heap を消費せず、後続の正しい生成を妨げない
    if (manager->create_rtv(*plain->resource()).has_value() ||
        (hasForeignDevice && manager->create_rtv(*foreignTexture->resource()).has_value()) ||
        manager->create_srv(*buffer->resource()).has_value() ||
        manager->create_rtv(*texture->resource(), {DXGI_FORMAT_UNKNOWN, 4, 1}).has_value() ||
        manager->create_rtv(*texture->resource(), {DXGI_FORMAT_UNKNOWN, 0, 2}).has_value() ||
        manager->create_srv(*texture->resource(), {DXGI_FORMAT_UNKNOWN, 2, 3}).has_value() ||
        manager->create_srv(*texture->resource(), {DXGI_FORMAT_R32_FLOAT, 0, 1}).has_value() ||
        manager->reserve(static_cast<DX12ViewType>(255)).has_value())
    {
        return 6;
    }
    auto targetResult = manager->create_rtv(*texture->resource(), {DXGI_FORMAT_UNKNOWN, 1, 1});
    auto readResult = manager->create_srv(*texture->resource(), {DXGI_FORMAT_UNKNOWN, 1, 0});
    if (!targetResult.has_value() || !readResult.has_value())
    {
        return 7;
    }
    const auto target = targetResult.take_value();
    const auto read = readResult.take_value();
    if (!manager->cpu_handle(target).has_value() || !manager->cpu_handle(read).has_value() ||
        !manager->gpu_handle(read).has_value() || manager->gpu_handle(target).has_value() ||
        manager->create_rtv(*texture->resource()).has_value() ||
        manager->create_srv(*texture->resource()).has_value() ||
        (hasForeignDevice && manager->write_texture2d(target, *foreignTexture->resource()).has_value()) ||
        !manager->release(target).has_value() || !manager->release(read).has_value())
    {
        return 8;
    }

    // import の予約 Slot は Binding 確定時に更新でき、旧世代の Handle は使えない
    auto reservedResult = manager->reserve(DX12ViewType::RenderTarget);
    if (!reservedResult.has_value())
    {
        return 9;
    }
    const auto reserved = reservedResult.take_value();
    if (manager->cpu_handle(target).has_value() || manager->release(target).has_value() ||
        manager->write_texture2d(target, *texture->resource()).has_value() ||
        manager->write_texture2d(reserved, *plain->resource()).has_value() ||
        !manager->write_texture2d(reserved, *texture->resource()).has_value() ||
        !manager->release(reserved).has_value())
    {
        return 10;
    }
    return 0;
}
} // namespace

/// @brief View の異常系が Slot の漏れや古い Handle の再利用を生まないことを確認する
int main()
{
    return run_tests();
}
