#include <DX12/DX12MainFrameGraph.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include <DX12/DX12CommandPool.h>
#include <DX12/DX12DescriptorAllocator.h>
#include <DX12/DX12GpuResourcePool.h>
#include <DX12/DX12GpuResource.h>
#include <DX12/DX12QueuePool.h>
#include <DX12/DX12RenderDevice.h>
#include <DX12/DX12SwapChain.h>
#include <Platform/Windows/WindowsPlatform.h>

namespace
{
/// @brief 追加 Pass が Graph の Resource 宣言と記録契約を通ることを確認する
class TestPass final : public cue::FrameGraphPass
{
public:
    /// @brief Graph の論理 Texture と実行回数を保持する
    TestPass(cue::FrameGraphResourceHandle a_color, int& a_count, bool& a_enabled) noexcept
        : m_color(a_color), m_count(&a_count), m_enabled(&a_enabled)
    {
    }

    [[nodiscard]] const char* name() const noexcept override { return "AfterClear"; }
    [[nodiscard]] cue::QueueType type() const noexcept override { return cue::QueueType::Graphics; }
    [[nodiscard]] bool is_enabled() const noexcept override { return *m_enabled; }
    [[nodiscard]] cue::Result<void> setup(cue::FrameGraphBuilder&) override
    {
        return cue::Result<void>::success();
    }
    [[nodiscard]] cue::Result<void> describe_resources(cue::FrameGraphBuilder& a_builder) override
    {
        return a_builder.use(m_color, cue::FrameGraphAccess::Read,
                             cue::FrameGraphResourceState::ShaderRead);
    }
    [[nodiscard]] cue::Result<void> execute(cue::FrameGraphContext& a_context) override
    {
        if (a_context.command_context().type() != cue::QueueType::Graphics)
        {
            return cue::Result<void>::failure({cue::ErrorCategory::InvalidState, "TestPass.execute"});
        }
        ++*m_count;
        return cue::Result<void>::success();
    }

private:
    cue::FrameGraphResourceHandle m_color;
    int* m_count = nullptr;
    bool* m_enabled = nullptr;
};

/// @brief Pool 所有 Buffer の世代付き Handle を Graph へ取り込む
class PoolPass final : public cue::FrameGraphPass
{
public:
    /// @brief Pool の寿命を Graph より長く保つ呼出側から借用する
    PoolPass(cue::IGpuResourcePool& a_pool, cue::GpuResourceHandle a_handle, int& a_count) noexcept
        : m_pool(&a_pool), m_handle(a_handle), m_count(&a_count)
    {
    }

    [[nodiscard]] const char* name() const noexcept override { return "PoolRead"; }
    [[nodiscard]] cue::QueueType type() const noexcept override { return cue::QueueType::Copy; }
    [[nodiscard]] cue::Result<void> setup(cue::FrameGraphBuilder& a_builder) override
    {
        auto result = a_builder.import_pool_buffer("PoolBuffer", *m_pool, m_handle, {64},
            cue::FrameGraphResourceState::Common, cue::FrameGraphResourceState::Common);
        if (!result.has_value())
        {
            return cue::Result<void>::failure(*result.try_error());
        }
        m_buffer = result.take_value();
        return cue::Result<void>::success();
    }
    [[nodiscard]] cue::Result<void> describe_resources(cue::FrameGraphBuilder& a_builder) override
    {
        return a_builder.use(m_buffer, cue::FrameGraphAccess::Read,
                             cue::FrameGraphResourceState::CopySource);
    }
    [[nodiscard]] cue::Result<void> execute(cue::FrameGraphContext& a_context) override
    {
        if (a_context.command_context().type() != cue::QueueType::Copy)
        {
            return cue::Result<void>::failure({cue::ErrorCategory::InvalidState, "PoolPass.execute"});
        }
        ++*m_count;
        return cue::Result<void>::success();
    }

private:
    cue::IGpuResourcePool* m_pool = nullptr;
    cue::GpuResourceHandle m_handle;
    cue::FrameGraphResourceHandle m_buffer;
    int* m_count = nullptr;
};

/// @brief Pool 所有 Texture を RenderTarget として使い、記録前の RTV 準備を通す
class PoolTexturePass final : public cue::FrameGraphPass
{
public:
    PoolTexturePass(cue::IGpuResourcePool& a_pool, cue::GpuResourceHandle a_handle) noexcept
        : m_pool(&a_pool), m_handle(a_handle) {}

