#include <DX12/DX12FrameGraphExecutor.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <vector>

#include <wrl/client.h>

#include <DX12/DX12CommandPool.h>
#include <DX12/DX12FrameGraphResources.h>
#include <DX12/DX12GpuResource.h>
#include <DX12/DX12QueuePool.h>
#include <DX12/DX12RenderDevice.h>

namespace
{
/// @brief Graphics 専用開始 State の Resource を Compute へ渡し、終了 State まで戻す
bool check_compute_read(cue::dx12::DX12RenderDevice &a_device, cue::ICommandPool &a_commands, cue::IQueuePool &a_queues,
                        bool a_useTexture)
{
    auto builderResult = cue::FrameGraphBuilder::create();
    if (!builderResult.has_value())
        return false;
    auto builder = builderResult.take_value();
    // Buffer は現行 DX12 の Common 開始、Texture は Graphics 専用 Shader State の持越しを検証する
    const auto initialState =
        a_useTexture ? cue::FrameGraphResourceState::ShaderRead : cue::FrameGraphResourceState::Common;
    auto imported = a_useTexture ? builder->import_texture2d({4, 4}, initialState, initialState)
                                 : builder->import_buffer({64}, initialState, initialState);
    auto added = builder->add_pass("ComputeShaderRead", cue::QueueType::Compute);
    if (!imported.has_value() || !added.has_value())
        return false;
    const auto handle = imported.take_value();
    if (!builder->use(added.take_value(), handle, cue::FrameGraphAccess::Read, cue::FrameGraphResourceState::ShaderRead)
             .has_value())
        return false;
    auto planResult = builder->build();
    if (!planResult.has_value())
        return false;
    const auto plan = planResult.take_value();
    auto resourcesResult = cue::dx12::DX12FrameGraphResources::create(a_device, plan);
    if (!resourcesResult.has_value())
        return false;
    auto resources = resourcesResult.take_value();
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = a_useTexture ? D3D12_RESOURCE_DIMENSION_TEXTURE2D : D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = a_useTexture ? 4 : 64;
    desc.Height = a_useTexture ? 4 : 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = a_useTexture ? D3D12_TEXTURE_LAYOUT_UNKNOWN : D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Format = a_useTexture ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_UNKNOWN;
    const auto nativeInitialState =
        a_useTexture ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
                     : D3D12_RESOURCE_STATE_COMMON;
    Microsoft::WRL::ComPtr<ID3D12Resource> native;
    if (FAILED(a_device.device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, nativeInitialState,
                                                          nullptr, IID_PPV_ARGS(&native))) ||
        FAILED(native->SetName(L"Compute ShaderRead Test")))
        return false;
    const std::array<cue::dx12::DX12FrameGraphExternalResource, 1> bindings{{{handle, native.Get()}}};
    int calls = 0;
    const std::array<cue::dx12::dx12FrameGraphPassCallback, 1> callbacks{
        {[&calls, handle](ID3D12GraphicsCommandList &, const cue::dx12::DX12FrameGraphPassContext &a_context)
         {
             ++calls;
             return a_context.resource(handle)
                        ? cue::Result<void>::success()
                        : cue::Result<void>::failure({cue::ErrorCategory::InvalidState, "ComputeReadTest.binding"});
         }}};
    auto graphicsResult = a_queues.acquire(cue::QueueType::Graphics);
    auto computeResult = a_queues.acquire(cue::QueueType::Compute);
    if (!graphicsResult.has_value() || !computeResult.has_value())
        return false;
    auto graphics = graphicsResult.take_value();
    auto compute = computeResult.take_value();
    cue::dx12::DX12FrameGraphPrepared prepared;
    std::shared_ptr<cue::ICommandCompletion> prior;
    for (int stage = 0; stage < 3; ++stage)
    {
        auto *queue = stage == 1 ? compute.get() : graphics.get();
        if (prior && !queue->wait_for_queue(stage == 1 ? *graphics : *compute, prior->fence_value()).has_value())
            return false;
        auto commandResult = a_commands.acquire(queue->type());
        if (!commandResult.has_value())
            return false;
        auto command = commandResult.take_value();
        auto *context = dynamic_cast<cue::dx12::DX12GpuCommandContext *>(command.get());
        if (!context)
            return false;
        if (stage == 0 &&
            !cue::dx12::DX12FrameGraphExecutor::prepare(plan, *resources, bindings, *context, prepared).has_value())
            return false;
        auto recorded = cue::dx12::DX12FrameGraphExecutor::record_prepared(
            plan, *resources, prepared,
            stage == 1 ? std::span<const cue::dx12::dx12FrameGraphPassCallback>(callbacks)
                       : std::span<const cue::dx12::dx12FrameGraphPassCallback>{},
            *context, stage == 2 ? 1 : 0, stage == 1 ? 1 : 0, stage == 2, stage == 0);
        if (!recorded.has_value())
        {
            std::fprintf(stderr, "Compute stage %d record: %s (%lld)\n", stage, recorded.try_error()->operation.c_str(),
                         static_cast<long long>(recorded.try_error()->nativeCode));
            return false;
        }
        auto closed = command->close();
        if (!closed.has_value())
        {
            std::fprintf(stderr, "Compute stage %d close: %s (%lld)\n", stage, closed.try_error()->operation.c_str(),
                         static_cast<long long>(closed.try_error()->nativeCode));
            return false;
        }
        auto submitted = a_commands.submit(*queue, *command);
        if (!submitted.has_value())
            return false;
        prior = std::shared_ptr<cue::ICommandCompletion>(submitted.take_value());
    }
    return calls == 1 && resources->mark_submitted(prior).has_value() && resources->shutdown().has_value();
}
} // namespace

