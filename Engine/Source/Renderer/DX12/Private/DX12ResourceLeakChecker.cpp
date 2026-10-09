#include "DX12ResourceLeakChecker.h"

#if defined(_DEBUG) && !defined(CUE_SHIPPING)
#include <cstdint>

#include <dxgi1_3.h>
#include <dxgidebug.h>
#include <wrl/client.h>

#include <Platform/Diagnostics.h>
#endif

namespace cue::dx12
{
/// @brief Debug 構成で DXGI の Live Object 診断を停止処理から独立して実行する
void DX12ResourceLeakChecker::report_live_objects()
{
#if defined(_DEBUG) && !defined(CUE_SHIPPING)
    Microsoft::WRL::ComPtr<IDXGIDebug1> debug;
    const HRESULT interfaceResult = DXGIGetDebugInterface1(0, IID_PPV_ARGS(&debug));
    if (FAILED(interfaceResult))
    {
        // Graphics Tools がない環境でも Backend の停止は成功として扱う
        report_log("DX12ResourceLeakChecker", "DXGI Debug interface is unavailable; live object report was skipped",
                       LogLevel::Warning);
        return;
    }

    // ALL は DXGI と D3D12 を含み、内部参照だけで残る Object は除外する
    const HRESULT reportResult = debug->ReportLiveObjects(DXGI_DEBUG_ALL, DXGI_DEBUG_RLO_ALL);
    if (FAILED(reportResult))
    {
        report_log_error("DX12ResourceLeakChecker",
                     {ErrorCategory::PlatformFailure, "IDXGIDebug.ReportLiveObjects",
                      static_cast<std::int64_t>(reportResult)},
                     LogLevel::Warning);
    }
#endif
}
} // namespace cue::dx12
