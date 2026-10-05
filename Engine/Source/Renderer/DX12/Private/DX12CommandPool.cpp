#include <DX12/DX12CommandPool.h>

#include <algorithm>
#include <array>
#include <exception>
#include <limits>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <Windows.h>

#include <DX12/DX12QueuePool.h>
#include <DX12/DX12RenderDevice.h>
#include <Platform/Diagnostics.h>

namespace cue::dx12
{
namespace
{
constexpr std::size_t k_typeCount = 3;
constexpr std::size_t k_maxContextsPerType = 32;

/// @brief Queue 種類を固定配列の位置へ変換する
std::optional<std::size_t> type_index(QueueType a_type)
{
    switch (a_type)
    {
    case QueueType::Graphics:
        return 0;
    case QueueType::Compute:
        return 1;
    case QueueType::Copy:
        return 2;
    }
    return std::nullopt;
}

/// @brief RHI の種類を D3D12 Command List の種類へ対応させる
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
Error command_error(const char* a_operation, HRESULT a_result)
{
    return {ErrorCategory::PlatformFailure, a_operation, static_cast<std::int64_t>(a_result)};
}

/// @brief Device Lost の Sentinel を完了値として扱わない
bool is_fence_complete(ID3D12Fence* a_fence, std::uint64_t a_value) noexcept
{
    if (!a_fence || a_value == 0)
    {
        return false;
    }
    const std::uint64_t completed = a_fence->GetCompletedValue();
    return completed != (std::numeric_limits<std::uint64_t>::max)() && completed >= a_value;
}

/// @brief Queue の Lease に依存せず、保持した Fence の完了を待つ
Result<void> wait_for_fence(ID3D12Fence* a_fence, ID3D12Device* a_device, std::uint64_t a_value)
{
    if (is_fence_complete(a_fence, a_value))
    {
        return Result<void>::success();
    }
    if (!a_fence || !a_device || a_value == 0)
    {
        return Result<void>::failure({ErrorCategory::Fatal, "DX12CommandCompletion.wait.fence"});
    }
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event)
    {
        return Result<void>::failure({ErrorCategory::PlatformFailure, "CreateEventW",
                                      static_cast<std::int64_t>(GetLastError())});
    }
    const HRESULT result = a_fence->SetEventOnCompletion(a_value, event);
    if (FAILED(result))
    {
        CloseHandle(event);
        return Result<void>::failure(command_error("ID3D12Fence.SetEventOnCompletion", result));
    }
    const DWORD waitResult = WaitForSingleObject(event, INFINITE);
    const DWORD waitError = waitResult == WAIT_OBJECT_0 ? ERROR_SUCCESS : GetLastError();
    CloseHandle(event);
    if (waitResult != WAIT_OBJECT_0)
    {
        return Result<void>::failure({ErrorCategory::PlatformFailure, "WaitForSingleObject",
                                      static_cast<std::int64_t>(waitError)});
    }
    if (!is_fence_complete(a_fence, a_value))
    {
        return Result<void>::failure(command_error("ID3D12Device.GetDeviceRemovedReason",
                                                   a_device->GetDeviceRemovedReason()));
    }
    return Result<void>::success();
}
} // namespace

/// @brief 提出元 Queue の Fence を値とともに保持し、Queue の寿命から独立させる
class DX12CommandCompletion final : public ICommandCompletion
{
public:
    /// @brief GPU 提出前に完了 Token の所有資源を確保する
    DX12CommandCompletion(Microsoft::WRL::ComPtr<ID3D12Device> a_device,
                          Microsoft::WRL::ComPtr<ID3D12Fence> a_fence, QueueType a_type) noexcept
        : m_device(std::move(a_device)), m_fence(std::move(a_fence)), m_type(a_type)
    {
    }

    /// @brief 成功した提出に対応する Fence 値を記録する
    void set_value(std::uint64_t a_value) noexcept
    {
        m_value = a_value;
    }

    /// @brief 提出に使用した Queue の種類を返す
    [[nodiscard]] QueueType type() const noexcept override
    {
        return m_type;
    }

    /// @brief この Token が保持する Fence の完了だけを確認する
    [[nodiscard]] bool is_complete() const noexcept override
    {
        return is_fence_complete(m_fence.Get(), m_value);
    }

