#include <EditorHost/ImGuiManager.h>

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <stop_token>
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
bool check_pixel_color(cue::dx12::DX12Backend &a_backend, const std::array<int, 3> &a_expected)
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
    for (std::size_t channel = 0; channel < a_expected.size(); ++channel)
    {
        if (std::to_integer<int>(pixels[offset + channel]) != a_expected[channel])
        {
            std::fprintf(stderr, "pixel channel %zu = %d, expected %d\n", channel,
                         std::to_integer<int>(pixels[offset + channel]), a_expected[channel]);
            return false;
        }
    }
    return true;
}

/// @brief 従来の UI 描画と標準表示への復帰を既定の色で確認する
bool check_pixels(cue::dx12::DX12Backend &a_backend, bool a_isRestored)
{
    return check_pixel_color(a_backend, a_isRestored ? std::array{51, 102, 153} : std::array{250, 20, 30});
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

/// @brief 選択した Adapter の説明と WARP 判定を記録し、明示した WARP 選択を検証する
bool check_adapter(cue::dx12::DX12Backend &a_backend, cue::dx12::AdapterSelection a_selection)
{
    auto *device = dynamic_cast<cue::dx12::DX12RenderDevice *>(a_backend.get_render_device());
    DXGI_ADAPTER_DESC1 desc{};
    if (!device || !device->adapter() || FAILED(device->adapter()->GetDesc1(&desc)))
    {
        return false;
    }
    std::wcout << L"adapter: " << desc.Description << L", is_warp=" << device->is_warp() << L'\n';
    return a_selection != cue::dx12::AdapterSelection::Warp || device->is_warp();
}

/// @brief 専用 Heap の不足回復と返却再利用を実 Graph / 公式 Texture Upload で検証する
int run_case(std::uint32_t a_capacity, cue::dx12::AdapterSelection a_selection)
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
    auto backendResult = cue::dx12::DX12Backend::create(a_selection);
    auto managerResult = cue::ImGuiManager::create(*window, config);
    if (!backendResult.has_value() || !managerResult.has_value())
    {
        return 3;
    }
    auto backend = backendResult.take_value();
    auto manager = managerResult.take_value();
    if (!check_adapter(*backend, a_selection))
    {
        return 22;
    }
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
#if defined(_DEBUG)
    auto *device = dynamic_cast<cue::dx12::DX12RenderDevice *>(backend->get_render_device());
    Microsoft::WRL::ComPtr<ID3D12Device> probe = device->device();
    Microsoft::WRL::ComPtr<ID3D12InfoQueue> infoQueue;
    if (FAILED(probe.As(&infoQueue)))
    {
        return 7;
    }
    infoQueue->ClearStoredMessages();
#endif
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
    if (!rejectedThread)
    {
        return 14;
    }
#if defined(_DEBUG)
    if (!check_messages(*infoQueue.Get(), false))
    {
        return 14;
    }
#endif
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
#if defined(_DEBUG)
    if (FAILED(infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, false)))
    {
        return 17;
    }
#endif
    if (!backend->shutdown().has_value())
    {
        return 17;
    }
    backend.reset();
#if defined(_DEBUG)
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
#endif
    if (!window->destroy().has_value() || !system->pump_events().has_value())
    {
        return 19;
    }
    return 0;
}

