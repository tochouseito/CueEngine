#include <DX12/DX12RenderDevice.h>

#include <algorithm>
#include <array>
#include <memory>
#include <utility>

#include <RHI/RenderDevice.h>

/// @brief 実 Adapter または WARP で Device と Factory の所有契約を検証する
int main()
{
    auto deviceResult = cue::dx12::DX12RenderDevice::create();
    if (!deviceResult.has_value())
    {
        return 1;
    }
    auto renderDevice = deviceResult.take_value();
    if (!renderDevice || !renderDevice->factory() || !renderDevice->adapter() || !renderDevice->device())
    {
        return 2;
    }

    // 選択した Adapter の種類と Device の LUID が同じ実体を指すことを確認する
    DXGI_ADAPTER_DESC1 desc{};
    if (FAILED(renderDevice->adapter()->GetDesc1(&desc)) ||
        renderDevice->is_warp() != ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0))
    {
        return 3;
    }
    cue::IRenderDevice& abstractDevice = *renderDevice;
    if (abstractDevice.is_software_adapter() != renderDevice->is_warp())
    {
        return 9;
    }
    const LUID deviceLuid = renderDevice->device()->GetAdapterLuid();
    if (deviceLuid.HighPart != desc.AdapterLuid.HighPart || deviceLuid.LowPart != desc.AdapterLuid.LowPart)
    {
        return 4;
    }

    // 公開する Feature Level が試行候補に含まれ、実 Device が機能照会に応答することを確認する
    constexpr std::array levels = {
        D3D_FEATURE_LEVEL_12_2,
        D3D_FEATURE_LEVEL_12_1,
        D3D_FEATURE_LEVEL_12_0,
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };
    if (std::find(levels.begin(), levels.end(), renderDevice->feature_level()) == levels.end())
    {
        return 5;
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
    if (FAILED(renderDevice->device()->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options))))
    {
        return 6;
    }

    // Hardware の有無に依存せず WARP 指定の Device 生成経路を検証する
    auto warpResult = cue::dx12::DX12RenderDevice::create(cue::dx12::AdapterSelection::Warp);
    if (!warpResult.has_value())
    {
        return 7;
    }
    auto warpDevice = warpResult.take_value();
    DXGI_ADAPTER_DESC1 warpDesc{};
    if (!warpDevice->is_warp() || !warpDevice->device() || !warpDevice->adapter() ||
        FAILED(warpDevice->adapter()->GetDesc1(&warpDesc)) ||
        (warpDesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0)
    {
        return 8;
    }
    // RHI の所有者からも派生 Device を破棄できることを検証する
    std::unique_ptr<cue::IRenderDevice> ownedDevice = std::move(warpDevice);
    if (!ownedDevice->is_software_adapter())
    {
        return 10;
    }
    return 0;
}