    /// @brief この Token が保持する Fence の完了を待つ
    [[nodiscard]] Result<void> wait() override
    {
        return wait_for_fence(m_fence.Get(), m_device.Get(), m_value);
    }

private:
    Microsoft::WRL::ComPtr<ID3D12Device> m_device;
    Microsoft::WRL::ComPtr<ID3D12Fence> m_fence;
    std::uint64_t m_value = 0;
    QueueType m_type;
};

/// @brief create の内部でだけ未完成の Context を構築する
DX12GpuCommandContext::DX12GpuCommandContext(CreateToken) noexcept
{
}

/// @brief Allocator と List を同じ種類で作り、途中失敗を外へ公開しない
Result<std::unique_ptr<DX12GpuCommandContext>> DX12GpuCommandContext::create(ID3D12Device& a_device,
                                                                              QueueType a_type,
                                                                              std::uint32_t a_index)
{
    using ContextResult = Result<std::unique_ptr<DX12GpuCommandContext>>;
    if (!type_index(a_type))
    {
        return ContextResult::failure({ErrorCategory::InvalidArgument, "DX12GpuCommandContext.create.type"});
    }

    auto context = std::make_unique<DX12GpuCommandContext>(CreateToken{});
    context->m_device = &a_device;
    context->m_type = a_type;
    const D3D12_COMMAND_LIST_TYPE nativeType = command_list_type(a_type);
    const HRESULT allocatorResult = a_device.CreateCommandAllocator(nativeType, IID_PPV_ARGS(&context->m_allocator));
    if (FAILED(allocatorResult))
    {
        return ContextResult::failure(command_error("ID3D12Device.CreateCommandAllocator", allocatorResult));
    }
    const HRESULT listResult = a_device.CreateCommandList(0, nativeType, context->m_allocator.Get(), nullptr,
                                                           IID_PPV_ARGS(&context->m_list));
    if (FAILED(listResult))
    {
        return ContextResult::failure(command_error("ID3D12Device.CreateCommandList", listResult));
    }

    // 同種類の Context を Debug Layer と PIX 上で識別できるようにする
    const wchar_t* typeName = a_type == QueueType::Graphics ? L"Graphics" :
                              a_type == QueueType::Compute ? L"Compute" : L"Copy";
    std::wstring namePrefix = L"CueEngine DX12 ";
    namePrefix += typeName;
    namePrefix += L" Command ";
    namePrefix += std::to_wstring(a_index);
    const std::wstring allocatorName = namePrefix + L" Allocator";
    const HRESULT allocatorNameResult = context->m_allocator->SetName(allocatorName.c_str());
    if (FAILED(allocatorNameResult))
    {
        report_error("DX12GpuCommandContext", command_error("ID3D12CommandAllocator.SetName", allocatorNameResult),
                     DiagnosticSeverity::Warning);
    }
    const std::wstring listName = namePrefix + L" List";
    const HRESULT listNameResult = context->m_list->SetName(listName.c_str());
    if (FAILED(listNameResult))
    {
        report_error("DX12GpuCommandContext", command_error("ID3D12GraphicsCommandList.SetName", listNameResult),
                     DiagnosticSeverity::Warning);
    }
    return ContextResult::success(std::move(context));
}

/// @brief List が作成された Queue 種類を返す
QueueType DX12GpuCommandContext::type() const noexcept
{
    return m_type;
}

/// @brief 呼出側が記録・提出の順序を確認できる状態を返す
CommandState DX12GpuCommandContext::state() const noexcept
{
    return m_state;
}

/// @brief Close に失敗した List は Reset せず使用不能にする
Result<void> DX12GpuCommandContext::close()
{
    if (m_state != CommandState::Recording)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "DX12GpuCommandContext.close.state"});
    }
    const HRESULT result = m_list->Close();
    if (FAILED(result))
    {
        m_state = CommandState::Failed;
        m_isFatalFailure = true;
        auto error = command_error("ID3D12GraphicsCommandList.Close", result);
        // 記録内容の不正は Slot の再生成では直せないため、呼出側の処理は止める
        error.category = ErrorCategory::Fatal;
        return Result<void>::failure(std::move(error));
    }
    m_state = CommandState::Closed;
    return Result<void>::success();
}

