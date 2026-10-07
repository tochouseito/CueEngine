#include <DX12/DX12MainFrameGraph.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>

#include <d3d12sdklayers.h>

#include <DX12/DX12CommandPool.h>
#include <DX12/DX12DescriptorAllocator.h>
#include <DX12/DX12GpuResource.h>
#include <DX12/DX12GpuResourcePool.h>
#include <DX12/DX12PipelineManager.h>
#include <DX12/DX12QueuePool.h>
#include <DX12/DX12RenderDevice.h>
#include <DX12/DX12SwapChain.h>
#include <DX12/DX12ViewManager.h>
#include <Passes/PresentToSwapChainPass.h>
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
        if (!result.has_value())
        {
            return cue::Result<void>::failure(*result.try_error());
        }
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
        if (!result.has_value())
        {
            return cue::Result<void>::failure(*result.try_error());
        }
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

/// @brief Pass 設定からの Compute PSO 生成と抽象 Context の Dispatch を確認する
class ComputePass final : public cue::FrameGraphPass
{
public:
    /// @brief 実行回数を呼出側で検証する
    explicit ComputePass(int& a_count) noexcept : m_count(&a_count) {}

    /// @brief 診断に使う名前を返す
    [[nodiscard]] const char* name() const noexcept override { return "Compute"; }
    /// @brief Compute List へ記録する
    [[nodiscard]] cue::QueueType type() const noexcept override { return cue::QueueType::Compute; }
    /// @brief Root、CS と PSO の生成を Builder に依頼する
    [[nodiscard]] cue::Result<void> setup(cue::FrameGraphBuilder &a_builder) override
    {
        auto root = a_builder.create_root_signature({});
        if (!root.has_value())
        {
            return cue::Result<void>::failure(*root.try_error());
        }
        auto shader = a_builder.create_shader_blob(
            {"ComputeTest.CS", CUE_TEST_SHADER_PATH, "cs_empty_main", cue::ShaderStage::Compute});
        if (!shader.has_value())
        {
            return cue::Result<void>::failure(*shader.try_error());
        }
        auto pipeline = a_builder.create_compute_pipeline({"ComputeTest.PSO", root.take_value(), shader.take_value()});
        if (!pipeline.has_value())
        {
            return cue::Result<void>::failure(*pipeline.try_error());
        }
        m_pipeline = pipeline.take_value();
        return cue::Result<void>::success();
    }
    /// @brief Resource を使わない Shader の実行だけを宣言する
    [[nodiscard]] cue::Result<void> describe_resources(cue::FrameGraphBuilder&) override
    {
        return cue::Result<void>::success();
    }
    /// @brief 無効な Group 数を拒否してから一回の Dispatch を記録する
    [[nodiscard]] cue::Result<void> execute(cue::FrameGraphContext& a_context) override
    {
        if (a_context.command_context().type() != cue::QueueType::Compute)
        {
            return cue::Result<void>::failure({cue::ErrorCategory::InvalidState, "ComputePass.execute"});
        }
        auto binding = a_context.set_compute_pipeline(m_pipeline);
        if (!binding.has_value())
        {
            return binding;
        }
        if (a_context.dispatch(0, 1, 1).has_value())
        {
            return cue::Result<void>::failure({cue::ErrorCategory::InvalidState, "ComputePass.invalid_dispatch"});
        }
        auto dispatch = a_context.dispatch(1, 1, 1);
        if (dispatch.has_value())
        {
            ++*m_count;
        }
        return dispatch;
    }

private:
    int* m_count = nullptr;
    cue::PipelineStateHandle m_pipeline;
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
    auto viewsResult = cue::dx12::DX12ViewManager::create(*device, *rtvAllocator, *srvAllocator);
    if (!viewsResult.has_value())
    {
        return 31;
    }
    auto views = viewsResult.take_value();
    auto pipelinesResult = cue::dx12::DX12PipelineManager::create(*device);
    if (!pipelinesResult.has_value())
    {
        return 90;
    }
    auto pipelines = pipelinesResult.take_value();
    const cue::dx12::DX12ResourceContext resources{*device, *views, *pipelines};
    cue::queueLease queue(queueResult.take_value().release(), [](cue::IQueueContext *a_queue) { delete a_queue; });
    const cue::dx12::DX12SwapChainConfig config{64, 64, 2, DXGI_FORMAT_R8G8B8A8_UNORM, false, false};
    auto swapResult = cue::dx12::DX12SwapChain::create(resources, std::move(queue), *handleResult.try_value(), config);
    if (!swapResult.has_value())
    {
        return 6;
    }
    auto swapChain = swapResult.take_value();
    const std::array<float, 4> clearColor{0.2f, 0.4f, 0.6f, 1.0f};
    auto graphResult = cue::dx12::DX12MainFrameGraph::create(resources, *swapChain, {2, clearColor, {}});
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
        resources, *swapChain,
        {2, clearColor,
         [&customCallCount, &customEnabled, &poolCallCount, &computeCallCount, &pool, poolBuffer,
          poolTexture](cue::FrameGraph &a_graph, cue::FrameGraphResourceHandle a_finalColor) -> cue::Result<void>
         {
             auto testResult =
                 a_graph.add_pass(std::make_unique<TestPass>(a_finalColor, customCallCount, customEnabled));
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
             auto poolTextureWriteResult = a_graph.add_pass(std::make_unique<PoolTexturePass>(*pool, poolTexture));
             if (!poolTextureWriteResult.has_value())
             {
                 return poolTextureWriteResult;
             }
             auto poolTextureReadResult = a_graph.add_pass(std::make_unique<PoolTextureReadPass>());
             if (!poolTextureReadResult.has_value())
             {
                 return poolTextureReadResult;
             }
             return a_graph.add_pass(std::make_unique<ComputePass>(computeCallCount));
         }});
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
    if (extended->execute(0, {*commandPool, *queuePool}).has_value() || customCallCount != 0 || poolCallCount != 0 ||
        computeCallCount != 0)
    {
        return 21;
    }
    customEnabled = true;
    auto executeResult = extended->execute(0, {*commandPool, *queuePool});
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

