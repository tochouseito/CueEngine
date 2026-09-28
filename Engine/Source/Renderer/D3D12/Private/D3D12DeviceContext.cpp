#include "D3D12DeviceContext.h"

#include <string_view>
#include <utility>

#include <d3d12sdklayers.h>
#include <d3dcommon.h>

namespace cue::detail
{
namespace
{
constexpr DWORD k_gpuWaitMilliseconds = 10'000;
constexpr D3D_FEATURE_LEVEL k_featureLevels[] = {
    D3D_FEATURE_LEVEL_12_2,
    D3D_FEATURE_LEVEL_12_1,
    D3D_FEATURE_LEVEL_12_0,
    D3D_FEATURE_LEVEL_11_1,
    D3D_FEATURE_LEVEL_11_0,
};

/// @brief Adapter が対応する最高の機能レベルで Device を生成する
HRESULT create_device(IDXGIAdapter1* a_adapter, Microsoft::WRL::ComPtr<ID3D12Device>& a_device,
                      D3D_FEATURE_LEVEL& a_featureLevel)
{
    HRESULT result = E_FAIL;
    for (D3D_FEATURE_LEVEL level : k_featureLevels)
    {
        result = D3D12CreateDevice(a_adapter, level, IID_PPV_ARGS(&a_device));
        if (SUCCEEDED(result))
        {
            a_featureLevel = level;
            return result;
        }
    }
    return result;
}
} // namespace

/// @brief Event を閉じてから COM 資源を解放する
D3D12DeviceContext::~D3D12DeviceContext()
{
    if (m_fenceEvent)
    {
        CloseHandle(m_fenceEvent);
    }
}

/// @brief Hardware を優先し、対応 Adapter がなければ WARP で GPU の実行基盤を生成する
Result<std::unique_ptr<D3D12DeviceContext>> D3D12DeviceContext::create()
{
    using DeviceResult = Result<std::unique_ptr<D3D12DeviceContext>>;
    auto context = std::make_unique<D3D12DeviceContext>();

#if defined(_DEBUG) && !defined(CUE_SHIPPING)

    // Debug Layer の有効化
    Microsoft::WRL::ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
    {
        debug->EnableDebugLayer();

        // GPU Validation の有効化
        Microsoft::WRL::ComPtr<ID3D12Debug1> gpuValidation;
        if (SUCCEEDED(debug.As(&gpuValidation)))
        {
            gpuValidation->SetEnableGPUBasedValidation(true);
        }
    }

    // DRED の有効化
    Microsoft::WRL::ComPtr<ID3D12DeviceRemovedExtendedDataSettings> deviceRemoved;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&deviceRemoved))))
    {
        deviceRemoved->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        deviceRemoved->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    }
#endif

    // DXGI Factory の生成
    HRESULT result = CreateDXGIFactory2(0, IID_PPV_ARGS(&context->m_factory));
    if (FAILED(result))
    {
        return DeviceResult::failure(gpu_error("CreateDXGIFactory2", result));
    }
    constexpr char k_factoryName[] = "CueEngine DXGI Factory";
    result = context->m_factory->SetPrivateData(WKPDID_D3DDebugObjectName, sizeof(k_factoryName) - 1, k_factoryName);
    if (FAILED(result))
    {
        return DeviceResult::failure(gpu_error("IDXGIFactory.SetPrivateData", result));
    }

    // D3D12 対応の Hardware Adapter だけを高性能順で採用する
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
    if (!context->m_adapter)
    {
        // 使用可能な Hardware Device がない場合だけ WARP を試し、列挙自体の失敗は隠さない
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
    constexpr std::string_view k_warpAdapterName = "CueEngine WARP Adapter";
    constexpr std::string_view k_hardwareAdapterName = "CueEngine Hardware Adapter";
    const std::string_view adapterName = context->m_isWarp ? k_warpAdapterName : k_hardwareAdapterName;
    result = context->m_adapter->SetPrivateData(WKPDID_D3DDebugObjectName,
                                                static_cast<UINT>(adapterName.size()), adapterName.data());
    if (FAILED(result))
    {
        return DeviceResult::failure(gpu_error("IDXGIAdapter.SetPrivateData", result));
    }

    result = context->m_device->SetName(L"CueEngine D3D12 Device");
    if (FAILED(result))
    {
        return DeviceResult::failure(gpu_error("ID3D12Device.SetName", result));
    }

    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    result = context->m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&context->m_queue));
    if (FAILED(result))
    {
        return DeviceResult::failure(gpu_error("ID3D12Device.CreateCommandQueue", result));
    }
    result = context->m_queue->SetName(L"CueEngine Direct Queue");
    if (FAILED(result))
    {
        return DeviceResult::failure(gpu_error("ID3D12CommandQueue.SetName", result));
    }
    result = context->m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&context->m_fence));
    if (FAILED(result))
    {
        return DeviceResult::failure(gpu_error("ID3D12Device.CreateFence", result));
    }
    result = context->m_fence->SetName(L"CueEngine Frame Fence");
    if (FAILED(result))
    {
        return DeviceResult::failure(gpu_error("ID3D12Fence.SetName", result));
    }
    context->m_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!context->m_fenceEvent)
    {
        return DeviceResult::failure({ErrorCategory::PlatformFailure, "CreateEventW.GpuFence", GetLastError()});
    }
    return DeviceResult::success(std::move(context));
}