/// @brief 記録中の Lease にだけ Native List を渡す
ID3D12GraphicsCommandList* DX12GpuCommandContext::command_list() const noexcept
{
    return m_state == CommandState::Recording ? m_list.Get() : nullptr;
}

/// @brief Native Binding 前に寿命を確保し、確保失敗時は List を変更しない
Result<void> DX12GpuCommandContext::retain_pipeline(std::shared_ptr<const void> a_pipeline)
{
    if (m_state != CommandState::Recording || !a_pipeline)
        return Result<void>::failure({ErrorCategory::InvalidState, "DX12GpuCommandContext.retain_pipeline"});
    if (std::find(m_pipelineReferences.begin(), m_pipelineReferences.end(), a_pipeline) != m_pipelineReferences.end())
        return Result<void>::success();
    try
    {
        m_pipelineReferences.push_back(std::move(a_pipeline));
        return Result<void>::success();
    }
    catch (const std::bad_alloc &)
    {
        return Result<void>::failure(
            {ErrorCategory::PlatformFailure, "DX12GpuCommandContext.retain_pipeline.allocation"});
    }
}

/// @brief 未提出または Fence 完了済みの Allocator だけを Reset する
Result<void> DX12GpuCommandContext::reset_for_recording()
{
    if (m_state != CommandState::Closed && m_state != CommandState::Submitted)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "DX12GpuCommandContext.reset.state"});
    }
    const HRESULT allocatorResult = m_allocator->Reset();
    if (FAILED(allocatorResult))
    {
        m_state = CommandState::Failed;
        return Result<void>::failure(command_error("ID3D12CommandAllocator.Reset", allocatorResult));
    }
    const HRESULT listResult = m_list->Reset(m_allocator.Get(), nullptr);
    if (FAILED(listResult))
    {
        m_state = CommandState::Failed;
        return Result<void>::failure(command_error("ID3D12GraphicsCommandList.Reset", listResult));
    }
    // Pool が未提出または Fence 完了を確認し、List の Reset も成功した後だけ旧 Pipeline を返す
    m_pipelineReferences.clear();
    m_fenceValue = 0;
    m_submissionFence.Reset();
    m_isFatalFailure = false;
    m_state = CommandState::Recording;
    return Result<void>::success();
}

/// @brief Pool と Lease の間で Command Slot を共有する
struct DX12CommandPool::State
{
    // 最後の Lease が提出済み Context の完了を確認するまで Descriptor Heap を保持する
    std::shared_ptr<const void> gpuLifetime;
    struct Slot
    {
        std::unique_ptr<DX12GpuCommandContext> context;
        bool isBorrowed = false;
    };

    Microsoft::WRL::ComPtr<ID3D12Device> device;
    std::array<std::vector<Slot>, k_typeCount> slots{};
    std::mutex mutex;
    bool isStopping = false;
    bool isFatal = false;
    bool hasUnknownSubmission = false;
    bool hasWaitedIdle = false;

    /// @brief 最後の Lease が消えた後も GPU 作業を完了させてから Slot を破棄する
    ~State()
    {
        if (!hasWaitedIdle)
        {
            auto result = wait_idle();
            if (!result.has_value())
            {
                report_error("DX12CommandPool.State", *result.try_error(),
                             hasUnknownSubmission ? DiagnosticSeverity::Fatal : DiagnosticSeverity::Error);
                // 完了を証明できない Allocator を破棄する前に停止する
                if (has_pending_gpu_work())
                {
                    std::terminate();
                }
            }
        }
    }

    /// @brief 提出済みの Slot が GPU 作業を残しているか確認する
    [[nodiscard]] bool has_pending_gpu_work() const noexcept
    {
        if (hasUnknownSubmission)
        {
            return true;
        }
        for (const auto& typeSlots : slots)
        {
            for (const auto& slot : typeSlots)
            {
                if (slot.context->m_state == CommandState::Submitted &&
                    !is_submission_complete(*slot.context))
                {
                    return true;
                }
            }
        }
        return false;
    }