/// @brief FinalColor の全画面表示を新しい寸法の四隅から読み戻し、Viewport の更新も検証する
bool check_resized_pixels(cue::dx12::DX12RenderDevice &a_device, cue::dx12::DX12MainFrameGraph &a_graph,
                          cue::dx12::DX12SwapChain &a_swapChain, cue::dx12::DX12CommandPool &a_commandPool,
                          std::uint32_t a_width, std::uint32_t a_height, std::uint32_t a_frameIndex)
{
    auto *backBuffer = a_swapChain.back_buffer(a_swapChain.current_index());
    if (!backBuffer || !a_swapChain.graphics_queue())
    {
        return false;
    }
    const auto desc = backBuffer->GetDesc();
    if (desc.Width != a_width || desc.Height != a_height)
    {
        return false;
    }
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
    UINT64 totalBytes = 0;
    a_device.device()->GetCopyableFootprints(&desc, 0, 1, 0, &layout, nullptr, nullptr, &totalBytes);
    auto readbackResult =
        cue::dx12::DX12GpuResource::create_buffer(*a_device.device(), {totalBytes, cue::GpuMemoryUsage::Readback});
    auto commandResult = a_commandPool.acquire(cue::QueueType::Graphics);
    if (!readbackResult.has_value() || !commandResult.has_value())
    {
        return false;
    }
    auto readback = readbackResult.take_value();
    auto command = commandResult.take_value();
    auto *context = dynamic_cast<cue::dx12::DX12GpuCommandContext *>(command.get());
    if (!context || !context->command_list() || !a_graph.record(a_frameIndex, *context).has_value())
    {
        return false;
    }
    auto *list = context->command_list();
    // Graph が Present へ戻した BackBuffer を CopySource として借用し、読み戻し後に同じ State へ戻す
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
        a_graph.discard_unsubmitted(a_frameIndex);
        return false;
    }
    auto submitted = a_commandPool.submit(*a_swapChain.graphics_queue(), *command);
    if (!submitted.has_value())
    {
        a_graph.discard_unsubmitted(a_frameIndex);
        return false;
    }
    std::shared_ptr<cue::ICommandCompletion> completion(submitted.take_value());
    if (!a_graph.mark_submitted(a_frameIndex, completion).has_value() || !completion->wait().has_value())
    {
        return false;
    }
    std::vector<std::byte> pixels(static_cast<std::size_t>(totalBytes));
    if (!readback->read(0, pixels).has_value())
    {
        return false;
    }
    const auto matches = [&pixels, &layout](std::uint32_t a_x, std::uint32_t a_y)
    {
        const auto offset = static_cast<std::size_t>(a_y) * layout.Footprint.RowPitch + a_x * 4;
        return std::to_integer<int>(pixels[offset]) == 51 && std::to_integer<int>(pixels[offset + 1]) == 102 &&
               std::to_integer<int>(pixels[offset + 2]) == 153 && std::to_integer<int>(pixels[offset + 3]) == 255;
    };
    // 四隅を検証することで、旧寸法の Viewport／Scissor が残る不具合も検出する
    const bool valid =
        matches(0, 0) && matches(a_width - 1, 0) && matches(0, a_height - 1) && matches(a_width - 1, a_height - 1);
    command.reset();
    return valid && a_swapChain.present().has_value();
}

