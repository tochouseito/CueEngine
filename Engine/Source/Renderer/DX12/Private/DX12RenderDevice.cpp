#include <DX12/DX12RenderDevice.h>

#include <cstdint>
#include <string_view>
#include <utility>

#include <d3d12sdklayers.h>
#include <d3dcommon.h>

#include <Platform/Diagnostics.h>

namespace cue::dx12
{
namespace
{
constexpr D3D_FEATURE_LEVEL k_featureLevels[] = {
    D3D_FEATURE_LEVEL_12_2,
    D3D_FEATURE_LEVEL_12_1,
    D3D_FEATURE_LEVEL_12_0,
    D3D_FEATURE_LEVEL_11_1,
    D3D_FEATURE_LEVEL_11_0,
};

/// @brief 対応する最高の Feature Level で Device を生成する
HRESULT create_device(IDXGIAdapter1* a_adapter, Microsoft::WRL::ComPtr<ID3D12Device>& a_device,
                      D3D_FEATURE_LEVEL& a_featureLevel)
{
    HRESULT result = E_FAIL;
    for (D3D_FEATURE_LEVEL level : k_featureLevels)
    {
        Microsoft::WRL::ComPtr<ID3D12Device> candidate;
        result = D3D12CreateDevice(a_adapter, level, IID_PPV_ARGS(&candidate));
        if (SUCCEEDED(result))
        {
            a_device = std::move(candidate);
            a_featureLevel = level;
            return result;
        }
    }
    return result;
}

/// @brief HRESULT を操作名とともに共通 Error へ変換する
Error gpu_error(const char* a_operation, HRESULT a_result)
{
    return {ErrorCategory::PlatformFailure, a_operation, static_cast<std::int64_t>(a_result)};
}
} // namespace

/// @brief create の内部でのみ Device を持たない中間状態を作る
DX12RenderDevice::DX12RenderDevice(CreateToken)
{
}

/// @brief DXGI Factory、Adapter、D3D12 Device を順に構築する
Result<std::unique_ptr<DX12RenderDevice>> DX12RenderDevice::create(AdapterSelection a_selection)
{
    using DeviceResult = Result<std::unique_ptr<DX12RenderDevice>>;
    auto context = std::make_unique<DX12RenderDevice>(CreateToken{});
    bool hasDebugLayer = false;

#if defined(_DEBUG) && !defined(CUE_SHIPPING)
    // 利用可能な Debug Layer と GPU Validation を Device 生成前に有効化する
    Microsoft::WRL::ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
    {
        debug->EnableDebugLayer();
        hasDebugLayer = true;

        Microsoft::WRL::ComPtr<ID3D12Debug1> gpuValidation;
        if (SUCCEEDED(debug.As(&gpuValidation)))
        {
            gpuValidation->SetEnableGPUBasedValidation(true);
        }
    }

    // Device Removed 時の原因を追跡できるよう DRED を先に設定する
    Microsoft::WRL::ComPtr<ID3D12DeviceRemovedExtendedDataSettings> deviceRemoved;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&deviceRemoved))))
    {
        deviceRemoved->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        deviceRemoved->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    }
