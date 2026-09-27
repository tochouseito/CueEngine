#include "D3D12DeviceContext.h"

#include <utility>

namespace cue::detail
{
namespace
{
constexpr DWORD k_gpuWaitMilliseconds = 10'000;
} // namespace

/// @brief Event を閉じてから COM 資源を解放する
D3D12DeviceContext::~D3D12DeviceContext()
{
    if (m_fenceEvent)
    {
        CloseHandle(m_fenceEvent);
    }
}

/// @brief Hardware 優先または明示 WARP で GPU の実行基盤を生成する
Result<std::unique_ptr<D3D12DeviceContext>> D3D12DeviceContext::create(bool a_useWarp)
{
    using DeviceResult = Result<std::unique_ptr<D3D12DeviceContext>>;
    auto context = std::make_unique<D3D12DeviceContext>();
    context->m_isWarp = a_useWarp;

#if defined(_DEBUG) && !defined(CUE_SHIPPING)
    // Device 作成前に Debug Layer を有効にする
    Microsoft::WRL::ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
    {
        debug->EnableDebugLayer();
    }
#endif

    HRESULT result = CreateDXGIFactory2(0, IID_PPV_ARGS(&context->m_factory));
    if (FAILED(result))
    {
        return DeviceResult::failure(gpu_error("CreateDXGIFactory2", result));
    }

    if (a_useWarp)
    {
        result = context->m_factory->EnumWarpAdapter(IID_PPV_ARGS(&context->m_adapter));
        if (FAILED(result))
        {
            return DeviceResult::failure(gpu_error("IDXGIFactory.EnumWarpAdapter", result));
        }
    }
    else
    {
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
                SUCCEEDED(D3D12CreateDevice(candidate.Get(), D3D_FEATURE_LEVEL_11_0,
                                            __uuidof(ID3D12Device), nullptr)))
            {
                context->m_adapter = std::move(candidate);
                break;
            }
        }
        if (!context->m_adapter)
        {
            return DeviceResult::failure({ErrorCategory::PlatformFailure, "D3D12Renderer.noHardwareAdapter",
                                          static_cast<std::int64_t>(DXGI_ERROR_NOT_FOUND)});
        }
    }

    result = D3D12CreateDevice(context->m_adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                               IID_PPV_ARGS(&context->m_device));
    if (FAILED(result))
    {
        return DeviceResult::failure(gpu_error("D3D12CreateDevice", result));
    }

    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    result = context->m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&context->m_queue));
    if (FAILED(result))
    {
        return DeviceResult::failure(gpu_error("ID3D12Device.CreateCommandQueue", result));
    }
    result = context->m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&context->m_fence));
    if (FAILED(result))
    {
        return DeviceResult::failure(gpu_error("ID3D12Device.CreateFence", result));
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
} // namespace cue::detail