/// @brief 全 Graph 所有者の停止後に Descriptor Slot が残らないことを検証する
bool check_free_descriptors(cue::dx12::DX12DescriptorAllocator &a_allocator, std::uint32_t a_capacity)
{
    std::vector<cue::dx12::DX12DescriptorHandle> handles;
    bool valid = true;
    for (std::uint32_t index = 0; index < a_capacity; ++index)
    {
        auto result = a_allocator.allocate();
        if (!result.has_value())
        {
            valid = false;
            break;
        }
        handles.push_back(result.take_value());
    }
    valid = valid && !a_allocator.allocate().has_value();
    for (const auto handle : handles)
    {
        valid = a_allocator.release(handle).has_value() && valid;
    }
    return valid;
}

/// @brief Resize 中の重大診断と、全 Owner 破棄後に残る GPU Object を検出する
bool check_resize_messages(ID3D12InfoQueue &a_queue, bool a_checkLeaks)
{
    bool valid = true;
    for (UINT64 index = 0; index < a_queue.GetNumStoredMessagesAllowedByRetrievalFilter(); ++index)
    {
        SIZE_T size = 0;
        if (FAILED(a_queue.GetMessage(index, nullptr, &size)))
        {
            return false;
        }
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

/// @brief WARP 上で連続 Resize、再構築 Callback、GPU 待機と表示画素を一つの寿命で検証する
int run_resize_tests()
{
    std::fprintf(stderr, "MainFrameGraph resize: create owners\n");
    auto systemResult = cue::create_windows_window_system();
    auto deviceResult = cue::dx12::DX12RenderDevice::create(cue::dx12::AdapterSelection::Warp);
    if (!systemResult.has_value() || !deviceResult.has_value())
    {
        return 101;
    }
    auto system = systemResult.take_value();
    auto device = deviceResult.take_value();
    auto windowResult = system->create_window({"CueEngine Graph Resize Test", {64, 64}});
    if (!windowResult.has_value())
    {
        return 102;
    }
    auto window = windowResult.take_value();
    auto handleResult = cue::borrow_windows_window_handle(*window);
    if (!handleResult.has_value() || !window->show().has_value())
    {
        return 103;
    }
#if defined(_DEBUG)
    Microsoft::WRL::ComPtr<ID3D12Device> probe = device->device();
    Microsoft::WRL::ComPtr<ID3D12InfoQueue> infoQueue;
    if (FAILED(probe.As(&infoQueue)))
    {
        return 104;
    }
    infoQueue->ClearStoredMessages();
#endif
    // 小さい Heap のまま繰り返し再構築し、旧 RTV／SRV が返却されない場合は容量不足で失敗させる
    constexpr std::uint32_t k_capacity = 8;
    auto rtvResult =
        cue::dx12::DX12DescriptorAllocator::create(*device->device(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, k_capacity);
    auto srvResult = cue::dx12::DX12DescriptorAllocator::create(
        *device->device(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, k_capacity, true);
    auto pipelineResult = cue::dx12::DX12PipelineManager::create(*device);
    auto commandPoolResult = cue::dx12::DX12CommandPool::create(*device);
    auto queuePoolResult = cue::dx12::DX12QueuePool::create(*device);
    if (!rtvResult.has_value() || !srvResult.has_value() || !pipelineResult.has_value() ||
        !commandPoolResult.has_value() || !queuePoolResult.has_value())
    {
        return 105;
    }
    auto rtv = rtvResult.take_value();
    auto srv = srvResult.take_value();
    auto pipelines = pipelineResult.take_value();
    auto commandPool = commandPoolResult.take_value();
    auto queuePool = queuePoolResult.take_value();
    auto viewsResult = cue::dx12::DX12ViewManager::create(*device, *rtv, *srv);
    if (!viewsResult.has_value())
    {
        return 106;
    }
    auto views = viewsResult.take_value();
    const cue::dx12::DX12ResourceContext resources{*device, *views, *pipelines};
    auto queueResult = cue::dx12::DX12GpuCommandQueue::create(*device->device(), cue::QueueType::Graphics, 0);
    if (!queueResult.has_value())
    {
        return 107;
    }
    cue::queueLease queue(queueResult.take_value().release(), [](cue::IQueueContext *a_queue) { delete a_queue; });
    auto swapResult = cue::dx12::DX12SwapChain::create(resources, std::move(queue), *handleResult.try_value(),
                                                       {64, 64, 2, DXGI_FORMAT_R8G8B8A8_UNORM, false, true});
    if (!swapResult.has_value())
    {
        return 108;
    }
    auto swapChain = swapResult.take_value();
    int configureCount = 0;
    int factoryCount = 0;
    int passCount = 0;
    bool enabled = true;
    cue::FrameGraphResourceHandle finalColor;
    cue::dx12::DX12MainFrameGraphConfig config;
    config.clearColor = {0.2f, 0.4f, 0.6f, 1.0f};
    config.configure = [&](cue::FrameGraph &a_graph, cue::FrameGraphResourceHandle a_color)
    {
        ++configureCount;
        finalColor = a_color;
        return a_graph.add_pass(std::make_unique<TestPass>(a_color, passCount, enabled));
    };
    config.displayPassFactory = [&factoryCount]() -> std::unique_ptr<cue::FrameGraphPass>
    {
        ++factoryCount;
        return std::make_unique<cue::PresentToSwapChainPass>();
    };
    auto graphResult = cue::dx12::DX12MainFrameGraph::create(resources, *swapChain, std::move(config));
    if (!graphResult.has_value())
    {
        return 109;
    }
    auto graph = graphResult.take_value();
    const auto initialHandle = finalColor;
    auto *initialBuffer = swapChain->back_buffer(0);
    auto *graphicsQueue = swapChain->graphics_queue();
    const bool isTearingEnabled = swapChain->is_tearing_enabled();
    if (configureCount != 1 || factoryCount != 1 || graph->resize(resources, 0, 64).has_value() ||
        graph->resize(resources, 64, 0).has_value() || !graph->resize(resources, 64, 64).has_value() ||
        configureCount != 1 || factoryCount != 1 || finalColor.graphId != initialHandle.graphId ||
        swapChain->back_buffer(0) != initialBuffer)
    {
        return 110;
    }
    const std::array<std::array<std::uint32_t, 2>, 7> sizes{
        {{96, 48}, {33, 79}, {128, 96}, {32, 32}, {75, 51}, {64, 64}, {160, 90}}};
    auto previousHandle = initialHandle;
    std::uint32_t frameIndex = 0;
    for (const auto size : sizes)
    {
        std::fprintf(stderr, "MainFrameGraph resize: %u x %u, frame %u\n", size[0], size[1], frameIndex);
        // CPU 側で Fence を待たず次の Resize を要求し、Graph が提出済み枠を待ってから解放する経路を通す
        auto submitted = graph->execute(frameIndex, {*commandPool, *queuePool});
        if (!submitted.has_value() || !*submitted.try_value())
        {
            if (auto *error = submitted.try_error())
            {
                std::fprintf(stderr, "MainFrameGraph execute: %s, native %lld\n", error->operation.c_str(),
                             static_cast<long long>(error->nativeCode));
            }
            return 111;
        }
        auto resized = graph->resize(resources, size[0], size[1]);
        if (!resized.has_value())
        {
            auto *error = resized.try_error();
            std::fprintf(stderr, "MainFrameGraph resize: %s, native %lld\n", error->operation.c_str(),
                         static_cast<long long>(error->nativeCode));
            return 111;
        }
        if (finalColor.graphId == previousHandle.graphId || configureCount != factoryCount ||
            swapChain->graphics_queue() != graphicsQueue || swapChain->is_tearing_enabled() != isTearingEnabled)
        {
            return 111;
        }
        const auto &color = graph->plan().resources()[finalColor.index];
        if (color.handle.graphId != finalColor.graphId || color.textureDesc.width != size[0] ||
            color.textureDesc.height != size[1] ||
            !check_resized_pixels(*device, *graph, *swapChain, *commandPool, size[0], size[1], frameIndex))
        {
            return 112;
        }
        previousHandle = finalColor;
        frameIndex = (frameIndex + 1) % 2;
    }
    if (configureCount != 8 || factoryCount != 8 || passCount != 14 || !graph->shutdown().has_value())
    {
        return 113;
    }
    graph.reset();
    std::fprintf(stderr, "MainFrameGraph resize: stale handle and one-shot rejection\n");
    // 旧 Graph の Handle を新しい Pass に渡しても、世代の違いを Build で検出して受け付けない
    cue::dx12::DX12MainFrameGraphConfig staleConfig;
    staleConfig.configure = [&](cue::FrameGraph &a_graph, cue::FrameGraphResourceHandle)
    { return a_graph.add_pass(std::make_unique<TestPass>(initialHandle, passCount, enabled)); };
    if (cue::dx12::DX12MainFrameGraph::create(resources, *swapChain, std::move(staleConfig)).has_value())
    {
        return 122;
    }
    // 一度だけ渡す独自 Pass は作り直せないため、サイズ変更前に失敗して旧 Graph を維持する
    cue::dx12::DX12MainFrameGraphConfig oneShotConfig;
    oneShotConfig.clearColor = {0.2f, 0.4f, 0.6f, 1.0f};
    oneShotConfig.displayPass = std::make_unique<cue::PresentToSwapChainPass>();
    auto oneShotResult = cue::dx12::DX12MainFrameGraph::create(resources, *swapChain, std::move(oneShotConfig));
    if (!oneShotResult.has_value())
    {
        return 114;
    }
    auto oneShot = oneShotResult.take_value();
    const auto oneShotGraphId = oneShot->plan().resources()[0].handle.graphId;
    if (oneShot->resize(resources, 80, 60).has_value() ||
        oneShot->plan().resources()[0].handle.graphId != oneShotGraphId ||
        !check_resized_pixels(*device, *oneShot, *swapChain, *commandPool, 160, 90, 0) ||
        !oneShot->shutdown().has_value() || oneShot->resize(resources, 80, 60).has_value())
    {
        return 115;
    }
    oneShot.reset();
    std::fprintf(stderr, "MainFrameGraph resize: default pass rebuild\n");
    // Factory のない標準表示は組み込み Pass を再生成し、既定経路でも Resize 後の描画を継続する
    cue::dx12::DX12MainFrameGraphConfig defaultConfig;
    defaultConfig.clearColor = {0.2f, 0.4f, 0.6f, 1.0f};
    auto defaultResult = cue::dx12::DX12MainFrameGraph::create(resources, *swapChain, std::move(defaultConfig));
    if (!defaultResult.has_value())
    {
        return 120;
    }
    auto defaultGraph = defaultResult.take_value();
    if (!defaultGraph->resize(resources, 80, 60).has_value() ||
        !check_resized_pixels(*device, *defaultGraph, *swapChain, *commandPool, 80, 60, 1) ||
        !defaultGraph->shutdown().has_value())
    {
        return 121;
    }
    defaultGraph.reset();
    std::fprintf(stderr, "MainFrameGraph resize: factory failure and recovery\n");
    // Graph 再構築の失敗後は実体のない Graph を実行させず、同じ寸法への再試行で復帰させる
    bool failFactory = false;
    bool throwFactory = false;
    int recoveryFactoryCount = 0;
    cue::dx12::DX12MainFrameGraphConfig recoveryConfig;
    recoveryConfig.clearColor = {0.2f, 0.4f, 0.6f, 1.0f};
    recoveryConfig.displayPassFactory = [&]() -> std::unique_ptr<cue::FrameGraphPass>
    {
        ++recoveryFactoryCount;
        if (throwFactory)
        {
            throw std::runtime_error("Resize display factory failure");
        }
        return failFactory ? nullptr : std::make_unique<cue::PresentToSwapChainPass>();
    };
    auto recoveryResult = cue::dx12::DX12MainFrameGraph::create(resources, *swapChain, std::move(recoveryConfig));
    if (!recoveryResult.has_value())
    {
        return 123;
    }
    auto recovery = recoveryResult.take_value();
    const std::array<std::array<std::uint32_t, 2>, 2> recoverySizes{{{96, 72}, {110, 70}}};
    for (std::uint32_t index = 0; index < recoverySizes.size(); ++index)
    {
        failFactory = index == 0;
        throwFactory = index == 1;
        const auto size = recoverySizes[index];
        auto failedResize = recovery->resize(resources, size[0], size[1]);
        auto stoppedExecution = recovery->execute(index, {*commandPool, *queuePool});
        const auto expectedCategory =
            throwFactory ? cue::ErrorCategory::PlatformFailure : cue::ErrorCategory::InvalidState;
        if (failedResize.has_value() || failedResize.try_error()->category != expectedCategory ||
            stoppedExecution.has_value() ||
            stoppedExecution.try_error()->category != cue::ErrorCategory::InvalidState || !swapChain->back_buffer(0) ||
            swapChain->back_buffer(0)->GetDesc().Width != size[0] ||
            swapChain->back_buffer(0)->GetDesc().Height != size[1])
        {
            return 124;
        }
        // SwapChain は既に新しい寸法でも、Graph の実体がない状態では再構築を省略しない
        failFactory = false;
        throwFactory = false;
        if (!recovery->resize(resources, size[0], size[1]).has_value() ||
            !check_resized_pixels(*device, *recovery, *swapChain, *commandPool, size[0], size[1], index))
        {
            return 125;
        }
    }
    if (recoveryFactoryCount != 5 || !recovery->shutdown().has_value())
    {
        return 126;
    }
    recovery.reset();
    std::fprintf(stderr, "MainFrameGraph resize: owner shutdown and descriptor check\n");
    if (!commandPool->shutdown().has_value() || !queuePool->shutdown().has_value() ||
        !swapChain->shutdown().has_value() || !check_free_descriptors(*rtv, k_capacity) ||
        !check_free_descriptors(*srv, k_capacity))
    {
        return 116;
    }
#if defined(_DEBUG)
    // Leak Probe が Device を保持する正常停止では Live Device Warning が出るため、この診断時だけ停止を解除する
    // Error／Corruption は停止対象のままとし、残存 Object は InfoQueue の内容で検証する
    if (FAILED(infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, false)))
    {
        return 127;
    }
#endif
    // D3D12Device 自身と診断 Interface 以外の Owner を破棄してから Leak 診断を行う
    commandPool.reset();
    queuePool.reset();
    swapChain.reset();
    pipelines.reset();
    views.reset();
    rtv.reset();
    srv.reset();
    device.reset();
#if defined(_DEBUG)
    std::fprintf(stderr, "MainFrameGraph resize: inspect debug messages\n");
    if (!check_resize_messages(*infoQueue.Get(), false))
    {
        return 117;
    }
    infoQueue->ClearStoredMessages();
    std::fprintf(stderr, "MainFrameGraph resize: report live device objects\n");
    Microsoft::WRL::ComPtr<ID3D12DebugDevice> debug;
    if (FAILED(probe.As(&debug)) ||
        FAILED(debug->ReportLiveDeviceObjects(
            static_cast<D3D12_RLDO_FLAGS>(D3D12_RLDO_DETAIL | D3D12_RLDO_IGNORE_INTERNAL))) ||
        !check_resize_messages(*infoQueue.Get(), true))
    {
        return 118;
    }
#endif
    return window->destroy().has_value() && system->pump_events().has_value() ? 0 : 119;
}
} // namespace

/// @brief 固定 Main Graph だけで Clear 色を表示できることを確認する
int main()
{
    std::fprintf(stderr, "MainFrameGraph: original graph tests\n");
    const auto result = run_tests();
    if (result != 0)
    {
        std::fprintf(stderr, "MainFrameGraph: original graph tests failed, code %d\n", result);
        return result;
    }
    const auto resizeResult = run_resize_tests();
    std::fprintf(stderr, "MainFrameGraph: resize tests completed, code %d\n", resizeResult);
    return resizeResult;
}