#endif

    // DXGI Debug Component だけがない場合は通常 Factory で継続する
    const UINT factoryFlags = hasDebugLayer ? DXGI_CREATE_FACTORY_DEBUG : 0;
    HRESULT result = CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&context->m_factory));
    if (result == DXGI_ERROR_SDK_COMPONENT_MISSING && factoryFlags != 0)
    {
        result = CreateDXGIFactory2(0, IID_PPV_ARGS(&context->m_factory));
    }
    if (FAILED(result))
    {
        return DeviceResult::failure(gpu_error("CreateDXGIFactory2", result));
    }
    constexpr char k_factoryName[] = "CueEngine DXGI Factory";
    result = context->m_factory->SetPrivateData(WKPDID_D3DDebugObjectName, sizeof(k_factoryName) - 1, k_factoryName);
    if (FAILED(result))
    {
        report_error("DX12RenderDevice", gpu_error("IDXGIFactory.SetPrivateData", result),
                     DiagnosticSeverity::Warning);
    }

    // Hardware Adapter を高性能順で調べ、Device を作れた候補だけ採用する
    if (a_selection == AdapterSelection::HardwarePreferred)
    {
        for (UINT index = 0;; ++index)
        {
            Microsoft::WRL::ComPtr<IDXGIAdapter1> candidate;
            result = context->m_factory->EnumAdapterByGpuPreference(index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                                      IID_PPV_ARGS(&candidate));
            if (result == DXGI_ERROR_NOT_FOUND)
            {
                break;
            }
            if (FAILED(result))
            {
                return DeviceResult::failure(gpu_error("IDXGIFactory.EnumAdapterByGpuPreference", result));
            }
            DXGI_ADAPTER_DESC1 desc{};
            result = candidate->GetDesc1(&desc);
            if (FAILED(result))
            {
                return DeviceResult::failure(gpu_error("IDXGIAdapter.GetDesc1", result));
            }
            if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0 &&
                SUCCEEDED(create_device(candidate.Get(), context->m_device, context->m_featureLevel)))
            {
                context->m_adapter = std::move(candidate);
                break;
            }
        }
    }

    if (!context->m_adapter)
    {
        // Hardware が利用できない環境だけ WARP を試す
        result = context->m_factory->EnumWarpAdapter(IID_PPV_ARGS(&context->m_adapter));
        if (FAILED(result))
        {
            return DeviceResult::failure(gpu_error("IDXGIFactory.EnumWarpAdapter", result));
        }
        result = create_device(context->m_adapter.Get(), context->m_device, context->m_featureLevel);
        if (FAILED(result))
        {
            return DeviceResult::failure(gpu_error("D3D12CreateDevice", result));
        }
        context->m_isWarp = true;
    }

    // 選択結果を PIX と Debug Layer で識別できる名前にする
    constexpr std::string_view k_warpName = "CueEngine WARP Adapter";
    constexpr std::string_view k_hardwareName = "CueEngine Hardware Adapter";
    const std::string_view adapterName = context->m_isWarp ? k_warpName : k_hardwareName;
    result = context->m_adapter->SetPrivateData(WKPDID_D3DDebugObjectName,
                                                static_cast<UINT>(adapterName.size()), adapterName.data());
    if (FAILED(result))
    {
        report_error("DX12RenderDevice", gpu_error("IDXGIAdapter.SetPrivateData", result),
                     DiagnosticSeverity::Warning);
    }
    result = context->m_device->SetName(L"CueEngine DX12 Device");
    if (FAILED(result))
    {
        report_error("DX12RenderDevice", gpu_error("ID3D12Device.SetName", result),
                     DiagnosticSeverity::Warning);
    }

#if defined(_DEBUG) && !defined(CUE_SHIPPING)
    if (hasDebugLayer)
    {
        Microsoft::WRL::ComPtr<ID3D12InfoQueue> infoQueue;
        result = context->m_device.As(&infoQueue);
        if (result != E_NOINTERFACE && FAILED(result))
        {
            return DeviceResult::failure(gpu_error("ID3D12Device.QueryInterface.InfoQueue", result));
        }
        if (infoQueue)
        {
            // GPU 検証の Warning も見落とさず、発生箇所で停止する
            result = infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, true);
            if (FAILED(result))
            {
                return DeviceResult::failure(gpu_error("ID3D12InfoQueue.SetBreakOnSeverity.Corruption", result));
            }
            result = infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, true);
            if (FAILED(result))
            {
                return DeviceResult::failure(gpu_error("ID3D12InfoQueue.SetBreakOnSeverity.Error", result));
            }
            result = infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, true);
            if (FAILED(result))
            {
                return DeviceResult::failure(gpu_error("ID3D12InfoQueue.SetBreakOnSeverity.Warning", result));
            }
            result = infoQueue->SetBreakOnID(D3D12_MESSAGE_ID_FENCE_ZERO_WAIT, true);
            if (FAILED(result))
            {
                return DeviceResult::failure(gpu_error("ID3D12InfoQueue.SetBreakOnID.FENCE_ZERO_WAIT", result));
            }
        }
    }
#endif

    return DeviceResult::success(std::move(context));
}

/// @brief 本体の生存中だけ Device を借用する
ID3D12Device* DX12RenderDevice::device() const noexcept
{
    return m_device.Get();
}

/// @brief 本体の生存中だけ Factory を借用する
IDXGIFactory6* DX12RenderDevice::factory() const noexcept
{
    return m_factory.Get();
}

/// @brief 本体の生存中だけ Adapter を借用する
IDXGIAdapter1* DX12RenderDevice::adapter() const noexcept
{
    return m_adapter.Get();
}

/// @brief 選択した Adapter が WARP か返す
bool DX12RenderDevice::is_warp() const noexcept
{
    return m_isWarp;
}

/// @brief 共通契約で Software Adapter の選択結果を返す
bool DX12RenderDevice::is_software_adapter() const noexcept
{
    return m_isWarp;
}

/// @brief Device 生成時に採用した Feature Level を返す
D3D_FEATURE_LEVEL DX12RenderDevice::feature_level() const noexcept
{
    return m_featureLevel;
}
} // namespace cue::dx12
