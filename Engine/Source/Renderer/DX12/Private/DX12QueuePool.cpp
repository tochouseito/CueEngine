#include <DX12/DX12QueuePool.h>

#include <limits>
#include <optional>
#include <string>
#include <utility>

#include <Windows.h>

#include <DX12/DX12RenderDevice.h>
#include <Platform/Diagnostics.h>

namespace cue::dx12
{
namespace
{
/// @brief RHI の種類を D3D12 Queue の種類へ対応させる
D3D12_COMMAND_LIST_TYPE command_list_type(QueueType a_type)
{
    switch (a_type)
    {
    case QueueType::Graphics:
        return D3D12_COMMAND_LIST_TYPE_DIRECT;
    case QueueType::Compute:
        return D3D12_COMMAND_LIST_TYPE_COMPUTE;
    case QueueType::Copy:
        return D3D12_COMMAND_LIST_TYPE_COPY;
    }
    return D3D12_COMMAND_LIST_TYPE_DIRECT;
}

/// @brief Native の失敗値と操作名を共通 Error に保持する
Error queue_error(const char* a_operation, HRESULT a_result)
{
    return {ErrorCategory::PlatformFailure, a_operation, static_cast<std::int64_t>(a_result)};
}
} // namespace

/// @brief create の内部でだけ Queue を持たない中間状態を作る
DX12GpuCommandQueue::DX12GpuCommandQueue(CreateToken) noexcept
{
}

/// @brief Queue と Fence の部分生成が外へ漏れないよう順に所有する
Result<std::unique_ptr<DX12GpuCommandQueue>> DX12GpuCommandQueue::create(ID3D12Device& a_device, QueueType a_type,
                                                                          std::uint32_t a_index)
{
    using QueueResult = Result<std::unique_ptr<DX12GpuCommandQueue>>;
    if (a_type != QueueType::Graphics && a_type != QueueType::Compute && a_type != QueueType::Copy)
    {
        return QueueResult::failure({ErrorCategory::InvalidArgument, "DX12GpuCommandQueue.create.type"});
    }

    auto queue = std::make_unique<DX12GpuCommandQueue>(CreateToken{});
    queue->m_device = &a_device;
    queue->m_type = a_type;

    D3D12_COMMAND_QUEUE_DESC descriptor{};
    descriptor.Type = command_list_type(a_type);
    const HRESULT queueResult = a_device.CreateCommandQueue(&descriptor, IID_PPV_ARGS(&queue->m_queue));
    if (FAILED(queueResult))
    {
        return QueueResult::failure(queue_error("ID3D12Device.CreateCommandQueue", queueResult));
    }

    const HRESULT fenceResult = a_device.CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&queue->m_fence));
    if (FAILED(fenceResult))
    {
        return QueueResult::failure(queue_error("ID3D12Device.CreateFence", fenceResult));
    }

    // PIX と Debug Layer で同種類の Queue を区別できるよう、種類と Pool 内番号を含める
    const wchar_t* typeName = a_type == QueueType::Graphics ? L"Graphics" :
                              a_type == QueueType::Compute ? L"Compute" : L"Copy";
    std::wstring namePrefix = L"CueEngine DX12 ";
    namePrefix += typeName;
    namePrefix += L" ";
    namePrefix += std::to_wstring(a_index);
    const std::wstring queueName = namePrefix + L" Command Queue";
    const HRESULT queueNameResult = queue->m_queue->SetName(queueName.c_str());
    if (FAILED(queueNameResult))
    {
        report_error("DX12GpuCommandQueue", queue_error("ID3D12CommandQueue.SetName", queueNameResult),
                     DiagnosticSeverity::Warning);
    }
    const std::wstring fenceName = namePrefix + L" Fence";
    const HRESULT fenceNameResult = queue->m_fence->SetName(fenceName.c_str());
    if (FAILED(fenceNameResult))
    {
        report_error("DX12GpuCommandQueue", queue_error("ID3D12Fence.SetName", fenceNameResult),
                     DiagnosticSeverity::Warning);
    }

    queue->m_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!queue->m_fenceEvent)
    {
        return QueueResult::failure(
            {ErrorCategory::PlatformFailure, "CreateEventW", static_cast<std::int64_t>(GetLastError())});
    }
    return QueueResult::success(std::move(queue));
}