    [[nodiscard]] const char* name() const noexcept override { return "PoolTextureWrite"; }
    [[nodiscard]] cue::QueueType type() const noexcept override { return cue::QueueType::Graphics; }
    [[nodiscard]] cue::Result<void> setup(cue::FrameGraphBuilder& a_builder) override
    {
        cue::GpuTexture2DDesc desc{64, 64};
        desc.isRenderTarget = true;
        desc.isShaderReadable = true;
        desc.clearColor = {0.1f, 0.2f, 0.3f, 1.0f};
        auto result = a_builder.import_pool_texture2d("PoolTexture", *m_pool, m_handle, desc,
            cue::FrameGraphResourceState::Common, cue::FrameGraphResourceState::Common);
        if (!result.has_value()) return cue::Result<void>::failure(*result.try_error());
        m_texture = result.take_value();
        return cue::Result<void>::success();
    }
    [[nodiscard]] cue::Result<void> describe_resources(cue::FrameGraphBuilder& a_builder) override
    {
        return a_builder.use(m_texture, cue::FrameGraphAccess::Write,
                             cue::FrameGraphResourceState::RenderTarget);
    }
    [[nodiscard]] cue::Result<void> execute(cue::FrameGraphContext& a_context) override
    {
        return a_context.clear_render_target(m_texture, {0.1f, 0.2f, 0.3f, 1.0f});
    }

private:
    cue::IGpuResourcePool* m_pool = nullptr;
    cue::GpuResourceHandle m_handle;
    cue::FrameGraphResourceHandle m_texture;
};

/// @brief 同じ Pool Texture の ShaderRead 宣言で SRV の準備も通す
class PoolTextureReadPass final : public cue::FrameGraphPass
{
public:
    [[nodiscard]] const char* name() const noexcept override { return "PoolTextureRead"; }
    [[nodiscard]] cue::QueueType type() const noexcept override { return cue::QueueType::Graphics; }
    [[nodiscard]] cue::Result<void> setup(cue::FrameGraphBuilder& a_builder) override
    {
        auto result = a_builder.get_texture("PoolTexture");
        if (!result.has_value()) return cue::Result<void>::failure(*result.try_error());
        m_texture = result.take_value();
        return cue::Result<void>::success();
    }
    [[nodiscard]] cue::Result<void> describe_resources(cue::FrameGraphBuilder& a_builder) override
    {
        return a_builder.use(m_texture, cue::FrameGraphAccess::Read,
                             cue::FrameGraphResourceState::ShaderRead);
    }
    [[nodiscard]] cue::Result<void> execute(cue::FrameGraphContext&) override
    {
        return cue::Result<void>::success();
    }

private:
    cue::FrameGraphResourceHandle m_texture;
};

/// @brief Compute Queue の空 Pass でも Graph の提出順と完了点を確認する
class ComputePass final : public cue::FrameGraphPass
{
public:
    /// @brief 実行回数を呼出側で検証する
    explicit ComputePass(int& a_count) noexcept : m_count(&a_count) {}

