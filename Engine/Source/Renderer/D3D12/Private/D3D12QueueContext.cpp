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

/// @brief Device より短い寿命の指定種類の Queue を生成する
Result<std::unique_ptr<D3D12QueueContext>> D3D12QueueContext::create(D3D12DeviceContext& a_device,
                                                                       GpuQueueType a_type)
{
    using QueueResult = Result<std::unique_ptr<D3D12QueueContext>>;
    auto context = std::make_unique<D3D12QueueContext>();
    context->m_device = a_device.device();
    context->m_type = a_type;

    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    switch (a_type)
    {
    case GpuQueueType::Graphics:
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        break;
    case GpuQueueType::Compute:
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
        break;
    case GpuQueueType::Copy:
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_COPY;
        break;
    default:
        return QueueResult::failure({ErrorCategory::InvalidArgument, "D3D12QueueContext.create.type"});
    }
    HRESULT result = context->m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&context->m_queue));
    if (FAILED(result))
    {
        return QueueResult::failure(gpu_error("ID3D12Device.CreateCommandQueue", result));
    }
    const wchar_t* queueName = a_type == GpuQueueType::Graphics ? L"CueEngine Graphics Queue"
                              : a_type == GpuQueueType::Compute  ? L"CueEngine Compute Queue"
                                                                  : L"CueEngine Copy Queue";
    result = context->m_queue->SetName(queueName);
    if (FAILED(result))
    {
        return QueueResult::failure(gpu_error("ID3D12CommandQueue.SetName", result));
    }
    result = context->m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&context->m_fence));
    if (FAILED(result))
    {
        return QueueResult::failure(gpu_error("ID3D12Device.CreateFence", result));
    }
    const wchar_t* fenceName = a_type == GpuQueueType::Graphics ? L"CueEngine Graphics Fence"
                              : a_type == GpuQueueType::Compute  ? L"CueEngine Compute Fence"
                                                                  : L"CueEngine Copy Fence";
    result = context->m_fence->SetName(fenceName);
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
    if (!has_issued(a_value))
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "D3D12QueueContext.wait_for.value"});
    }
    const UINT64 completed = m_fence->GetCompletedValue();
    if (completed == UINT64_MAX)
    {
        return Result<void>::failure(gpu_error("ID3D12Device.GetDeviceRemovedReason",
                                               m_device->GetDeviceRemovedReason()));
    }
    if (completed >= a_value)
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
    if (m_fence->GetCompletedValue() == UINT64_MAX)
    {
        return Result<void>::failure(gpu_error("ID3D12Device.GetDeviceRemovedReason",
                                               m_device->GetDeviceRemovedReason()));
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
    const std::uint64_t value = m_nextFenceValue;
    const HRESULT result = m_queue->Signal(m_fence.Get(), value);
    if (FAILED(result))
    {
        return Result<std::uint64_t>::failure(gpu_error("ID3D12CommandQueue.Signal", result));
    }
    ++m_nextFenceValue;
    return Result<std::uint64_t>::success(value);
}

/// @brief Queue Context の存続中だけ Queue を借用する
ID3D12CommandQueue* D3D12QueueContext::queue() const noexcept
{
    return m_queue.Get();
}

/// @brief 発行元 Fence が指定値へ進むまで受信 Queue の後続処理を停止する
Result<void> D3D12QueueContext::wait_on(const D3D12QueueContext& a_source, std::uint64_t a_value)
{
    if (m_device != a_source.m_device || !a_source.has_issued(a_value))
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "D3D12QueueContext.wait_on"});
    }
    const HRESULT result = m_queue->Wait(a_source.m_fence.Get(), a_value);
    if (FAILED(result))
    {
        return Result<void>::failure(gpu_error("ID3D12CommandQueue.Wait", result));
    }
    return Result<void>::success();
}

/// @brief 発行済みの正の Fence 値だけを受け入れる
bool D3D12QueueContext::has_issued(std::uint64_t a_value) const noexcept
{
    return a_value != 0 && a_value < m_nextFenceValue;
}

/// @brief 再利用できる Command Context を探すため Fence を確認する
bool D3D12QueueContext::is_complete(std::uint64_t a_value) const noexcept
{
    if (a_value == 0)
    {
        return true;
    }
    const UINT64 completed = m_fence->GetCompletedValue();
    return has_issued(a_value) && completed != UINT64_MAX && completed >= a_value;
}

/// @brief Queue と Command Pool の適合を検査するための種別を返す
GpuQueueType D3D12QueueContext::type() const noexcept
{
    return m_type;
}
} // namespace cue::detail