/// @brief CPU 待機 Event を先に閉じてから COM Resource を解放する
DX12GpuCommandQueue::~DX12GpuCommandQueue()
{
    if (m_fenceEvent)
    {
        CloseHandle(m_fenceEvent);
    }
}

/// @brief Queue の作成時に確定した種類を返す
QueueType DX12GpuCommandQueue::type() const noexcept
{
    return m_type;
}

/// @brief 正常に Signal できた値だけを次の待機対象として記録する
Result<std::uint64_t> DX12GpuCommandQueue::signal()
{
    std::lock_guard lock(m_mutex);
    return signal_locked();
}

/// @brief Command 投入と Fence 発行の間に別の投入を割り込ませない
Result<std::uint64_t> DX12GpuCommandQueue::submit(std::span<ID3D12CommandList* const> a_lists)
{
    if (a_lists.empty() || a_lists.size() > (std::numeric_limits<UINT>::max)())
    {
        return Result<std::uint64_t>::failure({ErrorCategory::InvalidArgument, "DX12GpuCommandQueue.submit.count"});
    }
    for (ID3D12CommandList* list : a_lists)
    {
        if (!list || list->GetType() != command_list_type(m_type))
        {
            return Result<std::uint64_t>::failure({ErrorCategory::InvalidArgument, "DX12GpuCommandQueue.submit.type"});
        }
    }

    std::lock_guard lock(m_mutex);
    if (m_fenceValue.load() == (std::numeric_limits<std::uint64_t>::max)())
    {
        return Result<std::uint64_t>::failure({ErrorCategory::InvalidState, "DX12GpuCommandQueue.submit.overflow"});
    }
    m_queue->ExecuteCommandLists(static_cast<UINT>(a_lists.size()), a_lists.data());
    // Signal 失敗時も作業自体は投入済みなので、呼出側は Resource を保持して停止する
    return signal_locked();
}

/// @brief Queue の排他保持中に次の Fence 値を発行する
Result<std::uint64_t> DX12GpuCommandQueue::signal_locked()
{
    const std::uint64_t currentValue = m_fenceValue.load();
    if (currentValue == (std::numeric_limits<std::uint64_t>::max)())
    {
        return Result<std::uint64_t>::failure({ErrorCategory::InvalidState, "DX12GpuCommandQueue.signal.overflow"});
    }
    const std::uint64_t nextValue = currentValue + 1;
    const HRESULT result = m_queue->Signal(m_fence.Get(), nextValue);
    if (FAILED(result))
    {
        return Result<std::uint64_t>::failure(queue_error("ID3D12CommandQueue.Signal", result));
    }
    m_fenceValue.store(nextValue);
    return Result<std::uint64_t>::success(nextValue);
}

/// @brief 未発行値への無限待機を拒否してから完了通知を待つ
Result<void> DX12GpuCommandQueue::wait_for_fence(std::uint64_t a_fenceValue)
{
    if (a_fenceValue > m_fenceValue.load())
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "DX12GpuCommandQueue.wait_for_fence.value"});
    }
    if (a_fenceValue == 0)
    {
        return Result<void>::success();
    }
    // 一つの自動 Reset Event を複数の CPU 待機者に同時登録しない
    std::lock_guard waitLock(m_waitMutex);
    const std::uint64_t completed = m_fence->GetCompletedValue();
    if (completed == (std::numeric_limits<std::uint64_t>::max)())
    {
        return Result<void>::failure(queue_error("ID3D12Device.GetDeviceRemovedReason",
                                                  m_device->GetDeviceRemovedReason()));
    }
    if (completed >= a_fenceValue)
    {
        return Result<void>::success();
    }

    const HRESULT result = m_fence->SetEventOnCompletion(a_fenceValue, m_fenceEvent);
    if (FAILED(result))
    {
        return Result<void>::failure(queue_error("ID3D12Fence.SetEventOnCompletion", result));
    }
    const DWORD waitResult = WaitForSingleObject(m_fenceEvent, INFINITE);
    if (waitResult != WAIT_OBJECT_0)
    {
        return Result<void>::failure({ErrorCategory::PlatformFailure, "WaitForSingleObject",
                                      static_cast<std::int64_t>(GetLastError())});
    }
    if (m_fence->GetCompletedValue() == (std::numeric_limits<std::uint64_t>::max)())
    {
        return Result<void>::failure(queue_error("ID3D12Device.GetDeviceRemovedReason",
                                                  m_device->GetDeviceRemovedReason()));
    }
    return Result<void>::success();
}