    [[nodiscard]] const char* name() const noexcept override { return "Compute"; }
    [[nodiscard]] cue::QueueType type() const noexcept override { return cue::QueueType::Compute; }
    [[nodiscard]] cue::Result<void> setup(cue::FrameGraphBuilder&) override
    {
        return cue::Result<void>::success();
    }
    [[nodiscard]] cue::Result<void> describe_resources(cue::FrameGraphBuilder&) override
    {
        return cue::Result<void>::success();
    }
    [[nodiscard]] cue::Result<void> execute(cue::FrameGraphContext& a_context) override
    {
        if (a_context.command_context().type() != cue::QueueType::Compute)
        {
            return cue::Result<void>::failure({cue::ErrorCategory::InvalidState, "ComputePass.execute"});
        }
        ++*m_count;
        return cue::Result<void>::success();
    }

private:
    int* m_count = nullptr;
};

/// @brief Pass 自身の setup で作った RenderTexture に色を書き込む
class ProduceColorPass final : public cue::FrameGraphPass
{
public:
    [[nodiscard]] const char* name() const noexcept override { return "ProduceColor"; }
    [[nodiscard]] cue::QueueType type() const noexcept override { return cue::QueueType::Graphics; }
    [[nodiscard]] cue::Result<void> setup(cue::FrameGraphBuilder& a_builder) override
    {
        cue::GpuTexture2DDesc desc{64, 64};
        desc.isRenderTarget = true;
        desc.clearColor = {0.8f, 0.1f, 0.3f, 1.0f};
        auto result = a_builder.create_transient_texture2d("PassAOutput", desc);
        if (!result.has_value())
        {
            return cue::Result<void>::failure(*result.try_error());
        }
        m_output = result.take_value();
        return cue::Result<void>::success();
    }
    [[nodiscard]] cue::Result<void> describe_resources(cue::FrameGraphBuilder& a_builder) override
    {
        return a_builder.use(m_output, cue::FrameGraphAccess::Write,
                             cue::FrameGraphResourceState::RenderTarget);
    }
    [[nodiscard]] cue::Result<void> execute(cue::FrameGraphContext& a_context) override
    {
        if (a_context.clear_render_target(m_output, {0.0f, 0.0f, 0.0f, 1.0f}).has_value())
        {
            return cue::Result<void>::failure({cue::ErrorCategory::InvalidState,
                                               "ProduceColorPass.unexpected_clear"});
        }
        auto bindResult = a_context.set_render_target(m_output);
        if (!bindResult.has_value())
        {
            return bindResult;
        }
        return a_context.clear_render_target(m_output, {0.8f, 0.1f, 0.3f, 1.0f});
    }

private:
    cue::FrameGraphResourceHandle m_output;
};

/// @brief 前の Pass の名前付き Texture を取得して FinalColor に複写する
class ConsumeColorPass final : public cue::FrameGraphPass
{
public:
    [[nodiscard]] const char* name() const noexcept override { return "ConsumeColor"; }
    [[nodiscard]] cue::QueueType type() const noexcept override { return cue::QueueType::Copy; }
    [[nodiscard]] cue::Result<void> setup(cue::FrameGraphBuilder& a_builder) override
    {
        auto sourceResult = a_builder.get_texture("PassAOutput");
        auto targetResult = a_builder.get_texture("FinalColorTexture");
        if (!sourceResult.has_value() || !targetResult.has_value())
        {
            return cue::Result<void>::failure({cue::ErrorCategory::InvalidState,
                                               "ConsumeColorPass.setup"});
        }
        m_source = sourceResult.take_value();
        m_target = targetResult.take_value();
        return cue::Result<void>::success();
    }
    [[nodiscard]] cue::Result<void> describe_resources(cue::FrameGraphBuilder& a_builder) override
    {
        auto sourceResult = a_builder.use(m_source, cue::FrameGraphAccess::Read,
                                          cue::FrameGraphResourceState::CopySource);
        if (!sourceResult.has_value())
        {
            return sourceResult;
        }
        return a_builder.use(m_target, cue::FrameGraphAccess::Write,
                             cue::FrameGraphResourceState::CopyDestination);
    }
    [[nodiscard]] cue::Result<void> execute(cue::FrameGraphContext& a_context) override
    {
        return a_context.copy_texture2d(m_source, m_target);
    }

private:
    cue::FrameGraphResourceHandle m_source;
    cue::FrameGraphResourceHandle m_target;
};

/// @brief 読み戻し用に Back Buffer の State を遷移させる
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

/// @brief 固定 Pass の State 計画と WARP の Back Buffer 画素を確認する
int run_tests()
{
    auto systemResult = cue::create_windows_window_system();
    if (!systemResult.has_value())
    {
        return 1;
    }
    auto system = systemResult.take_value();
    auto windowResult = system->create_window({"CueEngine Main Graph Test", {64, 64}});
    if (!windowResult.has_value())
    {
        return 2;
    }
    auto window = windowResult.take_value();
    if (!window->show().has_value())
    {
        return 3;
    }
    auto handleResult = cue::borrow_windows_window_handle(*window);
    auto deviceResult = cue::dx12::DX12RenderDevice::create(cue::dx12::AdapterSelection::Warp);
    if (!handleResult.has_value() || !deviceResult.has_value())
    {
        return 4;
    }
    auto device = deviceResult.take_value();
    auto rtvResult = cue::dx12::DX12DescriptorAllocator::create(*device->device(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 8);
    auto srvResult =
        cue::dx12::DX12DescriptorAllocator::create(*device->device(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 8, true);
    auto queueResult = cue::dx12::DX12GpuCommandQueue::create(*device->device(), cue::QueueType::Graphics, 0);
    if (!rtvResult.has_value() || !srvResult.has_value() || !queueResult.has_value())
    {
        return 5;
    }
    auto rtvAllocator = rtvResult.take_value();
    auto srvAllocator = srvResult.take_value();
    cue::queueLease queue(queueResult.take_value().release(), [](cue::IQueueContext *a_queue) { delete a_queue; });
    const cue::dx12::DX12SwapChainConfig config{64, 64, 2, DXGI_FORMAT_R8G8B8A8_UNORM, false, false};
    auto swapResult =
        cue::dx12::DX12SwapChain::create(*device, std::move(queue), *rtvAllocator, *handleResult.try_value(), config);
    if (!swapResult.has_value())
    {
        return 6;
    }
    auto swapChain = swapResult.take_value();
    const std::array<float, 4> clearColor{0.2f, 0.4f, 0.6f, 1.0f};
    auto graphResult =
        cue::dx12::DX12MainFrameGraph::create(*device, *swapChain, 2, *rtvAllocator, *srvAllocator, clearColor);
    if (!graphResult.has_value())
    {
        return 7;
    }
    auto graph = graphResult.take_value();
    const auto &plan = graph->plan();
    if (plan.passes().size() != 2 || plan.passes()[0].name != "ClearFinalColor" ||
        plan.passes()[1].name != "PresentToSwapChain" || plan.passes()[0].barriersBefore.size() != 1 ||
        plan.passes()[0].barriersBefore[0].after != cue::FrameGraphResourceState::RenderTarget ||
        plan.passes()[1].barriersBefore.size() != 2 ||
        plan.passes()[1].barriersBefore[0].after != cue::FrameGraphResourceState::ShaderRead ||
        plan.passes()[1].barriersBefore[1].before != cue::FrameGraphResourceState::Present ||
        plan.passes()[1].barriersBefore[1].after != cue::FrameGraphResourceState::RenderTarget ||
        plan.passes()[1].barriersAfter.size() != 1 ||
        plan.passes()[1].barriersAfter[0].after != cue::FrameGraphResourceState::Common ||
        plan.final_barriers().size() != 1 ||
        plan.final_barriers()[0].after != cue::FrameGraphResourceState::Present)
    {
        return 8;
    }
    const auto backIndex = swapChain->current_index();
    auto *backBuffer = swapChain->back_buffer(backIndex);
    if (!backBuffer || !swapChain->graphics_queue())
    {
        return 9;
    }
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
    UINT64 totalBytes = 0;
    const auto backDesc = backBuffer->GetDesc();
    device->device()->GetCopyableFootprints(&backDesc, 0, 1, 0, &layout, nullptr, nullptr, &totalBytes);
    auto readbackResult =
        cue::dx12::DX12GpuResource::create_buffer(*device->device(), {totalBytes, cue::GpuMemoryUsage::Readback});
    auto commandPoolResult = cue::dx12::DX12CommandPool::create(*device);
    if (!readbackResult.has_value() || !commandPoolResult.has_value())
    {
        return 10;
    }
    auto readback = readbackResult.take_value();
    auto commandPool = commandPoolResult.take_value();
    auto commandResult = commandPool->acquire(cue::QueueType::Graphics);
    if (!commandResult.has_value())
    {
        return 11;
    }
    auto command = commandResult.take_value();
    auto *context = dynamic_cast<cue::dx12::DX12GpuCommandContext *>(command.get());
    if (!context || !context->command_list() || graph->record(2, *context).has_value() ||
        !graph->record(0, *context).has_value())
    {
        return 12;
    }
    auto *list = context->command_list();
    transition(*list, *backBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = backBuffer;
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = readback->resource();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = layout;
    list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    transition(*list, *backBuffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
    if (!command->close().has_value())
    {
        return 13;
    }
    auto submitResult = commandPool->submit(*swapChain->graphics_queue(), *command);
    if (!submitResult.has_value())
    {
        return 14;
    }
    std::shared_ptr<cue::ICommandCompletion> completion(submitResult.take_value());
    if (!graph->mark_submitted(0, completion).has_value() || !completion->wait().has_value())
    {
        return 15;
    }
    std::vector<std::byte> pixels(static_cast<std::size_t>(totalBytes));
    if (!readback->read(0, pixels).has_value())
    {
        return 16;
    }
    const auto matches = [&pixels, &layout](std::size_t a_x, std::size_t a_y)
    {
        const auto offset = a_y * layout.Footprint.RowPitch + a_x * 4;
        return std::to_integer<int>(pixels[offset]) == 51 && std::to_integer<int>(pixels[offset + 1]) == 102 &&
               std::to_integer<int>(pixels[offset + 2]) == 153 && std::to_integer<int>(pixels[offset + 3]) == 255;
    };
    if (!matches(0, 0) || !matches(63, 0) || !matches(0, 63) || !matches(63, 63) || !swapChain->present().has_value())
    {
        return 17;
    }
    command.reset();
    if (!graph->shutdown().has_value() || !graph->shutdown().has_value())
    {
        return 18;
    }
    auto poolResult = cue::dx12::DX12GpuResourcePool::create(*device);
    auto queuePoolResult = cue::dx12::DX12QueuePool::create(*device);
    if (!poolResult.has_value() || !queuePoolResult.has_value())
    {
        return 19;
    }
    auto pool = poolResult.take_value();
    auto queuePool = queuePoolResult.take_value();
    auto poolBufferResult = pool->create_buffer({64});
    cue::GpuTexture2DDesc poolTextureDesc{64, 64};
    poolTextureDesc.isRenderTarget = true;
    poolTextureDesc.isShaderReadable = true;
    poolTextureDesc.clearColor = {0.1f, 0.2f, 0.3f, 1.0f};
    auto poolTextureResult = pool->create_texture2d(poolTextureDesc);
    if (!poolBufferResult.has_value() || !poolTextureResult.has_value())
    {
        return 19;
    }
    const auto poolBuffer = poolBufferResult.take_value();
    const auto poolTexture = poolTextureResult.take_value();
    int customCallCount = 0;
    bool customEnabled = true;
    int poolCallCount = 0;
    int computeCallCount = 0;
    auto extendedResult = cue::dx12::DX12MainFrameGraph::create(
        *device, *swapChain, 2, *rtvAllocator, *srvAllocator, clearColor,
        [&customCallCount, &customEnabled, &poolCallCount, &computeCallCount, &pool,
         poolBuffer, poolTexture](cue::FrameGraph& a_graph,
                           cue::FrameGraphResourceHandle a_finalColor) -> cue::Result<void>
        {
            auto testResult = a_graph.add_pass(std::make_unique<TestPass>(a_finalColor, customCallCount,
                                                                          customEnabled));
            if (!testResult.has_value())
            {
                return testResult;
            }
            auto produceResult = a_graph.add_pass(std::make_unique<ProduceColorPass>());
            if (!produceResult.has_value())
            {
                return produceResult;
            }
            auto consumeResult = a_graph.add_pass(std::make_unique<ConsumeColorPass>());
            if (!consumeResult.has_value())
            {
                return consumeResult;
            }
            auto poolResult = a_graph.add_pass(std::make_unique<PoolPass>(*pool, poolBuffer, poolCallCount));
            if (!poolResult.has_value())
            {
                return poolResult;
            }
            auto poolTextureWriteResult = a_graph.add_pass(
                std::make_unique<PoolTexturePass>(*pool, poolTexture));
            if (!poolTextureWriteResult.has_value()) return poolTextureWriteResult;
            auto poolTextureReadResult = a_graph.add_pass(std::make_unique<PoolTextureReadPass>());
            if (!poolTextureReadResult.has_value()) return poolTextureReadResult;
            return a_graph.add_pass(std::make_unique<ComputePass>(computeCallCount));
        });
    if (!extendedResult.has_value())
    {
        return 19;
    }
    auto extended = extendedResult.take_value();
    if (extended->plan().passes().size() != 9 || extended->plan().passes()[0].name != "ClearFinalColor" ||
        extended->plan().passes()[1].name != "AfterClear" ||
        extended->plan().passes()[2].name != "ProduceColor" ||
        extended->plan().passes()[3].name != "ConsumeColor" ||
        extended->plan().passes()[4].name != "PoolRead" ||
        extended->plan().passes()[5].name != "PoolTextureWrite" ||
        extended->plan().passes()[6].name != "PoolTextureRead" ||
        extended->plan().passes()[7].name != "Compute" ||
        extended->plan().passes()[8].name != "PresentToSwapChain" ||
        extended->plan().passes()[2].barriersAfter.size() != 1 ||
        extended->plan().passes()[2].barriersAfter[0].after != cue::FrameGraphResourceState::Common ||
        extended->plan().passes()[3].barriersBefore[0].before != cue::FrameGraphResourceState::Common ||
        extended->plan().passes()[3].barriersAfter.size() != 2 ||
        extended->plan().passes()[3].barriersAfter[0].after != cue::FrameGraphResourceState::Common ||
        extended->plan().passes()[8].barriersBefore[0].before != cue::FrameGraphResourceState::Common)
    {
        return 20;
    }
    customEnabled = false;
    if (extended->execute(0, *commandPool, *queuePool).has_value() ||
        customCallCount != 0 || poolCallCount != 0 || computeCallCount != 0)
    {
        return 21;
    }
    customEnabled = true;
    auto executeResult = extended->execute(0, *commandPool, *queuePool);
    if (!executeResult.has_value() || !*executeResult.try_value() ||
        customCallCount != 1 || poolCallCount != 1 || computeCallCount != 1)
    {
        return 22;
    }
    auto* extendedBackBuffer = swapChain->back_buffer(swapChain->current_index());
    auto verifyCommandResult = commandPool->acquire(cue::QueueType::Graphics);
    if (!extendedBackBuffer || !verifyCommandResult.has_value())
    {
        return 24;
    }
    auto verifyCommand = verifyCommandResult.take_value();
    auto* verifyContext = dynamic_cast<cue::dx12::DX12GpuCommandContext*>(verifyCommand.get());
    if (!verifyContext || !verifyContext->command_list())
    {
        return 25;
    }
    auto* verifyList = verifyContext->command_list();
    transition(*verifyList, *extendedBackBuffer, D3D12_RESOURCE_STATE_PRESENT,
               D3D12_RESOURCE_STATE_COPY_SOURCE);
    source.pResource = extendedBackBuffer;
    verifyList->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    transition(*verifyList, *extendedBackBuffer, D3D12_RESOURCE_STATE_COPY_SOURCE,
               D3D12_RESOURCE_STATE_PRESENT);
    if (!verifyCommand->close().has_value())
    {
        return 26;
    }
    auto verifySubmitResult = commandPool->submit(*swapChain->graphics_queue(), *verifyCommand);
    if (!verifySubmitResult.has_value())
    {
        return 27;
    }
    auto verifyCompletion = verifySubmitResult.take_value();
    if (!verifyCompletion->wait().has_value() || !readback->read(0, pixels).has_value())
    {
        return 28;
    }
    const auto outputMatches = [&pixels, &layout](std::size_t a_x, std::size_t a_y)
    {
        const auto offset = a_y * layout.Footprint.RowPitch + a_x * 4;
        const auto red = std::to_integer<int>(pixels[offset]);
        const auto green = std::to_integer<int>(pixels[offset + 1]);
        const auto blue = std::to_integer<int>(pixels[offset + 2]);
        return red >= 203 && red <= 205 && green >= 25 && green <= 27 &&
               blue >= 76 && blue <= 78 && std::to_integer<int>(pixels[offset + 3]) == 255;
    };
    if (!outputMatches(0, 0) || !outputMatches(63, 63))
    {
        return 29;
    }
    verifyCommand.reset();
    if (!extended->shutdown().has_value() || !commandPool->shutdown().has_value() ||
        !queuePool->shutdown().has_value() ||
        !pool->retire(poolBuffer).has_value() || !pool->shutdown().has_value() ||
        !swapChain->shutdown().has_value() || !window->destroy().has_value())
    {
        return 23;
    }
    return 0;
}
} // namespace

/// @brief 固定 Main Graph だけで Clear 色を表示できることを確認する
int main()
{
    return run_tests();
}