    /// @brief Device Lost の Sentinel を完了値として扱わない
    [[nodiscard]] static bool is_submission_complete(const DX12GpuCommandContext& a_context) noexcept
    {
        return is_fence_complete(a_context.m_submissionFence.Get(), a_context.m_fenceValue);
    }

    /// @brief Queue の Lease に依存せず、Slot が保持する Fence を待つ
    [[nodiscard]] Result<void> wait_for_submission(const DX12GpuCommandContext& a_context) const
    {
        return wait_for_fence(a_context.m_submissionFence.Get(), device.Get(), a_context.m_fenceValue);
    }

    /// @brief 発行済み Command の完了を各 Slot の Fence で確認する
    [[nodiscard]] Result<void> wait_idle()
    {
        if (hasUnknownSubmission)
        {
            return Result<void>::failure({ErrorCategory::Fatal, "DX12CommandPool.wait_idle.unknown_submission"});
        }
        std::optional<Error> firstError;
        for (const auto& typeSlots : slots)
        {
            for (const auto& slot : typeSlots)
            {
                if (slot.context->m_state == CommandState::Submitted)
                {
                    auto waitResult = wait_for_submission(*slot.context);
                    if (!waitResult.has_value() && !firstError)
                    {
                        firstError = *waitResult.try_error();
                    }
                }
            }
        }
        if (firstError)
        {
            return Result<void>::failure(std::move(*firstError));
        }
        hasWaitedIdle = true;
        return Result<void>::success();
    }
};

/// @brief create の内部でだけ空の Pool を構築する
DX12CommandPool::DX12CommandPool(CreateToken) noexcept
{
}

/// @brief 停止を試み、借用中の Context は Lease の破棄まで保持する
DX12CommandPool::~DX12CommandPool()
{
    auto result = shutdown();
    if (!result.has_value() && result.try_error()->category != ErrorCategory::InvalidState)
    {
        report_error("DX12CommandPool.shutdown", *result.try_error(), DiagnosticSeverity::Error);
    }
}

/// @brief Device を保持し、提出先 Queue を固定せず Command Slot を作る
Result<std::unique_ptr<DX12CommandPool>> DX12CommandPool::create(DX12RenderDevice& a_device,
                                                                  std::shared_ptr<const void> a_gpuLifetime)
{
    using PoolResult = Result<std::unique_ptr<DX12CommandPool>>;
    if (!a_device.device())
    {
        return PoolResult::failure({ErrorCategory::InvalidArgument, "DX12CommandPool.create.device"});
    }
    auto pool = std::make_unique<DX12CommandPool>(CreateToken{});
    pool->m_state = std::make_shared<State>();
    pool->m_state->gpuLifetime = std::move(a_gpuLifetime);
    pool->m_state->device = a_device.device();
    return PoolResult::success(std::move(pool));
}

