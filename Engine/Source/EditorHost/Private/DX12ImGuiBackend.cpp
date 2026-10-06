#include "DX12ImGuiBackend.h"

#include <algorithm>
#include <exception>
#include <new>
#include <optional>
#include <vector>

#include <imgui.h>
#include <imgui_impl_dx12.h>

#include <DX12/DX12Backend.h>
#include <DX12/DX12DescriptorAllocator.h>
#include <DX12/DX12FrameGraphContext.h>
#include <DX12/DX12QueuePool.h>
#include <DX12/DX12RenderDevice.h>
#include <DX12/DX12SwapChain.h>
#include <Platform/Diagnostics.h>

namespace cue
{
namespace
{
/// @brief 公式 Backend 操作中だけ借用した Context を Current にする
class ContextScope final
{
  public:
    /// @brief 呼出元の Context を退避する
    explicit ContextScope(ImGuiContext *a_context) noexcept : m_previous(ImGui::GetCurrentContext())
    {
        ImGui::SetCurrentContext(a_context);
    }
    /// @brief 呼出元の Context を復元する
    ~ContextScope()
    {
        ImGui::SetCurrentContext(m_previous);
    }

  private:
    ImGuiContext *m_previous;
};
} // namespace

class DX12ImGuiBackend::State final
{
  public:
    struct Descriptor final
    {
        dx12::DX12DescriptorHandle handle;
        D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
        D3D12_GPU_DESCRIPTOR_HANDLE gpu{};
        bool isReserved = false;
    };

    ImGuiContext *context = nullptr;
    ID3D12Device *device = nullptr;
    dx12::DX12GpuCommandQueue *queue = nullptr;
    std::unique_ptr<dx12::DX12DescriptorAllocator> allocator;
    std::vector<Descriptor> descriptors;
    ImGuiRendererInfo info;
    GpuTextureFormat format = GpuTextureFormat::Rgba8Unorm;
    bool isInitialized = false;

    /// @brief void Callback が失敗した状態で公式 CreateSRV を続けない
    [[noreturn]] static void invariant_failure(const char *a_operation) noexcept
    {
        report_error("ImGui DX12", {ErrorCategory::Fatal, a_operation}, DiagnosticSeverity::Fatal);
        std::terminate();
    }

    /// @brief 事前予約した Slot だけを公式 Backend へ渡し、Callback 内の割当を避ける
    static void allocate(ImGui_ImplDX12_InitInfo *a_info, D3D12_CPU_DESCRIPTOR_HANDLE *a_cpu,
                         D3D12_GPU_DESCRIPTOR_HANDLE *a_gpu) noexcept
    {
        auto &state = *static_cast<State *>(a_info->UserData);
        auto slot = std::find_if(state.descriptors.begin(), state.descriptors.end(),
                                 [](const Descriptor &a_slot) { return a_slot.isReserved; });
        if (slot == state.descriptors.end())
        {
            invariant_failure("ImGuiDX12.descriptor.reservation");
        }
        slot->isReserved = false;
        *a_cpu = slot->cpu;
        *a_gpu = slot->gpu;
        ++state.info.activeDescriptors;
    }

    /// @brief GPU 完了確認後の Native Pair を世代 Handle に戻して Slot を返却する
    static void release(ImGui_ImplDX12_InitInfo *a_info, D3D12_CPU_DESCRIPTOR_HANDLE a_cpu,
                        D3D12_GPU_DESCRIPTOR_HANDLE a_gpu) noexcept
    {
        auto &state = *static_cast<State *>(a_info->UserData);
        auto slot = std::find_if(state.descriptors.begin(), state.descriptors.end(),
                                 [a_cpu, a_gpu](const Descriptor &a_slot)
                                 {
                                     return a_slot.handle.is_valid() && !a_slot.isReserved &&
                                            a_slot.cpu.ptr == a_cpu.ptr && a_slot.gpu.ptr == a_gpu.ptr;
                                 });
        if (slot == state.descriptors.end() || !state.allocator->release(slot->handle).has_value())
        {
            invariant_failure("ImGuiDX12.descriptor.release");
        }
        *slot = {};
        --state.info.activeDescriptors;
    }

