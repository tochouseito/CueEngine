#include "D3D12DeviceContext.h"

#include <iterator>

/// @brief 選択した機能レベルが Driver の最大対応レベルと一致することを確認する
int main()
{
    auto deviceResult = cue::detail::D3D12DeviceContext::create();
    if (!deviceResult.has_value())
    {
        return 1;
    }
    auto device = deviceResult.take_value();

    constexpr D3D_FEATURE_LEVEL k_featureLevels[] = {
        D3D_FEATURE_LEVEL_12_2,
        D3D_FEATURE_LEVEL_12_1,
        D3D_FEATURE_LEVEL_12_0,
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };
    D3D12_FEATURE_DATA_FEATURE_LEVELS supportedLevels{};
    supportedLevels.NumFeatureLevels = static_cast<UINT>(std::size(k_featureLevels));
    supportedLevels.pFeatureLevelsRequested = k_featureLevels;
    if (FAILED(device->device()->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS,
                                                      &supportedLevels, sizeof(supportedLevels))))
    {
        return 2;
    }
    if (device->feature_level() != supportedLevels.MaxSupportedFeatureLevel)
    {
        return 3;
    }
    return 0;
}
