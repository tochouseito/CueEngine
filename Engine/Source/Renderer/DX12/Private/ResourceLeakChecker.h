#pragma once

#include <dxgidebug.h>
#include <wrl/client.h>

namespace cue::detail
{
/// @brief Backend の他の COM Owner 破棄後に Debug Layer の Live Object を報告する
class ResourceLeakChecker final
{
public:
    /// @brief Debug 構成だけ DXGI の Live Object を診断出力する
    ~ResourceLeakChecker()
    {
#if defined(_DEBUG) && !defined(CUE_SHIPPING)
        Microsoft::WRL::ComPtr<IDXGIDebug1> debug;
        if (SUCCEEDED(DXGIGetDebugInterface1(0, IID_PPV_ARGS(&debug))))
        {
            debug->ReportLiveObjects(DXGI_DEBUG_D3D12, DXGI_DEBUG_RLO_SUMMARY);
        }
#endif
    }
};
} // namespace cue::detail