    /// @brief 公式処理へ進む前に必要数の Slot と Native Handle をすべて確保する
    [[nodiscard]] Result<void> reserve(std::size_t a_count)
    {
        for (std::size_t index = 0; index < a_count; ++index)
        {
            auto slot = std::find_if(descriptors.begin(), descriptors.end(),
                                     [](const Descriptor &a_slot) { return !a_slot.handle.is_valid(); });
            if (slot == descriptors.end())
            {
                release_reservations();
                return Result<void>::failure({ErrorCategory::InvalidState, "ImGuiDX12.descriptor.capacity"});
            }
            auto allocated = allocator->allocate();
            if (!allocated.has_value())
            {
                release_reservations();
                return Result<void>::failure(*allocated.try_error());
            }
            slot->handle = allocated.take_value();
            slot->isReserved = true;
            auto cpu = allocator->cpu_handle(slot->handle);
            auto gpu = allocator->gpu_handle(slot->handle);
            if (!cpu.has_value() || !gpu.has_value())
            {
                const auto error = !cpu.has_value() ? *cpu.try_error() : *gpu.try_error();
                release_reservations();
                return Result<void>::failure(error);
            }
            slot->cpu = cpu.take_value();
            slot->gpu = gpu.take_value();
        }
        return Result<void>::success();
    }

    /// @brief 公式処理が消費しなかった予約を GPU 参照なしで返却する
    void release_reservations() noexcept
    {
        for (auto &slot : descriptors)
        {
            if (slot.isReserved)
            {
                if (!allocator->release(slot.handle).has_value())
                {
                    invariant_failure("ImGuiDX12.descriptor.rollback");
                }
                slot = {};
            }
        }
    }

    /// @brief 未登録または別 Heap の Texture ID を描画へ通さない
    [[nodiscard]] bool owns_texture(ImTextureID a_id) const noexcept
    {
        return std::any_of(descriptors.begin(), descriptors.end(), [a_id](const Descriptor &a_slot)
                           { return a_slot.handle.is_valid() && !a_slot.isReserved && a_slot.gpu.ptr == a_id; });
    }
};

/// @brief static create 専用の空状態を作る
DX12ImGuiBackend::DX12ImGuiBackend(CreateToken) noexcept
{
}

/// @brief SwapChain の Queue を借用し、Editor 専用 Heap を公式 Backend に接続する
Result<std::unique_ptr<DX12ImGuiBackend>> DX12ImGuiBackend::create(IBackend &a_backend, ImGuiContext &a_context,
                                                                   std::uint32_t a_frameCount, std::uint32_t a_capacity)
{
    using BackendResult = Result<std::unique_ptr<DX12ImGuiBackend>>;
    if (a_frameCount == 0 || a_frameCount > 2 || a_capacity == 0)
    {
        return BackendResult::failure({ErrorCategory::InvalidArgument, "ImGuiDX12.config"});
    }
    auto *backend = dynamic_cast<dx12::DX12Backend *>(&a_backend);
    auto *device = backend ? dynamic_cast<dx12::DX12RenderDevice *>(backend->get_render_device()) : nullptr;
    auto *swap = backend ? backend->get_swap_chain() : nullptr;
    auto *queue = swap ? dynamic_cast<dx12::DX12GpuCommandQueue *>(swap->graphics_queue()) : nullptr;
    auto *buffer = swap ? swap->back_buffer(0) : nullptr;
    if (!device || !queue || !buffer || queue->device() != device->device())
    {
        return BackendResult::failure({ErrorCategory::InvalidState, "ImGuiDX12.backend"});
    }
    const auto desc = buffer->GetDesc();
    if (desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM || desc.SampleDesc.Count != 1)
    {
        return BackendResult::failure({ErrorCategory::InvalidArgument, "ImGuiDX12.target_format"});
    }
    ContextScope current(&a_context);
    if (ImGui::GetIO().BackendRendererUserData)
    {
        return BackendResult::failure({ErrorCategory::InvalidState, "ImGuiDX12.already_initialized"});
    }
    try
    {
        auto result = std::make_unique<DX12ImGuiBackend>(CreateToken{});
        result->m_state = std::make_unique<State>();
        auto &state = *result->m_state;
        state.context = &a_context;
        state.device = device->device();
        state.queue = queue;
        state.info.frameCount = a_frameCount;
        state.info.descriptorCapacity = a_capacity;
        state.descriptors.resize(a_capacity);
        auto heap = dx12::DX12DescriptorAllocator::create(*state.device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                                                          a_capacity, true);
        if (!heap.has_value())
        {
            return BackendResult::failure(*heap.try_error());
        }
        state.allocator = heap.take_value();
        const auto named = state.allocator->heap()->SetName(L"Cue Editor ImGui SRV Heap");
        if (FAILED(named))
        {
            return BackendResult::failure({ErrorCategory::PlatformFailure, "ImGuiDX12.heap.name", named});
        }
        ImGui_ImplDX12_InitInfo info;
        info.Device = state.device;
        info.CommandQueue = queue->command_queue();
        info.NumFramesInFlight = static_cast<int>(a_frameCount);
        info.RTVFormat = desc.Format;
        info.DSVFormat = DXGI_FORMAT_UNKNOWN;
        info.UserData = &state;
        info.SrvDescriptorHeap = state.allocator->heap();
        info.SrvDescriptorAllocFn = &State::allocate;
        info.SrvDescriptorFreeFn = &State::release;
        bool initialized = false;
        try
        {
            // 公式 Init の C++ 配列生成は Context 登録後に行われ、途中例外を安全に復元できない
            initialized = ImGui_ImplDX12_Init(&info);
        }
        catch (const std::bad_alloc &)
        {
            State::invariant_failure("ImGuiDX12.official_init.allocation");
        }
        if (!initialized)
        {
            return BackendResult::failure({ErrorCategory::PlatformFailure, "ImGui_ImplDX12_Init"});
        }
        state.isInitialized = true;
        // NewFrame の遅延作成 Assert へ進めず、生成失敗を初期化の Result で通知する
        if (!ImGui_ImplDX12_CreateDeviceObjects())
        {
            return BackendResult::failure({ErrorCategory::PlatformFailure, "ImGui_ImplDX12_CreateDeviceObjects"});
        }
        return BackendResult::success(std::move(result));
    }
    catch (const std::bad_alloc &)
    {
        return BackendResult::failure({ErrorCategory::PlatformFailure, "ImGuiDX12.allocation"});
    }
}

/// @brief 明示停止されていない公式 GPU 資源を回収する
DX12ImGuiBackend::~DX12ImGuiBackend()
{
    auto result = shutdown();
    if (!result.has_value())
    {
        report_error("ImGui DX12 cleanup", *result.try_error(), DiagnosticSeverity::Fatal);
        std::terminate();
    }
}

/// @brief 初期化済み公式 Backend の Frame を開始する
Result<void> DX12ImGuiBackend::new_frame()
{
    if (!m_state || !m_state->isInitialized)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "ImGuiDX12.new_frame"});
    }
    ContextScope current(m_state->context);
    ImGui_ImplDX12_NewFrame();
    return Result<void>::success();
}