/// @brief 未完了 Context を飛ばし、Allocator を Reset できる Slot だけを貸し出す
Result<commandLease> DX12CommandPool::acquire(QueueType a_type)
{
    const auto typeIndex = type_index(a_type);
    if (!typeIndex)
    {
        return Result<commandLease>::failure({ErrorCategory::InvalidArgument, "DX12CommandPool.acquire.type"});
    }
    auto state = m_state;
    std::lock_guard lock(state->mutex);
    if (state->isStopping || state->isFatal)
    {
        return Result<commandLease>::failure({state->isFatal ? ErrorCategory::Fatal : ErrorCategory::InvalidState,
                                              "DX12CommandPool.acquire.unavailable"});
    }
    auto& slots = state->slots[*typeIndex];
    std::size_t slotIndex = slots.size();
    for (std::size_t index = 0; index < slots.size(); ++index)
    {
        auto& slot = slots[index];
        if (slot.isBorrowed || slot.context->m_state == CommandState::SubmissionUnknown)
        {
            continue;
        }
        bool shouldRebuild = slot.context->m_state == CommandState::Failed;
        if (shouldRebuild && slot.context->m_isFatalFailure)
        {
            // 不正な記録内容は Slot の再生成では直せない
            state->isFatal = true;
            return Result<commandLease>::failure({ErrorCategory::Fatal, "DX12CommandPool.acquire.recording_failure"});
        }
        if (!shouldRebuild)
        {
            if (slot.context->m_state == CommandState::Submitted &&
                !State::is_submission_complete(*slot.context))
            {
                const HRESULT deviceStatus = state->device->GetDeviceRemovedReason();
                if (FAILED(deviceStatus))
                {
                    state->isFatal = true;
                    auto error = command_error("ID3D12Device.GetDeviceRemovedReason", deviceStatus);
                    error.category = ErrorCategory::Fatal;
                    return Result<commandLease>::failure(std::move(error));
                }
                continue;
            }
            auto resetResult = slot.context->reset_for_recording();
            if (!resetResult.has_value())
            {
                // Reset 失敗後も前回の提出完了は確認済みなので、Device が有効なら Slot を再生成する
                report_error("DX12CommandPool.acquire", *resetResult.try_error(), DiagnosticSeverity::Error);
                shouldRebuild = true;
            }
        }
        if (shouldRebuild)
        {
            const HRESULT deviceStatus = state->device->GetDeviceRemovedReason();
            if (FAILED(deviceStatus))
            {
                state->isFatal = true;
                auto error = command_error("ID3D12Device.GetDeviceRemovedReason", deviceStatus);
                error.category = ErrorCategory::Fatal;
                return Result<commandLease>::failure(std::move(error));
            }
            auto replacementResult = DX12GpuCommandContext::create(*state->device.Get(), a_type,
                                                                    static_cast<std::uint32_t>(index));
            if (!replacementResult.has_value())
            {
                const HRESULT deviceStatus = state->device->GetDeviceRemovedReason();
                if (FAILED(deviceStatus))
                {
                    state->isFatal = true;
                    auto error = command_error("ID3D12Device.GetDeviceRemovedReason", deviceStatus);
                    error.category = ErrorCategory::Fatal;
                    return Result<commandLease>::failure(std::move(error));
                }
                return Result<commandLease>::failure(*replacementResult.try_error());
            }
            slot.context = replacementResult.take_value();
        }
        slotIndex = index;
        break;
    }
    if (slotIndex == slots.size())
    {
        if (slots.size() == k_maxContextsPerType)
        {
            return Result<commandLease>::failure({ErrorCategory::InvalidState, "DX12CommandPool.acquire.capacity"});
        }
        auto contextResult = DX12GpuCommandContext::create(*state->device.Get(), a_type,
                                                            static_cast<std::uint32_t>(slots.size()));
        if (!contextResult.has_value())
        {
            const HRESULT deviceStatus = state->device->GetDeviceRemovedReason();
            if (FAILED(deviceStatus))
            {
                state->isFatal = true;
                auto error = command_error("ID3D12Device.GetDeviceRemovedReason", deviceStatus);
                error.category = ErrorCategory::Fatal;
                return Result<commandLease>::failure(std::move(error));
            }
            return Result<commandLease>::failure(*contextResult.try_error());
        }
        slots.push_back({contextResult.take_value(), false});
    }

    auto& slot = slots[slotIndex];
    commandLease lease(slot.context.get(), [state, typeIndex = *typeIndex, slotIndex](ICommandContext*) mutable {
        // 未提出の記録を閉じてから返し、Close に失敗した Slot は再利用しない
        {
            std::lock_guard leaseLock(state->mutex);
            auto& returned = state->slots[typeIndex][slotIndex];
            if (returned.context->m_state == CommandState::Recording)
            {
                auto closeResult = returned.context->close();
                if (!closeResult.has_value())
                {
                    report_error("DX12CommandPool.release", *closeResult.try_error(), DiagnosticSeverity::Error);
                }
            }
            returned.isBorrowed = false;
        }
        // reset() 後も Deleter 自体は残るため、共有所有をここで明示的に終える
        state.reset();
    });
    slot.isBorrowed = true;
    state->hasWaitedIdle = false;
    return Result<commandLease>::success(std::move(lease));
}