/// @brief 指定 Fence 値の GPU 完了を確認する
Result<void> D3D12DeviceContext::wait_for(std::uint64_t a_value) const
{
    if (m_fence->GetCompletedValue() >= a_value)
    {
        return Result<void>::success();
    }
    const HRESULT eventResult = m_fence->SetEventOnCompletion(a_value, m_fenceEvent);
    if (FAILED(eventResult))
    {
        return Result<void>::failure(gpu_error("ID3D12Fence.SetEventOnCompletion", eventResult));
    }
    const DWORD waitResult = WaitForSingleObject(m_fenceEvent, k_gpuWaitMilliseconds);
    if (waitResult != WAIT_OBJECT_0)
    {
        // Device Lost 時は待機結果より Device の原因を優先する
        const HRESULT removedReason = m_device->GetDeviceRemovedReason();
        if (FAILED(removedReason))
        {
            return Result<void>::failure(gpu_error("ID3D12Device.GetDeviceRemovedReason", removedReason));
        }
        const DWORD errorCode = waitResult == WAIT_FAILED ? GetLastError() : waitResult;
        return Result<void>::failure({ErrorCategory::PlatformFailure, "WaitForSingleObject.GpuFence",
                                      static_cast<std::int64_t>(errorCode)});
    }
    return Result<void>::success();
}

/// @brief Direct Queue に完了印を入れ、先行する全処理を待つ
Result<void> D3D12DeviceContext::wait_idle()
{
    auto valueResult = signal();
    if (!valueResult.has_value())
    {
        return Result<void>::failure(*valueResult.try_error());
    }
    return wait_for(valueResult.take_value());
}

/// @brief Direct Queue 上の現在位置を Fence に記録する
Result<std::uint64_t> D3D12DeviceContext::signal()
{
    const std::uint64_t value = m_nextFenceValue++;
    const HRESULT result = m_queue->Signal(m_fence.Get(), value);
    if (FAILED(result))
    {
        return Result<std::uint64_t>::failure(gpu_error("ID3D12CommandQueue.Signal", result));
    }
    return Result<std::uint64_t>::success(value);
}

/// @brief Presentation と Frame Context が存続する間だけ Device を借用する
ID3D12Device* D3D12DeviceContext::device() const noexcept
{
    return m_device.Get();
}

/// @brief Presentation と Frame Context が存続する間だけ Queue を借用する
ID3D12CommandQueue* D3D12DeviceContext::queue() const noexcept
{
    return m_queue.Get();
}

/// @brief Presentation 作成中だけ Factory を借用する
IDXGIFactory6* D3D12DeviceContext::factory() const noexcept
{
    return m_factory.Get();
}

/// @brief 選択した Adapter の種類を診断する
bool D3D12DeviceContext::is_warp() const noexcept
{
    return m_isWarp;
}

/// @brief Device 生成時に選択した最高の機能レベルを返す
D3D_FEATURE_LEVEL D3D12DeviceContext::feature_level() const noexcept
{
    return m_featureLevel;
}
} // namespace cue::detail
