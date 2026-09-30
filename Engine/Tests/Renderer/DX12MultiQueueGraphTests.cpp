#include <Cue/Renderer/FrameGraph/FrameGraph.h>

#include "DX12CommandPool.h"
#include "DX12CommandRecorder.h"
#include "DX12GraphExecutor.h"
#include "DX12GraphResourceBindings.h"
#include "DX12PipelineManager.h"
#include "DX12QueuePool.h"
#include "DX12ResourcePool.h"

#include <array>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

#include <d3d12sdklayers.h>

#define CHECK(a_condition) do { if (!(a_condition)) { std::fprintf(stderr, "Check failed: %d\n", __LINE__); return __LINE__; } } while (false)

/// @brief Copy、Compute、Graphics の GPU 実行順と RHI Command を Readback 値で確認する
int main()
{
    using namespace cue;
    auto deviceResult = detail::DX12RenderDevice::create();
    CHECK(deviceResult.has_value());
    auto device = deviceResult.take_value();
    Microsoft::WRL::ComPtr<ID3D12InfoQueue> infoQueue;
    if (SUCCEEDED(device->device()->QueryInterface(IID_PPV_ARGS(&infoQueue))))
    {
        infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, false);
        infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, false);
    }
    auto queuesResult = detail::DX12QueuePool::create(*device);
    CHECK(queuesResult.has_value());
    auto queues = queuesResult.take_value();
    auto resourcesResult = detail::DX12ResourcePool::create(*device, *queues);
    CHECK(resourcesResult.has_value());
    auto resources = resourcesResult.take_value();
    auto pipelinesResult = detail::DX12PipelineManager::create(*device, *queues);
    CHECK(pipelinesResult.has_value());
    auto pipelines = pipelinesResult.take_value();
    auto graphicsPoolResult = detail::DX12CommandPool::create(*device, GpuQueueType::Graphics);
    auto computePoolResult = detail::DX12CommandPool::create(*device, GpuQueueType::Compute);
    auto copyPoolResult = detail::DX12CommandPool::create(*device, GpuQueueType::Copy);
    CHECK(graphicsPoolResult.has_value() && computePoolResult.has_value() && copyPoolResult.has_value());
    auto graphicsPool = graphicsPoolResult.take_value();
    auto computePool = computePoolResult.take_value();
    auto copyPool = copyPoolResult.take_value();
    auto recordingResult = graphicsPool->acquire_batch(queues->context(GpuQueueType::Graphics), 1);
    CHECK(recordingResult.has_value());
    const auto recording = recordingResult.take_value();
    CHECK(!graphicsPool->wait_for_slot(queues->context(GpuQueueType::Graphics), 1).has_value());
    CHECK(graphicsPool->abort(recording).has_value());

    constexpr std::uint32_t k_count = 16;
    std::array<std::uint32_t, k_count> input{};
    for (std::uint32_t index = 0; index < k_count; ++index)
    {
        input[index] = index * 3;
    }
    auto uploadResult = resources->create_buffer({256, GpuMemory::Upload});
    auto storageResult = resources->create_buffer({256, GpuMemory::Device, true});
    auto readbackResult = resources->create_buffer({256, GpuMemory::Readback});
    CHECK(uploadResult.has_value() && storageResult.has_value() && readbackResult.has_value());
    const auto upload = uploadResult.take_value();
    const auto storage = storageResult.take_value();
    const auto readback = readbackResult.take_value();
    CHECK(resources->write_buffer(upload, 0, input.data(), sizeof(input)).has_value());
    auto uavResult = resources->create_view(storage, {GpuViewKind::UnorderedAccess, 0, sizeof(input)});
    CHECK(uavResult.has_value());
    const auto uav = uavResult.take_value();

    constexpr const char* k_computeSource = R"(
        RWByteAddressBuffer dataBuffer : register(u0);
        [numthreads(16, 1, 1)]
        void main(uint3 id : SV_DispatchThreadID)
        {
            uint offset = id.x * 4;
            dataBuffer.Store(offset, dataBuffer.Load(offset) + 7);
        }
    )";
    auto shaderResult = pipelines->create_shader({k_computeSource, "main", GpuShaderStage::Compute,
                                                  "cs_6_0", "ComputeShader"});
    CHECK(shaderResult.has_value());
    const auto shader = shaderResult.take_value();
    auto rootResult = pipelines->create_root_signature({{{GpuViewKind::UnorderedAccess, 0, 0}},
                                                        "ComputeRoot"});
    CHECK(rootResult.has_value());
    const auto root = rootResult.take_value();
    auto pipelineResult = pipelines->create_compute_pipeline({root, shader, "ComputePipeline"});
    CHECK(pipelineResult.has_value());
    const auto pipeline = pipelineResult.take_value();
    IPipelineManager& pipelineApi = *pipelines;
    CHECK(pipelineApi.get_shader("ComputeShader").has_value());
    CHECK(pipelineApi.get_root_signature("ComputeRoot").has_value());
    CHECK(pipelineApi.get_compute_pipeline("ComputePipeline").has_value());
    CHECK(!pipelines->create_compute_pipeline({root, shader, "ComputePipeline"}).has_value());
    auto invalidShader = pipelines->create_shader({"void main( {", "main", GpuShaderStage::Compute});
    CHECK(!invalidShader.has_value() && invalidShader.try_error()->operation.find("DXC:") == 0);
    GpuShaderDesc fileShaderDesc{};
    fileShaderDesc.entry = "VSMain";
    fileShaderDesc.stage = GpuShaderStage::Vertex;
    fileShaderDesc.name = "FileShader";
    fileShaderDesc.filePath = CUE_TEST_SHADER_PATH;
    fileShaderDesc.enableDebugInfo = false;
    auto fileShaderResult = pipelines->create_shader(fileShaderDesc);
    CHECK(fileShaderResult.has_value());
    CHECK(pipelines->destroy_shader(fileShaderResult.take_value()).has_value());

    GpuRootSignatureDesc legacyRootDesc{};
    legacyRootDesc.parameters = {
        {GpuRootParameterType::TableUav, GpuShaderVisibility::All, 0, 2},
        {GpuRootParameterType::Constants32, GpuShaderVisibility::All, 1},
        {GpuRootParameterType::Cbv, GpuShaderVisibility::Vertex, 2}};
    auto legacyRootResult = pipelines->create_root_signature(legacyRootDesc);
    CHECK(legacyRootResult.has_value());
    CHECK(pipelines->destroy_root_signature(legacyRootResult.take_value()).has_value());
    GpuRootSignatureDesc unboundedRootDesc{};
    unboundedRootDesc.parameters = {{GpuRootParameterType::TableSrv, GpuShaderVisibility::All, 0, 0}};
    CHECK(!pipelines->create_root_signature(unboundedRootDesc).has_value());

    // 固定 Mesh とは別の Graphics PSO も生成し、Graphics Pass で Bind する
    constexpr const char* k_vertexSource = R"(
        float4 main(uint id : SV_VertexID) : SV_Position
        {
            return float4(0.0, 0.0, 0.5, 1.0);
        }
    )";
    constexpr const char* k_pixelSource = R"(
        float4 main() : SV_Target0
        {
            return float4(1.0, 0.0, 0.0, 1.0);
        }
    )";
    auto vertexResult = pipelines->create_shader({k_vertexSource, "main", GpuShaderStage::Vertex});
    auto pixelResult = pipelines->create_shader({k_pixelSource, "main", GpuShaderStage::Pixel});
    auto graphicsRootResult = pipelines->create_root_signature({});
    CHECK(vertexResult.has_value() && pixelResult.has_value() && graphicsRootResult.has_value());
    const auto vertex = vertexResult.take_value();
    const auto pixel = pixelResult.take_value();
    const auto graphicsRoot = graphicsRootResult.take_value();
    GpuGraphicsPipelineDesc graphicsDesc{};
    graphicsDesc.rootSignature = graphicsRoot;
    graphicsDesc.vertexShader = vertex;
    graphicsDesc.pixelShader = pixel;
    graphicsDesc.name = "GraphicsPipeline";
    auto graphicsPipelineResult = pipelines->create_graphics_pipeline(graphicsDesc);
    CHECK(graphicsPipelineResult.has_value());
    const auto graphicsPipeline = graphicsPipelineResult.take_value();
    CHECK(pipelineApi.get_graphics_pipeline("GraphicsPipeline").has_value());
    CHECK(!pipelines->create_graphics_pipeline(graphicsDesc).has_value());
    CHECK(!pipelines->destroy_root_signature(graphicsRoot).has_value());
    auto variedDesc = graphicsDesc;
    variedDesc.name = "LegacyRasterPipeline";
    variedDesc.blendMode = GpuBlendMode::Additive;
    variedDesc.fillMode = GpuFillMode::Wireframe;
    variedDesc.topology = GpuPrimitiveTopology::Line;
    variedDesc.cullMode = GpuCullMode::Front;
    variedDesc.hasDepth = true;
    variedDesc.depthFormat = GpuTextureFormat::Depth24Stencil8;
    variedDesc.colorFormats = {GpuTextureFormat::Rgba8Unorm};
    variedDesc.blendModes = {GpuBlendMode::Additive};
    variedDesc.depthWrite = false;
    variedDesc.frontCounterClockwise = true;
    variedDesc.depthBias = 1;
    auto variedResult = pipelines->create_graphics_pipeline(variedDesc);
    CHECK(variedResult.has_value());
    CHECK(pipelines->destroy_pipeline(variedResult.take_value()).has_value());

    // 旧間接描画の Root 定数と 24 byte Signature の生成を GPU Device で確認する
    GpuRootSignatureDesc indirectRootDesc{};
    indirectRootDesc.parameters = {{GpuRootParameterType::Constants32, GpuShaderVisibility::All, 0}};
    auto indirectRootResult = pipelines->create_root_signature(indirectRootDesc);
    CHECK(indirectRootResult.has_value());
    const auto indirectRoot = indirectRootResult.take_value();
    auto indirectDesc = graphicsDesc;
    indirectDesc.rootSignature = indirectRoot;
    indirectDesc.name = "IndirectPipeline";
    auto indirectPipelineResult = pipelines->create_graphics_pipeline(indirectDesc);
    CHECK(indirectPipelineResult.has_value());
    const auto indirectPipeline = indirectPipelineResult.take_value();
    CHECK(pipelines->indirect_signature(indirectPipeline).has_value());
    CHECK(!pipelines->indirect_signature(graphicsPipeline).has_value());
    CHECK(pipelines->destroy_pipeline(indirectPipeline).has_value());
    CHECK(pipelines->destroy_root_signature(indirectRoot).has_value());

    FrameGraphBuilder graph;
    auto uploadNodeResult = graph.import_resource("Upload", GraphResourceState::CopySource,
                                                 GraphResourceState::CopySource);
    auto storageNodeResult = graph.import_resource("Storage", GraphResourceState::Common,
                                                  GraphResourceState::Common);
    auto readbackNodeResult = graph.import_resource("Readback", GraphResourceState::CopyDest,
                                                   GraphResourceState::CopyDest);
    CHECK(uploadNodeResult.has_value() && storageNodeResult.has_value() && readbackNodeResult.has_value());
    const auto uploadNode = uploadNodeResult.take_value();
    const auto storageNode = storageNodeResult.take_value();
    const auto readbackNode = readbackNodeResult.take_value();
    CHECK(graph.add_pass("Upload", {{uploadNode, GraphResourceState::CopySource, GraphAccess::Read},
                                      {storageNode, GraphResourceState::CopyDest, GraphAccess::Write}},
                         GpuQueueType::Copy).has_value());
    CHECK(graph.add_pass("Compute", {{storageNode, GraphResourceState::UnorderedAccess,
                                       GraphAccess::ReadWrite}}, GpuQueueType::Compute).has_value());
    CHECK(graph.add_pass("Readback", {{storageNode, GraphResourceState::CopySource, GraphAccess::Read},
                                        {readbackNode, GraphResourceState::CopyDest, GraphAccess::Write}},
                         GpuQueueType::Graphics).has_value());
    CHECK(!graph.add_pass("InvalidCopy", {{storageNode, GraphResourceState::UnorderedAccess,
                                            GraphAccess::Write}}, GpuQueueType::Copy).has_value());
    auto graphResult = graph.compile();
    CHECK(graphResult.has_value());
    const auto compiled = graphResult.take_value();
    CHECK(compiled.passes.size() == 3 && compiled.passes[1].predecessors.size() == 1 &&
          compiled.passes[2].predecessors.size() == 2);

    auto uploadPhysical = resources->resource(upload);
    auto storagePhysical = resources->resource(storage);
    auto readbackPhysical = resources->resource(readback);
    CHECK(uploadPhysical.has_value() && storagePhysical.has_value() && readbackPhysical.has_value());
    const std::vector<ID3D12Resource*> physical = {uploadPhysical.take_value(), storagePhysical.take_value(),
                                                    readbackPhysical.take_value()};
    detail::DX12GraphResourceBindings graphBindings(graph);
    CHECK(graphBindings.bind(uploadNode, physical[0]).has_value());
    CHECK(!graphBindings.bind(storageNode, physical[0]).has_value());
    CHECK(graphBindings.bind(storageNode, physical[1]).has_value());
    CHECK(graphBindings.bind(readbackNode, physical[2]).has_value());
    CHECK(graphBindings.resolve().has_value());
    std::vector<detail::GraphPassCallback> callbacks;
    callbacks.emplace_back([&](ID3D12GraphicsCommandList* a_list) {
        detail::DX12CommandRecorder recorder(a_list, GpuQueueType::Copy, *resources, *pipelines);
        if (recorder.draw(3, 1).has_value())
        {
            return Result<void>::failure({ErrorCategory::InvalidState, "MultiQueue.CopyDraw"});
        }
        return recorder.copy_buffer_region({storage, GpuMemory::Device, 0, 0,
                                            upload, GpuMemory::Upload, 0, 0, sizeof(input)});
    });
    callbacks.emplace_back([&](ID3D12GraphicsCommandList* a_list) {
        detail::DX12CommandRecorder recorder(a_list, GpuQueueType::Compute, *resources, *pipelines);
        auto bindResult = recorder.bind_pipeline(pipeline);
        if (!bindResult.has_value())
        {
            return bindResult;
        }
        auto viewResult = recorder.bind_view(0, uav);
        if (!viewResult.has_value())
        {
            return viewResult;
        }
        return recorder.dispatch(1, 1, 1);
    });
    callbacks.emplace_back([&](ID3D12GraphicsCommandList* a_list) {
        detail::DX12CommandRecorder recorder(a_list, GpuQueueType::Graphics, *resources, *pipelines);
        auto bindResult = recorder.bind_pipeline(graphicsPipeline);
        if (!bindResult.has_value())
        {
            return bindResult;
        }
        return recorder.copy_buffer_region({readback, GpuMemory::Readback, 0, 0,
                                            storage, GpuMemory::Device, 0, 0, sizeof(input)});
    });
    auto executeResult = detail::DX12GraphExecutor::execute(
        compiled, *queues, {graphicsPool.get(), computePool.get(), copyPool.get()}, 0,
        physical, callbacks);
    CHECK(executeResult.has_value());
    CHECK(!detail::DX12GraphExecutor::execute(
        compiled, *queues, {graphicsPool.get(), computePool.get(), copyPool.get()}, 0,
        {physical[0], physical[0], physical[2]}, callbacks).has_value());
    CHECK(graphicsPool->wait_for_slot(queues->context(GpuQueueType::Graphics), 0).has_value());
    CHECK(computePool->wait_for_slot(queues->context(GpuQueueType::Compute), 0).has_value());
    CHECK(copyPool->wait_for_slot(queues->context(GpuQueueType::Copy), 0).has_value());
    auto secondResult = detail::DX12GraphExecutor::execute(
        compiled, *queues, {graphicsPool.get(), computePool.get(), copyPool.get()}, 0,
        physical, callbacks);
    CHECK(secondResult.has_value());
    std::array<std::uint32_t, k_count> output{};
    CHECK(resources->read_buffer(readback, 0, output.data(), sizeof(output)).has_value());
    for (std::uint32_t index = 0; index < k_count; ++index)
    {
        CHECK(output[index] == input[index] + 7);
    }

    // Graphics の Texture 書込から COPY Queue への引渡しは COMMON を経由する
    auto colorResult = resources->create_texture({8, 8, GpuTextureFormat::Rgba8Unorm});
    CHECK(colorResult.has_value());
    const auto color = colorResult.take_value();
    auto colorViewResult = resources->create_view(color, GpuViewKind::RenderTarget);
    CHECK(colorViewResult.has_value());
    const auto colorView = colorViewResult.take_value();
    auto colorPointer = resources->resource(color);
    CHECK(colorPointer.has_value());
    const auto colorDesc = (*colorPointer.try_value())->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 readbackBytes = 0;
    device->device()->GetCopyableFootprints(&colorDesc, 0, 1, 0, &footprint,
                                             nullptr, nullptr, &readbackBytes);
    auto textureReadbackResult = resources->create_buffer({readbackBytes, GpuMemory::Readback});
    CHECK(textureReadbackResult.has_value());
    const auto textureReadback = textureReadbackResult.take_value();
    auto readbackPointer = resources->resource(textureReadback);
    CHECK(readbackPointer.has_value());
    FrameGraphBuilder textureGraph;
    auto colorNode = textureGraph.import_resource("QueueColor", GraphResourceState::Common,
                                                   GraphResourceState::Common);
    auto textureReadbackNode = textureGraph.import_resource("QueueReadback", GraphResourceState::CopyDest,
                                                             GraphResourceState::CopyDest);
    CHECK(colorNode.has_value() && textureReadbackNode.has_value());
    const auto colorGraph = colorNode.take_value();
    const auto readbackGraph = textureReadbackNode.take_value();
    CHECK(textureGraph.add_pass("ColorClear", {{colorGraph, GraphResourceState::RenderTarget,
                                                GraphAccess::Write}}, GpuQueueType::Graphics).has_value());
    CHECK(textureGraph.add_pass("ColorReadback", {{colorGraph, GraphResourceState::CopySource,
                                                   GraphAccess::Read},
                                                  {readbackGraph, GraphResourceState::CopyDest,
                                                   GraphAccess::Write}}, GpuQueueType::Copy).has_value());
    auto texturePlan = textureGraph.compile();
    CHECK(texturePlan.has_value());
    std::vector<detail::GraphPassCallback> textureCallbacks;
    textureCallbacks.emplace_back([&](ID3D12GraphicsCommandList* a_list) {
        detail::DX12CommandRecorder recorder(a_list, GpuQueueType::Graphics, *resources, *pipelines);
        return recorder.clear_color(colorView, {1.0f, 0.0f, 0.0f, 1.0f});
    });
    textureCallbacks.emplace_back([&](ID3D12GraphicsCommandList* a_list) {
        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = *colorPointer.try_value();
        source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = *readbackPointer.try_value();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        destination.PlacedFootprint = footprint;
        a_list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        return Result<void>::success();
    });
    auto textureExecuteResult = detail::DX12GraphExecutor::execute(texturePlan.take_value(), *queues,
          {graphicsPool.get(), computePool.get(), copyPool.get()}, 1,
          {*colorPointer.try_value(), *readbackPointer.try_value()}, textureCallbacks);
    if (!textureExecuteResult.has_value())
    {
        std::fprintf(stderr, "Texture graph failed: %s\n", textureExecuteResult.try_error()->operation.c_str());
        return __LINE__;
    }
    std::vector<std::uint8_t> texturePixels(static_cast<std::size_t>(readbackBytes));
    CHECK(resources->read_buffer(textureReadback, 0, texturePixels.data(), readbackBytes).has_value());
    if (infoQueue)
    {
        bool hasDiagnosticError = false;
        for (UINT64 index = 0; index < infoQueue->GetNumStoredMessages(); ++index)
        {
            SIZE_T size = 0;
            infoQueue->GetMessage(index, nullptr, &size);
            std::vector<std::uint8_t> bytes(size);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(bytes.data());
            if (SUCCEEDED(infoQueue->GetMessage(index, message, &size)) &&
                message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR)
            {
                std::fprintf(stderr, "D3D12: %s\n", message->pDescription);
                hasDiagnosticError = true;
            }
        }
        CHECK(!hasDiagnosticError);
    }
    CHECK(texturePixels[0] == 255 && texturePixels[1] == 0 && texturePixels[2] == 0 &&
          texturePixels[3] == 255);
    CHECK(resources->destroy_view(colorView).has_value());
    CHECK(resources->destroy(color).has_value());
    CHECK(resources->destroy(textureReadback).has_value());
    CHECK(pipelines->destroy_shader(shader).has_value() == false);
    CHECK(pipelines->destroy_pipeline(graphicsPipeline).has_value());
    CHECK(!pipelineApi.get_graphics_pipeline("GraphicsPipeline").has_value());
    CHECK(!pipelines->destroy_pipeline(graphicsPipeline).has_value());
    CHECK(pipelines->destroy_root_signature(graphicsRoot).has_value());
    CHECK(pipelines->destroy_shader(vertex).has_value());
    CHECK(pipelines->destroy_shader(pixel).has_value());
    CHECK(pipelines->destroy_pipeline(pipeline).has_value());
    CHECK(!pipelineApi.get_compute_pipeline("ComputePipeline").has_value());
    CHECK(pipelines->destroy_root_signature(root).has_value());
    CHECK(!pipelineApi.get_root_signature("ComputeRoot").has_value());
    CHECK(pipelines->destroy_shader(shader).has_value());
    CHECK(!pipelineApi.get_shader("ComputeShader").has_value());
    CHECK(resources->destroy_view(uav).has_value());
    CHECK(resources->destroy(storage).has_value());
    CHECK(resources->destroy(upload).has_value());
    CHECK(resources->destroy(readback).has_value());
    return 0;
}