/// @brief 先に確定した二つの UI Frame を固定 Render Worker へ渡し、旧 Snapshot が上書きされないことを画素で確認する
int run_transfer_case(cue::dx12::AdapterSelection a_selection)
{
    // 異常終了経路でも Manager より後に Texture を破棄し、登録先の参照を保つ
    ImTextureData texture;
    auto systemResult = cue::create_windows_window_system();
    if (!systemResult.has_value())
    {
        return 1;
    }
    auto system = systemResult.take_value();
    auto windowResult = system->create_window({"ImGui DX12 Transfer Test", {96, 96}});
    if (!windowResult.has_value())
    {
        return 2;
    }
    auto window = windowResult.take_value();
    cue::ImGuiManagerConfig config;
    config.settingsFile.clear();
    auto managerResult = cue::ImGuiManager::create(*window, config);
    auto backendResult = cue::dx12::DX12Backend::create(a_selection);
    if (!managerResult.has_value() || !backendResult.has_value())
    {
        return 3;
    }
    auto manager = managerResult.take_value();
    auto backend = backendResult.take_value();
    if (!check_adapter(*backend, a_selection))
    {
        return 21;
    }
    auto handle = cue::borrow_windows_window_handle(*window);
    if (!handle.has_value() || !backend->create_swap_chain(handle.take_value(), {96, 96, 2}).has_value() ||
        !manager->initialize_renderer(*backend, 2).has_value() || !manager->enable_frame_transfer().has_value())
    {
        return 4;
    }
#if defined(_DEBUG)
    auto *device = dynamic_cast<cue::dx12::DX12RenderDevice *>(backend->get_render_device());
    Microsoft::WRL::ComPtr<ID3D12Device> probe = device->device();
    Microsoft::WRL::ComPtr<ID3D12InfoQueue> infoQueue;
    if (FAILED(probe.As(&infoQueue)))
    {
        return 25;
    }
    infoQueue->ClearStoredMessages();
#endif

    cue::dx12::DX12MainFrameGraphConfig graphConfig;
    graphConfig.frameCount = 2;
    graphConfig.displayPass = std::make_unique<cue::ImGuiPass>(std::make_unique<ManagerRenderer>(*manager));
    auto graphResult = cue::dx12::DX12MainFrameGraph::create(*backend->get_resource_context(),
                                                             *backend->get_swap_chain(), std::move(graphConfig));
    if (!graphResult.has_value())
    {
        return 5;
    }
    auto graph = graphResult.take_value();
    constexpr std::array<std::array<int, 3>, 2> colors{{{250, 20, 30}, {20, 220, 40}}};
    for (std::uint64_t frame = 0; frame < colors.size(); ++frame)
    {
        auto built = manager->build_frame(
            [&]()
            {
                const auto &color = colors[frame];
                ImGui::GetBackgroundDrawList()->AddRectFilled({10, 10}, {60, 60},
                                                              IM_COL32(color[0], color[1], color[2], 255));
                return cue::Result<void>::success();
            });
        if (!built.has_value() || !manager->publish_frame(frame).has_value())
        {
            return 6;
        }
    }
    auto pending = manager->transfer_info();
    if (!pending.has_value() || pending.try_value()->publishedFrames != 2 || pending.try_value()->pendingFrames != 2 ||
        pending.try_value()->consumedFrames != 0)
    {
        return 7;
    }

    std::mutex gateMutex;
    std::condition_variable gate;
    bool firstBatchDone = false;
    bool secondBatchReady = false;
    bool isSecondBatchAborted = false;
    bool renderEntered = false;
    bool mainUiBuilt = false;
    int workerFailure = 0;
    std::thread::id renderThreadId;
    std::thread worker(
        [&]()
        {
            auto finish_first_batch = [&]()
            {
                {
                    std::lock_guard lock(gateMutex);
                    firstBatchDone = true;
                }
                gate.notify_one();
            };
            renderThreadId = std::this_thread::get_id();
            for (std::uint64_t frame = 0; frame < colors.size(); ++frame)
            {
                auto rendered = manager->render_frame(
                    frame, {},
                    [&](std::uint64_t a_frame, std::stop_token a_token)
                    {
                        if (a_frame == 0)
                        {
                            // 通常 Graph Callback の待機中に Main が Context を取得できることを確認する
                            // 全 Graph を Lock した回帰でも無期限に止めず、失敗として回収する
                            std::unique_lock lock(gateMutex);
                            renderEntered = true;
                            gate.notify_one();
                            if (!gate.wait_for(lock, std::chrono::seconds(2), [&]() { return mainUiBuilt; }))
                            {
                                return cue::Result<void>::failure(
                                    {cue::ErrorCategory::InvalidState, "Test.transfer.graph_context_lock"});
                            }
                        }
                        if (a_frame != frame || a_token.stop_requested())
                        {
                            return cue::Result<void>::failure(
                                {cue::ErrorCategory::InvalidState, "Test.transfer.frame"});
                        }
                        auto executed =
                            graph->execute(static_cast<std::uint32_t>(a_frame % 2), *backend->get_execution_context());
                        if (!executed.has_value())
                        {
                            return cue::Result<void>::failure(*executed.try_error());
                        }
                        return *executed.try_value() ? cue::Result<void>::success()
                                                     : cue::Result<void>::failure(
                                                           {cue::ErrorCategory::InvalidState, "Test.transfer.skip"});
                    });
                if (!rendered.has_value())
                {
                    std::fprintf(stderr, "transfer frame %llu: %s\n", static_cast<unsigned long long>(frame),
                                 rendered.try_error()->operation.c_str());
                    workerFailure = 8;
                    finish_first_batch();
                    return;
                }
                // Present は render_frame Scope の外で行い、各枠の提出画素を個別に確認する
                if (!check_pixel_color(*backend, colors[frame]))
                {
                    workerFailure = 9;
                    finish_first_batch();
                    return;
                }
                if (!backend->get_swap_chain()->present().has_value())
                {
                    workerFailure = 10;
                    finish_first_batch();
                    return;
                }
            }
            finish_first_batch();
            {
                std::unique_lock lock(gateMutex);
                gate.wait(lock, [&]() { return secondBatchReady; });
                if (isSecondBatchAborted)
                {
                    return;
                }
            }
            // 停止 Token による Skip でも Callback を呼び、GPU 未提出のまま Snapshot を回収する
            std::stop_source cancelled;
            cancelled.request_stop();
            auto skipped =
                manager->render_frame(2, cancelled.get_token(),
                                      [&](std::uint64_t a_frame, std::stop_token a_token)
                                      {
                                          return a_frame == 2 && a_token.stop_requested()
                                                     ? cue::Result<void>::success()
                                                     : cue::Result<void>::failure({cue::ErrorCategory::InvalidState,
                                                                                   "Test.transfer.cancel_token"});
                                      });
            if (!skipped.has_value() || texture.QueueUserData == nullptr)
            {
                workerFailure = 15;
                return;
            }
            auto failed = manager->render_frame(
                3, {},
                [&](std::uint64_t, std::stop_token)
                {
                    // 範囲外の Graph 枠で実際の Graph Error を返し、Render Scope の後始末を確認する
                    auto rejected = graph->execute(2, *backend->get_execution_context());
                    return rejected.has_value() ? cue::Result<void>::failure({cue::ErrorCategory::InvalidState,
                                                                              "Test.transfer.expected_graph_failure"})
                                                : cue::Result<void>::failure(*rejected.try_error());
                });
            if (failed.has_value() || failed.try_error()->operation != "DX12MainFrameGraph.execute_queues" ||
                texture.QueueUserData != nullptr)
            {
                workerFailure = 16;
            }
        });
    {
        std::unique_lock lock(gateMutex);
        gate.wait(lock, [&]() { return renderEntered; });
    }
    auto concurrentBuilt = manager->build_frame(
        []()
        {
            ImGui::GetBackgroundDrawList()->AddRectFilled({10, 10}, {60, 60}, IM_COL32(40, 50, 60, 255));
            return cue::Result<void>::success();
        });
    {
        std::lock_guard lock(gateMutex);
        mainUiBuilt = concurrentBuilt.has_value();
    }
    gate.notify_one();
    {
        std::unique_lock lock(gateMutex);
        gate.wait(lock, [&]() { return firstBatchDone; });
    }
    if (workerFailure != 0)
    {
        worker.join();
        return workerFailure;
    }
    auto firstTransfer = manager->transfer_info();
    auto renderer = manager->renderer_info();
    auto timing = manager->timing_info();
    if (!concurrentBuilt.has_value() || !timing.has_value() || !timing.try_value()->uiBuild.sampleCount ||
        timing.try_value()->snapshotCopy.sampleCount != 2 || !timing.try_value()->contextWait.sampleCount ||
        !firstTransfer.has_value() || !renderer.has_value() || firstTransfer.try_value()->publishedFrames != 2 ||
        firstTransfer.try_value()->consumedFrames != 2 || firstTransfer.try_value()->discardedFrames != 0 ||
        firstTransfer.try_value()->pendingFrames != 0 || firstTransfer.try_value()->renderThreadId != renderThreadId ||
        renderThreadId == std::this_thread::get_id() || renderer.try_value()->recordedFrames != 2)
    {
        {
            std::lock_guard lock(gateMutex);
            secondBatchReady = true;
            isSecondBatchAborted = true;
        }
        gate.notify_one();
        worker.join();
        return 11;
    }
    bool publishedSecondBatch = true;
    for (std::uint64_t frame = 2; frame < 4; ++frame)
    {
        auto built = manager->build_frame(
            [&]()
            {
                if (frame == 2)
                {
                    texture.Create(ImTextureFormat_RGBA32, 4, 4);
                    std::memset(texture.Pixels, 255, 4 * 4 * 4);
                    ImGui::RegisterUserTexture(&texture);
                }
                ImGui::GetBackgroundDrawList()->AddImage(texture.GetTexRef(), {10, 10}, {60, 60});
                return cue::Result<void>::success();
            });
        if (!built.has_value() || !manager->publish_frame(frame).has_value())
        {
            publishedSecondBatch = false;
            break;
        }
    }
    auto queued = manager->transfer_info();
    if (!queued.has_value() || queued.try_value()->pendingFrames != 2 || texture.QueueUserData == nullptr)
    {
        publishedSecondBatch = false;
    }
    {
        std::lock_guard lock(gateMutex);
        secondBatchReady = true;
        isSecondBatchAborted = !publishedSecondBatch;
    }
    gate.notify_one();
    worker.join();
    if (!publishedSecondBatch || workerFailure != 0)
    {
        return publishedSecondBatch ? workerFailure : 17;
    }
    auto transfer = manager->transfer_info();
    if (!transfer.has_value() || transfer.try_value()->publishedFrames != 4 ||
        transfer.try_value()->consumedFrames != 4 || transfer.try_value()->discardedFrames != 2 ||
        transfer.try_value()->pendingFrames != 0 || texture.QueueUserData != nullptr)
    {
        return 18;
    }

    // 未消費の Frame は停止時に回収し、登録 Texture の Queue Pin を残さない
    auto lastBuilt = manager->build_frame(
        [&]()
        {
            ImGui::GetBackgroundDrawList()->AddImage(texture.GetTexRef(), {10, 10}, {60, 60});
            return cue::Result<void>::success();
        });
    if (!lastBuilt.has_value() || !manager->publish_frame(4).has_value() || texture.QueueUserData == nullptr)
    {
        return 19;
    }
    auto reused = manager->transfer_info();
    if (!reused.has_value() || reused.try_value()->snapshotAllocations != transfer.try_value()->snapshotAllocations ||
        reused.try_value()->copiedBytes <= transfer.try_value()->copiedBytes)
    {
        // 同じ Image UI の枠を再利用し、Copy を維持しながら出力 Buffer の追加確保がないことを確認する
        return 26;
    }
    auto updateBuilt = manager->build_frame(
        [&]()
        {
            // 前の Snapshot が借用中の Texture 更新を、停止済み待機として取消可能にする
            texture.UpdateRect = {0, 0, 4, 4};
            std::memset(texture.Pixels, 128, 4 * 4 * 4);
            texture.SetStatus(ImTextureStatus_WantUpdates);
            ImGui::GetBackgroundDrawList()->AddImage(texture.GetTexRef(), {10, 10}, {60, 60});
            return cue::Result<void>::success();
        });
    std::stop_source updateCancelled;
    updateCancelled.request_stop();
    auto cancelledPublish =
        updateBuilt.has_value()
            ? manager->publish_frame(5, updateCancelled.get_token())
            : cue::Result<void>::failure({cue::ErrorCategory::InvalidState, "Test.transfer.update_build"});
    auto beforeShutdown = manager->transfer_info();
    if (cancelledPublish.has_value() || cancelledPublish.try_error()->operation != "ImGuiManager.publish.cancelled" ||
        !beforeShutdown.has_value() || beforeShutdown.try_value()->publishedFrames != 5 ||
        beforeShutdown.try_value()->pendingFrames != 1 || texture.QueueUserData == nullptr)
    {
        return 20;
    }
    if (!graph->shutdown().has_value())
    {
        return 12;
    }
    graph.reset();
    if (!manager->shutdown().has_value() || texture.QueueUserData != nullptr || texture.BackendUserData != nullptr)
    {
        return 13;
    }
    manager.reset();
#if defined(_DEBUG)
    // 検査用 Device の保持だけを許容し、Worker 描画の資源も停止後に検査する
    if (!check_messages(*infoQueue.Get(), false) ||
        FAILED(infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, false)))
    {
        return 25;
    }