/// @brief WARP で Pass 順、Alias、外部 Binding、UAV Barrier 記録と GPU 転送を検証する
int main()
{
    auto builderResult = cue::FrameGraphBuilder::create();
    if (!builderResult.has_value())
    {
        return 1;
    }
    auto builder = builderResult.take_value();
    auto uploadResult =
        builder->import_buffer({32, cue::GpuMemoryUsage::Upload}, cue::FrameGraphResourceState::GenericRead,
                               cue::FrameGraphResourceState::GenericRead);
    auto outputResult =
        builder->import_buffer({32}, cue::FrameGraphResourceState::Common, cue::FrameGraphResourceState::Common);
    auto readbackResult =
        builder->import_buffer({32, cue::GpuMemoryUsage::Readback}, cue::FrameGraphResourceState::CopyDestination,
                               cue::FrameGraphResourceState::CopyDestination);
    auto uavResult =
        builder->import_buffer({32}, cue::FrameGraphResourceState::Common, cue::FrameGraphResourceState::Common);
    auto firstResult = builder->create_transient_buffer({16});
    auto secondResult = builder->create_transient_buffer({16});
    if (!uploadResult.has_value() || !outputResult.has_value() || !readbackResult.has_value() ||
        !uavResult.has_value() || !firstResult.has_value() || !secondResult.has_value())
    {
        return 2;
    }
    const auto upload = uploadResult.take_value();
    const auto output = outputResult.take_value();
    const auto readback = readbackResult.take_value();
    const auto uav = uavResult.take_value();
    const auto first = firstResult.take_value();
    const auto second = secondResult.take_value();
    std::array<cue::FrameGraphPassHandle, 7> passes{};
    constexpr std::array<const char *, 7> k_names = {"UploadFirst", "StoreFirst", "UploadSecond", "StoreSecond",
                                                     "Readback",    "UavFirst",   "UavSecond"};
    for (std::size_t index = 0; index < passes.size(); ++index)
    {
        auto passResult = builder->add_pass(k_names[index]);
        if (!passResult.has_value())
        {
            return 3;
        }
        passes[index] = passResult.take_value();
    }
    if (!builder->use(passes[0], upload, cue::FrameGraphAccess::Read, cue::FrameGraphResourceState::GenericRead)
             .has_value() ||
        !builder->use(passes[0], first, cue::FrameGraphAccess::Write, cue::FrameGraphResourceState::CopyDestination)
             .has_value() ||
        !builder->use(passes[1], first, cue::FrameGraphAccess::Read, cue::FrameGraphResourceState::CopySource)
             .has_value() ||
        !builder->use(passes[1], output, cue::FrameGraphAccess::Write, cue::FrameGraphResourceState::CopyDestination)
             .has_value() ||
        !builder->use(passes[2], upload, cue::FrameGraphAccess::Read, cue::FrameGraphResourceState::GenericRead)
             .has_value() ||
        !builder->use(passes[2], second, cue::FrameGraphAccess::Write, cue::FrameGraphResourceState::CopyDestination)
             .has_value() ||
        !builder->use(passes[3], second, cue::FrameGraphAccess::Read, cue::FrameGraphResourceState::CopySource)
             .has_value() ||
        !builder->use(passes[3], output, cue::FrameGraphAccess::Write, cue::FrameGraphResourceState::CopyDestination)
             .has_value() ||
        !builder->use(passes[4], output, cue::FrameGraphAccess::Read, cue::FrameGraphResourceState::CopySource)
             .has_value() ||
        !builder->use(passes[4], readback, cue::FrameGraphAccess::Write, cue::FrameGraphResourceState::CopyDestination)
             .has_value() ||
        !builder->use(passes[5], uav, cue::FrameGraphAccess::Write, cue::FrameGraphResourceState::UnorderedAccess)
             .has_value() ||
        !builder->use(passes[6], uav, cue::FrameGraphAccess::Write, cue::FrameGraphResourceState::UnorderedAccess)
             .has_value() ||
        !builder->depends_on(passes[2], passes[1]).has_value() ||
        !builder->depends_on(passes[5], passes[4]).has_value())
    {
        return 4;
    }
    auto planResult = builder->build();
    if (!planResult.has_value())
    {
        return 5;
    }
    auto plan = planResult.take_value();
    if (plan.passes().size() != passes.size() || plan.alias_slots().size() != 1 ||
        plan.alias_slots()[0].resources.size() != 2 || plan.final_barriers().size() != 2 ||
        plan.passes()[6].barriersBefore.size() != 1 ||
        plan.passes()[6].barriersBefore[0].kind != cue::FrameGraphBarrierKind::UnorderedAccess)
    {
        return 6;
    }

    auto deviceResult = cue::dx12::DX12RenderDevice::create(cue::dx12::AdapterSelection::Warp);
    if (!deviceResult.has_value())
    {
        return 7;
    }
    auto device = deviceResult.take_value();
    Microsoft::WRL::ComPtr<ID3D12InfoQueue> infoQueue;
    if (SUCCEEDED(device->device()->QueryInterface(IID_PPV_ARGS(&infoQueue))))
    {
        infoQueue->ClearStoredMessages();
    }
    auto resourcesResult = cue::dx12::DX12FrameGraphResources::create(*device, plan);
    auto nativeUploadResult =
        cue::dx12::DX12GpuResource::create_buffer(*device->device(), {32, cue::GpuMemoryUsage::Upload});
    auto nativeOutputResult = cue::dx12::DX12GpuResource::create_buffer(*device->device(), {32});
    auto nativeReadbackResult =
        cue::dx12::DX12GpuResource::create_buffer(*device->device(), {32, cue::GpuMemoryUsage::Readback});
    if (!resourcesResult.has_value() || !nativeUploadResult.has_value() || !nativeOutputResult.has_value() ||
        !nativeReadbackResult.has_value())
    {
        return 8;
    }
    auto resources = resourcesResult.take_value();
    auto nativeUpload = nativeUploadResult.take_value();
    auto nativeOutput = nativeOutputResult.take_value();
    auto nativeReadback = nativeReadbackResult.take_value();
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC uavDesc{};
    uavDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    uavDesc.Width = 32;
    uavDesc.Height = 1;
    uavDesc.DepthOrArraySize = 1;
    uavDesc.MipLevels = 1;
    uavDesc.SampleDesc.Count = 1;
    uavDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    uavDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    Microsoft::WRL::ComPtr<ID3D12Resource> nativeUav;
    if (FAILED(device->device()->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &uavDesc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&nativeUav))) ||
        FAILED(nativeUav->SetName(L"CueEngine FrameGraph UAV Test")))
    {
        return 21;
    }
    constexpr std::array<std::uint32_t, 8> k_values = {0x12345678, 0x9abcdef0, 0x13572468, 0xdeadbeef,
                                                       0x24681357, 0xfedcba98, 0xabcdef01, 0x76543210};
    if (!nativeUpload->write(0, std::as_bytes(std::span{k_values})).has_value())
    {
        return 9;
    }
    auto queuePoolResult = cue::dx12::DX12QueuePool::create(*device);
    auto commandPoolResult = cue::dx12::DX12CommandPool::create(*device);
    if (!queuePoolResult.has_value() || !commandPoolResult.has_value())
    {
        return 10;
    }
    auto queuePool = queuePoolResult.take_value();
    auto commandPool = commandPoolResult.take_value();
    if (!check_compute_read(*device, *commandPool, *queuePool, false) ||
        !check_compute_read(*device, *commandPool, *queuePool, true))
    {
        std::fprintf(stderr, "Compute ShaderRead fixture failed\n");
        if (infoQueue)
        {
            for (UINT64 index = 0; index < infoQueue->GetNumStoredMessages(); ++index)
            {
                SIZE_T size = 0;
                if (FAILED(infoQueue->GetMessage(index, nullptr, &size)))
                    continue;
                std::vector<std::max_align_t> storage((size + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t));
                auto *message = reinterpret_cast<D3D12_MESSAGE *>(storage.data());
                if (SUCCEEDED(infoQueue->GetMessage(index, message, &size)))
                    std::fprintf(stderr, "D3D12 %u: %s\n", message->ID, message->pDescription);
            }
        }
        return 30;
    }
    auto queueResult = queuePool->acquire(cue::QueueType::Graphics);
    auto commandResult = commandPool->acquire(cue::QueueType::Graphics);
    if (!queueResult.has_value() || !commandResult.has_value())
    {
        return 11;
    }
    auto queue = queueResult.take_value();
    auto command = commandResult.take_value();
    auto *context = dynamic_cast<cue::dx12::DX12GpuCommandContext *>(command.get());
    if (!context)
    {
        return 12;
    }

    std::vector<cue::dx12::dx12FrameGraphPassCallback> callbacks;
    std::size_t recorded = 0;
    for (std::size_t index = 0; index < passes.size(); ++index)
    {
        // どの Pass も Resolver だけから Resource を取得し、Executor の順序を確認する
        callbacks.emplace_back(
            [&, index](ID3D12GraphicsCommandList &a_list,
                       const cue::dx12::DX12FrameGraphPassContext &a_pass) -> cue::Result<void>
            {
                if (recorded++ != index)
                {
                    return cue::Result<void>::failure({cue::ErrorCategory::InvalidState, "Pass order"});
                }
                if (index >= 5)
                {
                    return a_pass.resource(uav) == nativeUav.Get()
                               ? cue::Result<void>::success()
                               : cue::Result<void>::failure({cue::ErrorCategory::InvalidState, "UAV binding"});
                }
                auto *source = a_pass.resource(index == 0 || index == 2 ? upload
                                               : index == 1             ? first
                                               : index == 3             ? second
                                                                        : output);
                auto *destination = a_pass.resource(index == 0                 ? first
                                                    : index == 1 || index == 3 ? output
                                                    : index == 2               ? second
                                                                               : readback);
                if (!source || !destination || a_pass.resource({first.graphId + 1, first.index}))
                {
                    return cue::Result<void>::failure({cue::ErrorCategory::InvalidState, "Pass resource"});
                }
                const std::uint64_t sourceOffset = index == 2 ? 16 : 0;
                const std::uint64_t destinationOffset = index == 3 ? 16 : 0;
                const std::uint64_t bytes = index == 4 ? 32 : 16;
                a_list.CopyBufferRegion(destination, destinationOffset, source, sourceOffset, bytes);
                return cue::Result<void>::success();
            });
    }
    const std::array<cue::dx12::DX12FrameGraphExternalResource, 4> bindings = {{
        {upload, nativeUpload->resource()},
        {output, nativeOutput->resource()},
        {readback, nativeReadback->resource()},
        {uav, nativeUav.Get()},
    }};
    auto rebuiltResult = builder->build();
    if (!rebuiltResult.has_value() || rebuiltResult.try_value()->id() == plan.id() ||
        cue::dx12::DX12FrameGraphExecutor::record(*rebuiltResult.try_value(), *resources, bindings, callbacks, *context)
            .has_value() ||
        recorded != 0)
    {
        return 22;
    }
    auto invalidBindings = bindings;
    invalidBindings[1].resource = nativeUpload->resource();
    if (cue::dx12::DX12FrameGraphExecutor::record(plan, *resources, invalidBindings, callbacks, *context).has_value() ||
        recorded != 0)
    {
        return 23;
    }
    invalidBindings = bindings;
    invalidBindings[0].resource = nativeOutput->resource();
    invalidBindings[1].resource = nativeUpload->resource();
    if (cue::dx12::DX12FrameGraphExecutor::record(plan, *resources, invalidBindings, callbacks, *context).has_value() ||
        recorded != 0)
    {
        return 24;
    }
    // Binding の不足は記録前に拒否され、Callback を実行しない
    if (cue::dx12::DX12FrameGraphExecutor::record(plan, *resources, std::span{bindings}.first(3), callbacks, *context)
            .has_value() ||
        recorded != 0)
    {
        return 13;
    }
    if (!cue::dx12::DX12FrameGraphExecutor::record(plan, *resources, bindings, callbacks, *context).has_value() ||
        recorded != passes.size() || !command->close().has_value())
    {
        return 14;
    }
    auto submittedResult = commandPool->submit(*queue, *command);
    if (!submittedResult.has_value())
    {
        return 15;
    }
    std::shared_ptr<cue::ICommandCompletion> completion(submittedResult.take_value());
    if (!resources->mark_submitted(completion).has_value() || !resources->shutdown().has_value())
    {
        return 16;
    }
    std::array<std::uint32_t, k_values.size()> actual{};
    if (!nativeReadback->read(0, std::as_writable_bytes(std::span{actual})).has_value() || actual != k_values)
    {
        return 17;
    }
    command.reset();
    queue.reset();
    if (!commandPool->shutdown().has_value() || !queuePool->shutdown().has_value())
    {
        return 18;
    }
    if (infoQueue)
    {
        const auto count = infoQueue->GetNumStoredMessages();
        for (std::uint64_t index = 0; index < count; ++index)
        {
            SIZE_T bytes = 0;
            if (FAILED(infoQueue->GetMessage(index, nullptr, &bytes)))
            {
                return 19;
            }
            std::vector<std::max_align_t> storage((bytes + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t));
            auto *message = reinterpret_cast<D3D12_MESSAGE *>(storage.data());
            const auto retrieved = infoQueue->GetMessage(index, message, &bytes);
            if (SUCCEEDED(retrieved) && message->Severity == D3D12_MESSAGE_SEVERITY_WARNING)
                std::fprintf(stderr, "D3D12 warning %u: %s\n", message->ID, message->pDescription);
            if (FAILED(retrieved) || message->Severity == D3D12_MESSAGE_SEVERITY_ERROR ||
                message->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION)
            {
                std::fprintf(stderr, "D3D12: %s\n", message->pDescription);
                return 20;
            }
        }
    }
    return 0;
}