/// @brief Device Lost の Sentinel を完了値として扱わない
bool DX12GpuCommandQueue::is_fence_complete(std::uint64_t a_fenceValue) const noexcept
{
    if (a_fenceValue > m_fenceValue.load())
    {
        return false;
    }
    const std::uint64_t completed = m_fence->GetCompletedValue();
    return completed != (std::numeric_limits<std::uint64_t>::max)() && completed >= a_fenceValue;
}

/// @brief 発行済みの別 Queue の Fence だけを GPU 側で待つ
Result<void> DX12GpuCommandQueue::wait_for_queue(IQueueContext& a_queue, std::uint64_t a_fenceValue)
{
    auto* other = dynamic_cast<DX12GpuCommandQueue*>(&a_queue);
    if (!other || other->m_device.Get() != m_device.Get() || a_fenceValue == 0 ||
        a_fenceValue > other->m_fenceValue.load())
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "DX12GpuCommandQueue.wait_for_queue"});
    }
    std::lock_guard lock(m_mutex);
    const HRESULT result = m_queue->Wait(other->m_fence.Get(), a_fenceValue);
    if (FAILED(result))
    {
        return Result<void>::failure(queue_error("ID3D12CommandQueue.Wait", result));
    }
    return Result<void>::success();
}

/// @brief Timestamp の基準を Queue から取得し、失敗理由を返す
Result<std::uint64_t> DX12GpuCommandQueue::get_timestamp_frequency() const
{
    std::uint64_t frequency = 0;
    const HRESULT result = m_queue->GetTimestampFrequency(&frequency);
    if (FAILED(result))
    {
        return Result<std::uint64_t>::failure(queue_error("ID3D12CommandQueue.GetTimestampFrequency", result));
    }
    return Result<std::uint64_t>::success(frequency);
}

/// @brief 最終投入点を Signal して GPU の作業完了を確認する
Result<void> DX12GpuCommandQueue::wait_idle()
{
    auto signalResult = signal();
    if (!signalResult.has_value())
    {
        return Result<void>::failure(*signalResult.try_error());
    }
    return wait_for_fence(signalResult.take_value());
}

/// @brief Pool と Lease の間で Queue と貸出状態を共有する
struct DX12QueuePool::State
{
    std::array<std::unique_ptr<DX12GpuCommandQueue>, k_queueCount> queues{};
    std::array<bool, k_queueCount> borrowed{};
    std::mutex mutex;
    bool stopping = false;
    bool waitedIdle = false;

    /// @brief 最後の Lease が消えた時も GPU 完了を待ってから Queue を解放する
    ~State()
    {
        if (!waitedIdle)
        {
            auto result = wait_idle();
            if (!result.has_value())
            {
                report_error("DX12QueuePool.State", *result.try_error(), DiagnosticSeverity::Error);
            }
        }
    }

    /// @brief 部分生成中の Queue も含め、存在する Queue の GPU 完了を待つ
    [[nodiscard]] Result<void> wait_idle()
    {
        std::optional<Error> firstError;
        for (const auto& queue : queues)
        {
            if (!queue)
            {
                continue;
            }
            auto result = queue->wait_idle();
            if (!result.has_value() && !firstError)
            {
                firstError = *result.try_error();
            }
        }
        if (firstError)
        {
            return Result<void>::failure(std::move(*firstError));
        }
        waitedIdle = true;
        return Result<void>::success();
    }
};

/// @brief create の内部でだけ空の Pool を構築する
DX12QueuePool::DX12QueuePool(CreateToken) noexcept
{
}

