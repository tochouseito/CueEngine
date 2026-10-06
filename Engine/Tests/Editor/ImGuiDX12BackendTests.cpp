#include <EditorHost/ImGuiManager.h>

#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

#include <d3d12sdklayers.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <DX12/DX12Backend.h>
#include <DX12/DX12CommandPool.h>
#include <DX12/DX12FrameGraphContext.h>
#include <DX12/DX12GpuResource.h>
#include <DX12/DX12MainFrameGraph.h>
#include <DX12/DX12RenderDevice.h>
#include <DX12/DX12SwapChain.h>
#include <EditorHost/ImGuiPass.h>
#include <Passes/PresentToSwapChainPass.h>
#include <Platform/Windows/WindowsPlatform.h>

namespace
{
/// @brief 本番の ImGuiPass に Test の Manager を借用させる
class ManagerRenderer final : public cue::IImGuiRenderer
{
  public:
    /// @brief Graph と Pass より長く生存する Manager を借用する
    explicit ManagerRenderer(cue::ImGuiManager &a_manager) noexcept : m_manager(&a_manager)
    {
    }

    /// @brief 本番と同じ抽象契約から公式 GPU 記録へ進む
    [[nodiscard]] cue::Result<void> record_draw_data(cue::FrameGraphContext &a_context) override
    {
        return m_manager->record_draw_data(a_context);
    }

  private:
    cue::ImGuiManager *m_manager;
};

/// @brief Test 専用の表示 Pass から抽象 API で GPU Adapter を呼ぶ
class BackendPass final : public cue::FrameGraphPass
{
  public:
    /// @brief Graph より長く生存する Manager と検査値を借用する
    BackendPass(cue::ImGuiManager &a_manager, bool &a_restore)
        : m_manager(&a_manager), m_restore(&a_restore), m_ui(std::make_unique<ManagerRenderer>(a_manager))
    {
    }
    /// @brief Test Pass の診断名を返す
    [[nodiscard]] const char *name() const noexcept override
    {
        return "ImGuiBackendTest";
    }
    /// @brief 公式 Backend は Graphics Command に記録する
    [[nodiscard]] cue::QueueType type() const noexcept override
    {
        return cue::QueueType::Graphics;
    }
    /// @brief 標準表示 Pipeline と BackBuffer を構築時に取得する
    [[nodiscard]] cue::Result<void> setup(cue::FrameGraphBuilder &a_builder) override
    {
        auto buffer = a_builder.get_texture("BackBuffer");
        if (!buffer.has_value())
        {
            return cue::Result<void>::failure(*buffer.try_error());
        }
        m_buffer = buffer.take_value();
        auto present = m_present.setup(a_builder);
        if (!present.has_value())
        {
            return present;
        }
        return m_ui.setup(a_builder);
    }
    /// @brief Texture 読取と BackBuffer 書込の State を標準表示と同じく宣言する
    [[nodiscard]] cue::Result<void> describe_resources(cue::FrameGraphBuilder &a_builder) override
    {
        return m_present.describe_resources(a_builder);
    }
    /// @brief 外部記録の事前検証、例外後の失効、UI 描画と通常 Heap の再設定を確認する
    [[nodiscard]] cue::Result<void> execute(cue::FrameGraphContext &a_context) override
    {
        if (m_manager->record_draw_data(a_context).has_value())
        {
            return cue::Result<void>::failure({cue::ErrorCategory::InvalidState, "Test.unbound_target"});
        }
        auto bound = a_context.set_render_target(m_buffer);
        auto *context = dynamic_cast<cue::dx12::DX12FrameGraphContext *>(&a_context);
        if (!bound.has_value() || !context ||
            context->validate_external_graphics(cue::GpuTextureFormat::Bgra8Unorm).has_value())
        {
            return cue::Result<void>::failure({cue::ErrorCategory::InvalidState, "Test.target_format"});
        }
        auto interrupted = context->record_external_graphics(cue::GpuTextureFormat::Rgba8Unorm,
                                                             [&](ID3D12GraphicsCommandList &) -> cue::Result<void>
                                                             {
                                                                 // Context で State を設定した後の例外でも Cache
                                                                 // が残らないことを検査する
                                                                 [[maybe_unused]] auto viewport =
                                                                     a_context.set_viewport_scissor(96, 96);
                                                                 throw std::runtime_error("external record failure");
                                                             });
        if (interrupted.has_value() ||
            context->validate_external_graphics(cue::GpuTextureFormat::Rgba8Unorm).has_value() ||
            a_context.draw_instanced(3, 1).has_value())
        {
            return cue::Result<void>::failure({cue::ErrorCategory::InvalidState, "Test.external_cache"});
        }
        auto displayed = m_present.execute(a_context);
        if (!displayed.has_value())
        {
            return displayed;
        }
        // 本番 Pass の Clear / RTV 設定と抽象 Adapter を含めて実画素を検証する
        auto recorded = m_ui.execute(a_context);
        if (!recorded.has_value())
        {
            return recorded;
        }
        if (m_manager->record_draw_data(a_context).has_value() || a_context.draw_instanced(3, 1).has_value())
        {
            return cue::Result<void>::failure({cue::ErrorCategory::InvalidState, "Test.repeated_record"});
        }
        return *m_restore ? m_present.execute(a_context) : cue::Result<void>::success();
    }

