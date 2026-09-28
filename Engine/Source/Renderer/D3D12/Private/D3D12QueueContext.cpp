#include "D3D12QueueContext.h"

#include <utility>

namespace cue::detail
{
namespace
{
constexpr DWORD k_gpuWaitMilliseconds = 10'000;
} // namespace

/// @brief 待機 Event を閉じる
D3D12QueueContext::~D3D12QueueContext()
{
    if (m_fenceEvent)
    {
        CloseHandle(m_fenceEvent);
    }
}

/// @brief Device より短い寿命の Direct Queue を生成する
Result<std::unique_ptr<D3D12QueueContext>> D3D12QueueContext::create(D3D12DeviceContext& a_device)
{
    using QueueResult = Result<std::unique_ptr<D3D12QueueContext>>;
    auto context = std::make_unique<D3D12QueueContext>();
    context->m_device = a_device.device();

    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    HRESULT result = context->m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&context->m_queue));
    if (FAILED(result))
    {
        return QueueResult::failure(gpu_error("ID3D12Device.CreateCommandQueue", result));
    }
    result = context->m_queue->SetName(L"CueEngine Direct Queue");
    if (FAILED(result))
    {
        return QueueResult::failure(gpu_error("ID3D12CommandQueue.SetName", result));
    }
    result = context->m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&context->m_fence));
    if (FAILED(result))
    {
        return QueueResult::failure(gpu_error("ID3D12Device.CreateFence", result));
    }
    result = context->m_fence->SetName(L"CueEngine Frame Fence");
    if (FAILED(result))
    {
        return QueueResult::failure(gpu_error("ID3D12Fence.SetName", result));
    }
    context->m_fenceEvent = CreateEventW(nullptr, false, false, nullptr);
    if (!context->m_fenceEvent)
    {
        return QueueResult::failure({ErrorCategory::PlatformFailure, "CreateEventW.GpuFence", GetLastError()});
    }
    return QueueResult::success(std::move(context));
}

/// @brief 指定 Fence 値の GPU 完了を確認する
Result<void> D3D12QueueContext::wait_for(std::uint64_t a_value) const
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

/// @brief Queue の先行する全処理を待つ
Result<void> D3D12QueueContext::wait_idle()
{
    auto valueResult = signal();
    if (!valueResult.has_value())
    {
        return Result<void>::failure(*valueResult.try_error());
    }
    return wait_for(valueResult.take_value());
}

/// @brief Queue 上の現在位置を Fence に記録する
Result<std::uint64_t> D3D12QueueContext::signal()
{
    const std::uint64_t value = m_nextFenceValue++;
    const HRESULT result = m_queue->Signal(m_fence.Get(), value);
    if (FAILED(result))
    {
        return Result<std::uint64_t>::failure(gpu_error("ID3D12CommandQueue.Signal", result));
    }
    return Result<std::uint64_t>::success(value);
}

/// @brief Queue Context の存続中だけ Queue を借用する
ID3D12CommandQueue* D3D12QueueContext::queue() const noexcept
{
    return m_queue.Get();
}
} // namespace cue::detail