/// @brief 停止を試み、残る Lease の解放後に Queue を破棄する
DX12QueuePool::~DX12QueuePool()
{
    auto result = shutdown();
    if (!result.has_value() && result.try_error()->category != ErrorCategory::InvalidState)
    {
        report_error("DX12QueuePool.shutdown", *result.try_error(), DiagnosticSeverity::Error);
    }
}

/// @brief 全 Queue を先に作り、失敗時は生成済み Queue を自動解放する
Result<std::unique_ptr<DX12QueuePool>> DX12QueuePool::create(DX12RenderDevice& a_device)
{
    using PoolResult = Result<std::unique_ptr<DX12QueuePool>>;
    if (!a_device.device())
    {
        return PoolResult::failure({ErrorCategory::InvalidArgument, "DX12QueuePool.create.device"});
    }
    auto pool = std::make_unique<DX12QueuePool>(CreateToken{});
    pool->m_state = std::make_shared<State>();
    for (std::size_t index = 0; index < k_queueCount; ++index)
    {
        const QueueType type = index < k_graphicsCount ? QueueType::Graphics
                               : index < k_graphicsCount + k_computeCount ? QueueType::Compute
                                                                          : QueueType::Copy;
        const std::uint32_t typeIndex = type == QueueType::Graphics ? static_cast<std::uint32_t>(index)
                                        : type == QueueType::Compute
                                            ? static_cast<std::uint32_t>(index - k_graphicsCount)
                                            : static_cast<std::uint32_t>(index - k_graphicsCount - k_computeCount);
        auto queueResult = DX12GpuCommandQueue::create(*a_device.device(), type, typeIndex);
        if (!queueResult.has_value())
        {
            return PoolResult::failure(*queueResult.try_error());
        }
        pool->m_state->queues[index] = queueResult.take_value();
    }
    return PoolResult::success(std::move(pool));
}

/// @brief 共有状態を保持する Lease を作り、破棄時に自動返却する
Result<queueLease> DX12QueuePool::acquire(QueueType a_type)
{
    if (a_type != QueueType::Graphics && a_type != QueueType::Compute && a_type != QueueType::Copy)
    {
        return Result<queueLease>::failure({ErrorCategory::InvalidArgument, "DX12QueuePool.acquire.type"});
    }
    const auto state = m_state;
    std::lock_guard lock(state->mutex);
    if (state->stopping)
    {
        return Result<queueLease>::failure({ErrorCategory::InvalidState, "DX12QueuePool.acquire.stopping"});
    }
    for (std::size_t index = 0; index < k_queueCount; ++index)
    {
        if (!state->borrowed[index] && state->queues[index]->type() == a_type)
        {
            queueLease lease(state->queues[index].get(), [state, index](IQueueContext*) {
                std::lock_guard leaseLock(state->mutex);
                state->borrowed[index] = false;
            });
            state->borrowed[index] = true;
            state->waitedIdle = false;
            return Result<queueLease>::success(std::move(lease));
        }
    }
    return Result<queueLease>::failure({ErrorCategory::InvalidState, "DX12QueuePool.acquire.capacity"});
}

/// @brief Queue を破棄する前に全種類の GPU 作業を完了させる
Result<void> DX12QueuePool::wait_idle()
{
    const auto state = m_state;
    std::lock_guard lock(state->mutex);
    for (bool borrowed : state->borrowed)
    {
        if (borrowed)
        {
            return Result<void>::failure({ErrorCategory::InvalidState, "DX12QueuePool.wait_idle.borrowed"});
        }
    }
    return state->wait_idle();
}

/// @brief 新規貸出を停止し、借用中なら寿命を Lease に引き継ぐ
Result<void> DX12QueuePool::shutdown()
{
    const auto state = m_state;
    if (!state)
    {
        return Result<void>::success();
    }
    std::lock_guard lock(state->mutex);
    state->stopping = true;
    for (bool borrowed : state->borrowed)
    {
        if (borrowed)
        {
            return Result<void>::failure({ErrorCategory::InvalidState, "DX12QueuePool.shutdown.borrowed"});
        }
    }
    return state->wait_idle();
}
} // namespace cue::dx12