/// @brief Texture 更新と公式 Native 記録を Graph の Graphics Command へ接続する
Result<void> DX12ImGuiBackend::record(ImDrawData &a_draw, FrameGraphContext &a_context)
{
    auto *context = dynamic_cast<dx12::DX12FrameGraphContext *>(&a_context);
    if (!m_state || !m_state->isInitialized || !context || !a_draw.Valid ||
        a_context.frame_index() >= m_state->info.frameCount ||
        a_context.command_context().type() != QueueType::Graphics ||
        a_context.command_context().state() != CommandState::Recording)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "ImGuiDX12.record"});
    }
    auto &state = *m_state;
    ContextScope current(state.context);
    auto target = context->validate_external_graphics(state.format);
    if (!target.has_value())
    {
        return target;
    }
    Microsoft::WRL::ComPtr<ID3D12Device> commandDevice;
    auto deviceResult = context->command_list().GetDevice(IID_PPV_ARGS(&commandDevice));
    if (FAILED(deviceResult) || commandDevice.Get() != state.device)
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "ImGuiDX12.command_device", deviceResult});
    }
    // 公式 Callback からの例外や未申告 Resource 利用を避け、公式 State 切替だけを許可する
    const auto &platform = ImGui::GetPlatformIO();
    for (const auto *list : a_draw.CmdLists)
    {
        for (const auto &command : list->CmdBuffer)
        {
            if (command.UserCallback && command.UserCallback != ImDrawCallback_ResetRenderState &&
                command.UserCallback != platform.DrawCallback_ResetRenderState &&
                command.UserCallback != platform.DrawCallback_SetSamplerLinear &&
                command.UserCallback != platform.DrawCallback_SetSamplerNearest)
            {
                return Result<void>::failure({ErrorCategory::InvalidArgument, "ImGuiDX12.draw_callback"});
            }
        }
    }
    // 最小化等で公式 Ring と Graph 枠がずれても VB/IB を上書きしない初期の同期方式
    // 公式 Texture Upload の Native 提出も新しい Queue Fence の完了点に含める
    auto waited = state.queue->wait_idle();
    if (!waited.has_value())
    {
        return waited;
    }
    std::size_t creates = 0;
    if (a_draw.Textures)
    {
        for (const auto *texture : *a_draw.Textures)
        {
            if (!texture ||
                (texture->Status == ImTextureStatus_WantCreate &&
                 (texture->Format != ImTextureFormat_RGBA32 || !texture->Pixels || texture->Width <= 0 ||
                  texture->Height <= 0 || texture->Width > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
                  texture->Height > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION)) ||
                ((texture->Status == ImTextureStatus_WantUpdates ||
                  (texture->Status == ImTextureStatus_WantDestroy && texture->BackendUserData)) &&
                 !state.owns_texture(texture->GetTexID())))
            {
                return Result<void>::failure({ErrorCategory::InvalidArgument, "ImGuiDX12.texture_data"});
            }
        }
        // 完了済み旧 Texture を先に回収し、満杯の Heap でも返却可能な Slot を再利用する
        for (auto *texture : *a_draw.Textures)
        {
            if (texture->Status == ImTextureStatus_WantDestroy &&
                texture->UnusedFrames >= static_cast<int>(state.info.frameCount))
            {
                ImGui_ImplDX12_UpdateTexture(texture);
            }
        }
        for (const auto *texture : *a_draw.Textures)
        {
            creates += texture->Status == ImTextureStatus_WantCreate ? 1 : 0;
        }
    }
    auto reserved = state.reserve(creates);
    if (!reserved.has_value())
    {
        return reserved;
    }
    if (a_draw.Textures)
    {
        for (auto *texture : *a_draw.Textures)
        {
            if (texture->Status != ImTextureStatus_OK)
            {
                ImGui_ImplDX12_UpdateTexture(texture);
            }
        }
    }
    state.release_reservations();
    const auto removed = state.device->GetDeviceRemovedReason();
    if (FAILED(removed))
    {
        return Result<void>::failure({ErrorCategory::Fatal, "ImGuiDX12.device_removed", removed});
    }
    for (const auto *list : a_draw.CmdLists)
    {
        for (const auto &command : list->CmdBuffer)
        {
            if (!command.UserCallback && command.ElemCount != 0 && !state.owns_texture(command.GetTexID()))
            {
                return Result<void>::failure({ErrorCategory::InvalidArgument, "ImGuiDX12.texture_heap"});
            }
        }
    }
    return context->record_external_graphics(state.format,
                                             [&](ID3D12GraphicsCommandList &a_list)
                                             {
                                                 ID3D12DescriptorHeap *heaps[] = {state.allocator->heap()};
                                                 a_list.SetDescriptorHeaps(1, heaps);
                                                 // Texture 更新は予約付きで完了済みなので、公式描画では二重処理しない
                                                 auto *textures = a_draw.Textures;
                                                 a_draw.Textures = nullptr;
                                                 ImGui_ImplDX12_RenderDrawData(&a_draw, &a_list);
                                                 a_draw.Textures = textures;
                                                 ++state.info.recordedFrames;
                                                 return Result<void>::success();
                                             });
}

/// @brief Snapshot は所有値だけで公開する
ImGuiRendererInfo DX12ImGuiBackend::info() const noexcept
{
    return m_state ? m_state->info : ImGuiRendererInfo{};
}

/// @brief 公式 Texture、PSO、Root、Vertex / Index Buffer を Heap より先に解放する
Result<void> DX12ImGuiBackend::shutdown()
{
    if (!m_state)
    {
        return Result<void>::success();
    }
    auto &state = *m_state;
    if (state.isInitialized)
    {
        auto waited = state.queue->wait_idle();
        if (!waited.has_value())
        {
            return waited;
        }
        ContextScope current(state.context);
        ImGui_ImplDX12_Shutdown();
        state.isInitialized = false;
    }
    state.release_reservations();
    if (state.info.activeDescriptors != 0)
    {
        State::invariant_failure("ImGuiDX12.shutdown.descriptors");
    }
    m_state.reset();
    return Result<void>::success();
}
} // namespace cue
