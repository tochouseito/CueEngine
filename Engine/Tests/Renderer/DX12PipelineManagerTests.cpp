#include <DX12/DX12PipelineManager.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <string>

#include <DX12/DX12CommandPool.h>
#include <DX12/DX12GpuResource.h>
#include <DX12/DX12QueuePool.h>
#include <DX12/DX12RenderDevice.h>

namespace
{
/// @brief テスト失敗の経路でも Queue の CPU 制御 Gate を解除する
struct GateRelease final
{
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    /// @brief 未完了の Queue を待つ Owner の破棄前に Gate を開く
    ~GateRelease()
    {
        if (fence)
            (void)fence->Signal(1);
    }
};

/// @brief 無効な Handle と Shader 失敗を検証し、Manager 破棄後に提出済み PSO で GPU 書込みを行う
int run_tests()
{
    using namespace cue;
    using namespace cue::dx12;
    auto deviceResult = DX12RenderDevice::create(AdapterSelection::Warp);
    if (!deviceResult.has_value())
        return __LINE__;
    auto device = deviceResult.take_value();
    auto managerResult = DX12PipelineManager::create(*device);
    auto otherResult = DX12PipelineManager::create(*device);
    if (!managerResult.has_value() || !otherResult.has_value())
        return __LINE__;
    auto manager = managerResult.take_value();
    auto other = otherResult.take_value();
    ShaderCompileDesc bad{"Missing", "Hlsl/missing.hlsl", "vs_main", ShaderStage::Vertex};
    if (manager->create_shader_blob(bad).has_value())
        return __LINE__;
    bad.filePath = "Hlsl/FullscreenTriangle.hlsl";
    bad.entryPoint = "missing_entry";
    auto failed = manager->create_shader_blob(bad);
    if (failed.has_value() || failed.try_error()->operation.find("DXC.Compile") == std::string::npos)
        return __LINE__;
    bad.entryPoint = "vs_main";
    bad.targetProfile = "ps_6_0";
    if (manager->create_shader_blob(bad).has_value())
        return __LINE__;
    bad.targetProfile.clear();
    bad.filePath = std::string("Hlsl/test") + '\0' + ".hlsl";
    if (manager->create_shader_blob(bad).has_value())
        return __LINE__;
    RootSignatureDesc invalid;
    invalid.parameters.push_back({static_cast<RootParameterType>(255)});
    if (manager->create_root_signature(invalid).has_value())
        return __LINE__;
    invalid.parameters[0] = {RootParameterType::SrvTable, ShaderVisibility::Pixel, 0, 0, 0};
    if (manager->create_root_signature(invalid).has_value())
        return __LINE__;
    RootSignatureDesc graphicsRoot;
    graphicsRoot.parameters.push_back({RootParameterType::SrvTable, ShaderVisibility::Pixel});
    graphicsRoot.samplers.push_back({});
    auto rootResult = manager->create_root_signature(graphicsRoot);
    auto foreignRootResult = other->create_root_signature(graphicsRoot);
    auto vsResult = manager->create_shader_blob({"VS", "Hlsl/FullscreenTriangle.hlsl", "vs_main", ShaderStage::Vertex});
    auto psResult = manager->create_shader_blob({"PS", "Hlsl/FullscreenTriangle.hlsl", "ps_main", ShaderStage::Pixel});
    if (!rootResult.has_value() || !foreignRootResult.has_value() || !vsResult.has_value() || !psResult.has_value())
        return __LINE__;
    const auto root = rootResult.take_value();
    const auto vs = vsResult.take_value();
    const auto ps = psResult.take_value();
    GraphicsPipelineStateDesc desc;
    desc.rootSignature = root;
    desc.vertexShader = vs;
    desc.pixelShader = ps;
    auto graphicsResult = manager->create_graphics_pipeline(desc);
    if (!graphicsResult.has_value())
        return __LINE__;
    const auto graphics = graphicsResult.take_value();
    desc.rootSignature = foreignRootResult.take_value();
    if (manager->create_graphics_pipeline(desc).has_value())
        return __LINE__;
    desc.rootSignature = root;
    desc.vertexShader = ps;
    if (manager->create_graphics_pipeline(desc).has_value())
        return __LINE__;
    if (manager->retire(RootSignatureHandle{}).has_value() || other->retire(graphics).has_value() ||
        manager->validate_texture_binding(graphics, 1).has_value() ||
        !manager->validate_texture_binding(graphics, 0).has_value() ||
        manager->validate_bindings(graphics, {}).has_value())
        return __LINE__;
    auto poolResult = DX12CommandPool::create(*device);
    auto queueResult = DX12GpuCommandQueue::create(*device->device(), QueueType::Compute, 0);
    if (!poolResult.has_value() || !queueResult.has_value())
        return __LINE__;
    auto pool = poolResult.take_value();
    auto queue = queueResult.take_value();
    auto wrongCommandResult = pool->acquire(QueueType::Compute);
    if (!wrongCommandResult.has_value())
        return __LINE__;
    auto wrongCommand = wrongCommandResult.take_value();
    auto *compute = dynamic_cast<DX12GpuCommandContext *>(wrongCommand.get());
    if (!compute || manager->bind_graphics(*compute, graphics).has_value())
        return __LINE__;
    wrongCommand.reset();
    if (!manager->retire(graphics).has_value() || manager->retire(graphics).has_value() ||
        !manager->retire(root).has_value() || !manager->retire(vs).has_value() || !manager->retire(ps).has_value())
        return __LINE__;
    auto replacement = manager->create_root_signature({});
    if (!replacement.has_value() || manager->retire(root).has_value())
        return __LINE__;
    const auto newRoot = replacement.take_value();
    if (newRoot.index != root.index || newRoot.generation == root.generation)
        return __LINE__;
    if (!manager->retire(newRoot).has_value())
        return __LINE__;

    RootSignatureDesc computeRoot;
    computeRoot.parameters.push_back({RootParameterType::UnorderedAccess});
    auto computeRootResult = manager->create_root_signature(computeRoot);
    auto csResult = manager->create_shader_blob({"CS", CUE_TEST_SHADER_PATH, "cs_main", ShaderStage::Compute});
    if (!computeRootResult.has_value() || !csResult.has_value())
        return __LINE__;
    const auto cs = csResult.take_value();
    const auto csRoot = computeRootResult.take_value();
    auto computePipelineResult = manager->create_compute_pipeline({"Compute", csRoot, cs});
    if (!computePipelineResult.has_value())
        return __LINE__;
    const auto pipeline = computePipelineResult.take_value();
    auto commandResult = pool->acquire(QueueType::Compute);
    if (!commandResult.has_value())
        return __LINE__;
    auto command = commandResult.take_value();
    auto *nativeCommand = dynamic_cast<DX12GpuCommandContext *>(command.get());
    if (!nativeCommand || !manager->bind_compute(*nativeCommand, pipeline).has_value() ||
        manager->bind_graphics(*nativeCommand, pipeline).has_value())
        return __LINE__;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = 4;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    buffer.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    Microsoft::WRL::ComPtr<ID3D12Resource> output;
    // Default Buffer は Common で作り、初回の UAV 使用を明示的な Barrier で宣言する
    if (FAILED(device->device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                         D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&output))))
        return __LINE__;
    auto readbackResult = DX12GpuResource::create_buffer(*device->device(), {4, GpuMemoryUsage::Readback});
    if (!readbackResult.has_value())
        return __LINE__;
    auto readback = readbackResult.take_value();
    auto *list = nativeCommand->command_list();
    D3D12_RESOURCE_BARRIER initialBarrier{};
    initialBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    initialBarrier.Transition = {output.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COMMON,
                                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
    list->ResourceBarrier(1, &initialBarrier);
    list->SetComputeRootUnorderedAccessView(0, output->GetGPUVirtualAddress());
    list->Dispatch(1, 1, 1);
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {output.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                          D3D12_RESOURCE_STATE_COPY_SOURCE};
    list->ResourceBarrier(1, &barrier);
    list->CopyBufferRegion(readback->resource(), 0, output.Get(), 0, 4);
    if (!command->close().has_value())
        return __LINE__;
    GateRelease gate;
    if (FAILED(device->device()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate.fence))) ||
        FAILED(queue->command_queue()->Wait(gate.fence.Get(), 1)))
        return __LINE__;
    auto submitted = pool->submit(*queue, *command);
    if (!submitted.has_value())
        return __LINE__;
    auto completion = submitted.take_value();
    if (completion->is_complete())
        return __LINE__;
    // Queue がまだ実行できない時点で Manager と Handle を破棄し、Command の保持だけで実行する
    if (!manager->retire(pipeline).has_value() || !manager->retire(csRoot).has_value() ||
        !manager->retire(cs).has_value())
        return __LINE__;
    manager.reset();
    command.reset();
    if (FAILED(gate.fence->Signal(1)) || !completion->wait().has_value())
        return __LINE__;
    std::uint32_t value = 0;
    if (!readback->read(0, std::as_writable_bytes(std::span{&value, 1})).has_value())
        return __LINE__;
    const bool isCorrect = value == 0x12345678;
    if (!isCorrect || !pool->shutdown().has_value())
        return __LINE__;
    return 0;
}
} // namespace

/// @brief WARP と実 DXC で Pipeline の世代、Stage と GPU 完了までの寿命を検証する
int main()
{
    const int result = run_tests();
    if (result)
        std::fprintf(stderr, "DX12PipelineManager test failed at line %d\n", result);
    return result;
}