  private:
    cue::ImGuiManager *m_manager;
    bool *m_restore;
    cue::FrameGraphResourceHandle m_buffer;
    cue::PresentToSwapChainPass m_present;
    cue::ImGuiPass m_ui;
};

/// @brief Graph 外の読み戻しだけに必要な State 遷移を記録する
void transition(ID3D12GraphicsCommandList &a_list, ID3D12Resource &a_resource, D3D12_RESOURCE_STATES a_before,
                D3D12_RESOURCE_STATES a_after)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = &a_resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = a_before;
    barrier.Transition.StateAfter = a_after;
    a_list.ResourceBarrier(1, &barrier);
}

/// @brief Present 前の実画素を読み戻し、公式 Draw と Heap 再設定が有効だったか確認する
bool check_pixels(cue::dx12::DX12Backend &a_backend, bool a_isRestored)
{
    auto *device = dynamic_cast<cue::dx12::DX12RenderDevice *>(a_backend.get_render_device());
    auto *swap = a_backend.get_swap_chain();
    auto *buffer = swap->back_buffer(swap->current_index());
    const auto desc = buffer->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
    UINT64 bytes = 0;
    device->device()->GetCopyableFootprints(&desc, 0, 1, 0, &layout, nullptr, nullptr, &bytes);
    auto read = cue::dx12::DX12GpuResource::create_buffer(*device->device(), {bytes, cue::GpuMemoryUsage::Readback},
                                                          L"ImGui Backend Test Readback");
    auto acquired = a_backend.get_command_pool()->acquire(cue::QueueType::Graphics);
    if (!read.has_value() || !acquired.has_value())
    {
        return false;
    }
    auto resource = read.take_value();
    auto command = acquired.take_value();
    auto *context = dynamic_cast<cue::dx12::DX12GpuCommandContext *>(command.get());
    auto *list = context->command_list();
    transition(*list, *buffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = buffer;
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = resource->resource();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = layout;
    list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    transition(*list, *buffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
    if (!command->close().has_value())
    {
        return false;
    }
    auto submitted = a_backend.get_command_pool()->submit(*swap->graphics_queue(), *command);
    if (!submitted.has_value() || !(*submitted.try_value())->wait().has_value())
    {
        return false;
    }
    std::vector<std::byte> pixels(static_cast<std::size_t>(bytes));
    if (!resource->read(0, pixels).has_value())
    {
        return false;
    }
    const auto offset = 30 * layout.Footprint.RowPitch + 30 * 4;
    const std::array<int, 3> expected = a_isRestored ? std::array{51, 102, 153} : std::array{250, 20, 30};
    for (std::size_t channel = 0; channel < expected.size(); ++channel)
    {
        if (std::to_integer<int>(pixels[offset + channel]) != expected[channel])
        {
            std::fprintf(stderr, "pixel channel %zu = %d, expected %d\n", channel,
                         std::to_integer<int>(pixels[offset + channel]), expected[channel]);
            return false;
        }
    }
    return true;
}

/// @brief Debug Layer の重大 Error と、全 Owner 停止後の残存 GPU Object を検出する
bool check_messages(ID3D12InfoQueue &a_queue, bool a_checkLeaks)
{
    bool valid = true;
    for (UINT64 index = 0; index < a_queue.GetNumStoredMessagesAllowedByRetrievalFilter(); ++index)
    {
        SIZE_T size = 0;
        a_queue.GetMessage(index, nullptr, &size);
        std::vector<std::byte> storage(size);
        auto *message = reinterpret_cast<D3D12_MESSAGE *>(storage.data());
        if (FAILED(a_queue.GetMessage(index, message, &size)))
        {
            return false;
        }
        const bool leak = a_checkLeaks && std::strstr(message->pDescription, "Live ID3D12") &&
                          !std::strstr(message->pDescription, "Live ID3D12Device ");
        if (leak || message->Severity == D3D12_MESSAGE_SEVERITY_ERROR ||
            message->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION)
        {
            std::fprintf(stderr, "%s\n", message->pDescription);
            valid = false;
        }
    }
    return valid;
}

/// @brief 専用 Heap の不足回復と返却再利用を実 Graph / 公式 Texture Upload で検証する
int run_case(std::uint32_t a_capacity)
{
    const auto frameCount = a_capacity;
    std::array<ImTextureData, 2> textures;
    auto systemResult = cue::create_windows_window_system();
    if (!systemResult.has_value())
    {
        return 1;
    }
    auto system = systemResult.take_value();
    auto windowResult = system->create_window({"ImGui DX12 Backend Test", {96, 96}});
    if (!windowResult.has_value())
    {
        return 2;
    }
    auto window = windowResult.take_value();
    cue::ImGuiManagerConfig config;
    config.settingsFile.clear();
    config.rendererDescriptorCapacity = a_capacity;
    auto backendResult = cue::dx12::DX12Backend::create();
    auto managerResult = cue::ImGuiManager::create(*window, config);
    if (!backendResult.has_value() || !managerResult.has_value())
    {
        return 3;
    }
    auto backend = backendResult.take_value();
    auto manager = managerResult.take_value();
    // SwapChain 未生成の Backend は借用せず、失敗後も CPU Context を維持する
    if (manager->initialize_renderer(*backend, 2).has_value())
    {
        return 4;
    }
    auto handle = cue::borrow_windows_window_handle(*window);
    if (!handle.has_value() || !backend->create_swap_chain(handle.take_value(), {96, 96, 2}).has_value())
    {
        return 5;
    }
    if (manager->initialize_renderer(*backend, 0).has_value() ||
        manager->initialize_renderer(*backend, 3).has_value() ||
        !manager->initialize_renderer(*backend, frameCount).has_value() ||
        manager->initialize_renderer(*backend, frameCount).has_value())
    {
        return 6;
    }
    auto *device = dynamic_cast<cue::dx12::DX12RenderDevice *>(backend->get_render_device());
    Microsoft::WRL::ComPtr<ID3D12Device> probe = device->device();
    Microsoft::WRL::ComPtr<ID3D12InfoQueue> infoQueue;
    if (FAILED(probe.As(&infoQueue)))
    {
        return 7;
    }
    infoQueue->ClearStoredMessages();
    bool restore = false;
    cue::dx12::DX12MainFrameGraphConfig graphConfig;
    graphConfig.frameCount = frameCount;
    graphConfig.clearColor = {0.2f, 0.4f, 0.6f, 1.0f};
    graphConfig.displayPass = std::make_unique<BackendPass>(*manager, restore);
    auto graphResult = cue::dx12::DX12MainFrameGraph::create(*backend->get_resource_context(),
                                                             *backend->get_swap_chain(), std::move(graphConfig));
    if (!graphResult.has_value())
    {
        return 8;
    }
    auto graph = graphResult.take_value();
    bool firstRegistered = false;
    for (int frame = 0; frame < 4; ++frame)
    {
        restore = frame == 3;
        auto built = manager->build_frame(
            [&]()
            {
                if (frame == 0)
                {
                    textures[0].Create(ImTextureFormat_RGBA32, 4, 4);
                    ImGui::RegisterUserTexture(&textures[0]);
                    firstRegistered = true;
                }
                if (frame == 1)
                {
                    // Capacity 1 は未生成 Texture を取り下げ、Font だけで再試行する
                    // Capacity 2 は旧 GPU Texture を返し、その Slot に次の Texture を作る
                    textures[0].WantDestroyNextFrame = true;
                    textures[0].SetStatus(ImTextureStatus_WantDestroy);
                    textures[0].UnusedFrames = 2;
                    if (a_capacity == 2)
                    {
                        textures[1].Create(ImTextureFormat_RGBA32, 4, 4);
                        ImGui::RegisterUserTexture(&textures[1]);
                    }
                }
                if (frame == 2 && a_capacity == 2)
                {
                    // 生成済み Texture の変更領域を公式 Upload に通す
                    textures[1].UpdateRect = {0, 0, 4, 4};
                    std::memset(textures[1].Pixels, 255, 4 * 4 * 4);
                    textures[1].SetStatus(ImTextureStatus_WantUpdates);
                }
                ImGui::GetBackgroundDrawList()->AddRectFilled({10, 10}, {60, 60}, IM_COL32(250, 20, 30, 255));
                return cue::Result<void>::success();
            });
        if (!built.has_value())
        {
            return 9;
        }
        auto executed =
            graph->execute(static_cast<std::uint32_t>(frame) % frameCount, *backend->get_execution_context());
        if (a_capacity == 1 && frame == 0)
        {
            auto stats = manager->renderer_info();
            if (executed.has_value() || executed.try_error()->operation != "ImGuiDX12.descriptor.capacity" ||
                !stats.has_value() || stats.try_value()->activeDescriptors != 0)
            {
                return 10;
            }
            continue;
        }
        if (!executed.has_value() || !*executed.try_value() || !check_pixels(*backend, restore))
        {
            if (!executed.has_value())
            {
                std::fprintf(stderr, "frame %d: %s\n", frame, executed.try_error()->operation.c_str());
            }
            return 11;
        }
        if (!backend->get_swap_chain()->present().has_value())
        {
            return 12;
        }
    }
    auto stats = manager->renderer_info();
    const auto expected = a_capacity == 1 ? 1u : 2u;
    if (!firstRegistered || !stats.has_value() || stats.try_value()->frameCount != frameCount ||
        stats.try_value()->activeDescriptors != expected ||
        stats.try_value()->recordedFrames != (a_capacity == 1 ? 3u : 4u) || textures[0].BackendUserData != nullptr ||
        (a_capacity == 2 && textures[1].Status != ImTextureStatus_OK))
    {
        return 13;
    }
    bool rejectedThread = false;
    std::thread worker(
        [&]()
        {
            auto init = manager->initialize_renderer(*backend, 2);
            auto snapshot = manager->renderer_info();
            rejectedThread = !init.has_value() && !snapshot.has_value() &&
                             init.try_error()->category == cue::ErrorCategory::WrongThread &&
                             snapshot.try_error()->category == cue::ErrorCategory::WrongThread;
        });
    worker.join();
    if (!rejectedThread || !check_messages(*infoQueue.Get(), false))
    {
        return 14;
    }
    if (!graph->shutdown().has_value())
    {
        return 15;
    }
    graph.reset();
    auto managerStopped = manager->shutdown();
    if (!managerStopped.has_value() || manager->renderer_info().has_value() || textures[1].BackendUserData != nullptr)
    {
        return 16;
    }
    manager.reset();
    // CPU Frame を中断した場合も Legacy Atlas は生成済みなので後付けを拒否する
    auto cpuManagerResult = cue::ImGuiManager::create(*window, config);
    if (!cpuManagerResult.has_value())
    {
        return 20;
    }
    auto cpuManager = cpuManagerResult.take_value();
    auto cancelled = cpuManager->build_frame(
        []() { return cue::Result<void>::failure({cue::ErrorCategory::InvalidState, "Test.cancel_cpu_frame"}); });
    auto late = cpuManager->initialize_renderer(*backend, frameCount);
    if (cancelled.has_value() || late.has_value() || late.try_error()->category != cue::ErrorCategory::InvalidState ||
        !cpuManager->build_frame().has_value() || !cpuManager->shutdown().has_value())
    {
        return 21;
    }
    cpuManager.reset();
    // Leak 検査用 Device を保持した正常停止は Live Device Warning を発生させる
    // 対話 Break の代わりに、下記の Message 検査で Error と残存 Object を失敗にする
    if (FAILED(infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, false)))
    {
        return 17;
    }
    if (!backend->shutdown().has_value())
    {
        return 17;
    }
    backend.reset();
    if (!check_messages(*infoQueue.Get(), false))
    {
        return 18;
    }
    infoQueue->ClearStoredMessages();
    Microsoft::WRL::ComPtr<ID3D12DebugDevice> debug;
    if (FAILED(probe.As(&debug)) ||
        FAILED(debug->ReportLiveDeviceObjects(
            static_cast<D3D12_RLDO_FLAGS>(D3D12_RLDO_DETAIL | D3D12_RLDO_IGNORE_INTERNAL))) ||
        !check_messages(*infoQueue.Get(), true))
    {
        return 18;
    }
    if (!window->destroy().has_value() || !system->pump_events().has_value())
    {
        return 19;
    }
    return 0;
}
} // namespace

/// @brief 公式 GPU Backend の描画、動的 Texture と Descriptor 不足からの回復を確認する
int main()
{
    for (const auto capacity : {1u, 2u})
    {
        if (const auto result = run_case(capacity); result != 0)
        {
            std::fprintf(stderr, "capacity %u, failure %d\n", capacity, result);
            return static_cast<int>(capacity * 30) + result;
        }
    }
    return 0;
}