#endif
    if (!backend->shutdown().has_value())
    {
        return 13;
    }
    backend.reset();
#if defined(_DEBUG)
    if (!check_messages(*infoQueue.Get(), false))
    {
        return 25;
    }
    infoQueue->ClearStoredMessages();
    Microsoft::WRL::ComPtr<ID3D12DebugDevice> debug;
    if (FAILED(probe.As(&debug)) ||
        FAILED(debug->ReportLiveDeviceObjects(
            static_cast<D3D12_RLDO_FLAGS>(D3D12_RLDO_DETAIL | D3D12_RLDO_IGNORE_INTERNAL))) ||
        !check_messages(*infoQueue.Get(), true))
    {
        return 25;
    }
#endif
    if (!window->destroy().has_value() || !system->pump_events().has_value())
    {
        return 14;
    }
    return 0;
}
} // namespace

/// @brief 公式 GPU Backend の描画、動的 Texture と Descriptor 不足からの回復を確認する
int main(int a_argumentCount, char **a_arguments)
{
    if (a_argumentCount > 2 || (a_argumentCount == 2 && std::strcmp(a_arguments[1], "--warp") != 0))
    {
        std::fprintf(stderr, "usage: ImGuiDX12BackendTests [--warp]\n");
        return 1;
    }
    const auto selection =
        a_argumentCount == 2 ? cue::dx12::AdapterSelection::Warp : cue::dx12::AdapterSelection::HardwarePreferred;
    for (const auto capacity : {1u, 2u})
    {
        if (const auto result = run_case(capacity, selection); result != 0)
        {
            std::fprintf(stderr, "capacity %u, failure %d\n", capacity, result);
            return static_cast<int>(capacity * 30) + result;
        }
    }
    if (const auto result = run_transfer_case(selection); result != 0)
    {
        std::fprintf(stderr, "transfer failure %d\n", result);
        return 100 + result;
    }
    return 0;
}