/// @brief 提出と同じ排他区間で Context に Queue の Fence 値を紐づける
Result<commandCompletion> DX12CommandPool::submit(IQueueContext& a_queue, ICommandContext& a_context)
{
    const auto state = m_state;
    std::lock_guard lock(state->mutex);
    if (state->isStopping || state->isFatal)
    {
        return Result<commandCompletion>::failure({state->isFatal ? ErrorCategory::Fatal : ErrorCategory::InvalidState,
                                                   "DX12CommandPool.submit.unavailable"});
    }
    auto* queue = dynamic_cast<DX12GpuCommandQueue*>(&a_queue);
    auto* context = dynamic_cast<DX12GpuCommandContext*>(&a_context);
    if (!context || context->m_device.Get() != state->device.Get())
    {
        return Result<commandCompletion>::failure({ErrorCategory::InvalidArgument, "DX12CommandPool.submit.foreign"});
    }
    if (!queue || queue->device() != state->device.Get() || queue->type() != context->m_type)
    {
        return Result<commandCompletion>::failure({ErrorCategory::InvalidArgument, "DX12CommandPool.submit.queue"});
    }
    const auto typeIndex = type_index(context->m_type);
    if (!typeIndex)
    {
        return Result<commandCompletion>::failure({ErrorCategory::InvalidArgument, "DX12CommandPool.submit.type"});
    }
    bool isOwnedAndBorrowed = false;
    for (const auto& slot : state->slots[*typeIndex])
    {
        if (slot.context.get() == context && slot.isBorrowed)
        {
            isOwnedAndBorrowed = true;
            break;
        }
    }
    if (!isOwnedAndBorrowed || context->m_state != CommandState::Closed)
    {
        return Result<commandCompletion>::failure({ErrorCategory::InvalidState, "DX12CommandPool.submit.state"});
    }
    // ExecuteCommandLists の後に Allocation 失敗を起こさないよう Token を先に生成する
    auto completion = std::make_unique<DX12CommandCompletion>(state->device, queue->completion_fence(),
                                                              context->m_type);
    ID3D12CommandList* lists[] = {context->m_list.Get()};
    bool mayHaveExecuted = false;
    auto result = queue->submit(lists, &mayHaveExecuted);
    if (!result.has_value())
    {
        if (mayHaveExecuted)
        {
            // 提出後に Fence が発行できなければ、Queue を停止して GPU 完了を証明する
            context->m_state = CommandState::SubmissionUnknown;
            state->hasUnknownSubmission = true;
            state->isFatal = true;
            auto error = *result.try_error();
            error.category = ErrorCategory::Fatal;
            auto idleResult = queue->wait_idle();
            if (!idleResult.has_value())
            {
                report_error("DX12CommandPool.submit", *idleResult.try_error(), DiagnosticSeverity::Fatal);
                // Queue の Lease と Allocator が生きている間に停止する
                std::terminate();
            }
            state->hasUnknownSubmission = false;
            context->m_state = CommandState::Failed;
            return Result<commandCompletion>::failure(std::move(error));
        }
        else if (result.try_error()->category != ErrorCategory::InvalidArgument)
        {
            // 提出前でも Device 故障や Fence 枯渇は同じ Pool では回復できない
            state->isFatal = true;
            auto error = *result.try_error();
            error.category = ErrorCategory::Fatal;
            return Result<commandCompletion>::failure(std::move(error));
        }
        return Result<commandCompletion>::failure(*result.try_error());
    }
    context->m_submissionFence = queue->completion_fence();
    context->m_fenceValue = *result.try_value();
    context->m_state = CommandState::Submitted;
    state->hasWaitedIdle = false;
    completion->set_value(context->m_fenceValue);
    return Result<commandCompletion>::success(std::move(completion));
}

/// @brief 貸出を止め、返却済み Context の GPU 作業をすべて完了させる
Result<void> DX12CommandPool::shutdown()
{
    const auto state = m_state;
    if (!state)
    {
        return Result<void>::success();
    }
    std::lock_guard lock(state->mutex);
    state->isStopping = true;
    for (const auto& slots : state->slots)
    {
        for (const auto& slot : slots)
        {
            if (slot.isBorrowed)
            {
                return Result<void>::failure({ErrorCategory::InvalidState, "DX12CommandPool.shutdown.borrowed"});
            }
        }
    }
    return state->wait_idle();
}
} // namespace cue::dx12
