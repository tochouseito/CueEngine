#include <Cue/Renderer/FrameGraph/FrameGraph.h>

#include "D3D12CommandPool.h"
#include "D3D12CommandRecorder.h"
#include "D3D12GraphExecutor.h"
#include "D3D12PipelineLibrary.h"
#include "D3D12QueuePool.h"
#include "D3D12ResourcePool.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#define CHECK(a_condition) do { if (!(a_condition)) { return __LINE__; } } while (false)

/// @brief Copy、Compute、Graphics の GPU 実行順と RHI Command を Readback 値で確認する
int main()
{
    using namespace cue;
    auto deviceResult = detail::D3D12DeviceContext::create();
    CHECK(deviceResult.has_value());
    auto device = deviceResult.take_value();
    auto queuesResult = detail::D3D12QueuePool::create(*device);
    CHECK(queuesResult.has_value());
    auto queues = queuesResult.take_value();
    auto resourcesResult = detail::D3D12ResourcePool::create(*device, *queues);
    CHECK(resourcesResult.has_value());
    auto resources = resourcesResult.take_value();
    auto pipelines = detail::D3D12PipelineLibrary::create(*device, *queues);
    auto graphicsPoolResult = detail::D3D12CommandPool::create(*device, GpuQueueType::Graphics);
    auto computePoolResult = detail::D3D12CommandPool::create(*device, GpuQueueType::Compute);
    auto copyPoolResult = detail::D3D12CommandPool::create(*device, GpuQueueType::Copy);
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
    auto shaderResult = pipelines->create_shader({k_computeSource, "main", GpuShaderStage::Compute, "cs_6_0"});
    CHECK(shaderResult.has_value());
    const auto shader = shaderResult.take_value();
    auto rootResult = pipelines->create_root_signature({{{GpuViewKind::UnorderedAccess, 0, 0}}});
    CHECK(rootResult.has_value());
    const auto root = rootResult.take_value();
    auto pipelineResult = pipelines->create_compute_pipeline({root, shader});
    CHECK(pipelineResult.has_value());
    const auto pipeline = pipelineResult.take_value();
    auto invalidShader = pipelines->create_shader({"void main( {", "main", GpuShaderStage::Compute});
    CHECK(!invalidShader.has_value() && invalidShader.try_error()->operation.find("DXC:") == 0);

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
    auto graphicsPipelineResult = pipelines->create_graphics_pipeline(graphicsDesc);
    CHECK(graphicsPipelineResult.has_value());
    const auto graphicsPipeline = graphicsPipelineResult.take_value();
    CHECK(!pipelines->destroy_root_signature(graphicsRoot).has_value());

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
    std::vector<detail::GraphPassCallback> callbacks;
    callbacks.emplace_back([&](ID3D12GraphicsCommandList* a_list) {
        detail::D3D12CommandRecorder recorder(a_list, GpuQueueType::Copy, *resources, *pipelines);
        if (recorder.draw(3, 1).has_value())
        {
            return Result<void>::failure({ErrorCategory::InvalidState, "MultiQueue.CopyDraw"});
        }
        return recorder.copy_buffer(storage, 0, upload, 0, sizeof(input));
    });
    callbacks.emplace_back([&](ID3D12GraphicsCommandList* a_list) {
        detail::D3D12CommandRecorder recorder(a_list, GpuQueueType::Compute, *resources, *pipelines);
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
        detail::D3D12CommandRecorder recorder(a_list, GpuQueueType::Graphics, *resources, *pipelines);
        auto bindResult = recorder.bind_pipeline(graphicsPipeline);
        if (!bindResult.has_value())
        {
            return bindResult;
        }
        return recorder.copy_buffer(readback, 0, storage, 0, sizeof(input));
    });
    auto executeResult = detail::D3D12GraphExecutor::execute(
        compiled, *queues, {graphicsPool.get(), computePool.get(), copyPool.get()}, 0,
        physical, callbacks);
    CHECK(executeResult.has_value());
    CHECK(graphicsPool->wait_for_slot(queues->context(GpuQueueType::Graphics), 0).has_value());
    CHECK(computePool->wait_for_slot(queues->context(GpuQueueType::Compute), 0).has_value());
    CHECK(copyPool->wait_for_slot(queues->context(GpuQueueType::Copy), 0).has_value());
    auto secondResult = detail::D3D12GraphExecutor::execute(
        compiled, *queues, {graphicsPool.get(), computePool.get(), copyPool.get()}, 0,
        physical, callbacks);
    CHECK(secondResult.has_value());
    std::array<std::uint32_t, k_count> output{};
    CHECK(resources->read_buffer(readback, 0, output.data(), sizeof(output)).has_value());
    for (std::uint32_t index = 0; index < k_count; ++index)
    {
        CHECK(output[index] == input[index] + 7);
    }
    CHECK(pipelines->destroy_shader(shader).has_value() == false);
    CHECK(pipelines->destroy_pipeline(graphicsPipeline).has_value());
    CHECK(!pipelines->destroy_pipeline(graphicsPipeline).has_value());
    CHECK(pipelines->destroy_root_signature(graphicsRoot).has_value());
    CHECK(pipelines->destroy_shader(vertex).has_value());
    CHECK(pipelines->destroy_shader(pixel).has_value());
    CHECK(pipelines->destroy_pipeline(pipeline).has_value());
    CHECK(pipelines->destroy_root_signature(root).has_value());
    CHECK(pipelines->destroy_shader(shader).has_value());
    CHECK(resources->destroy_view(uav).has_value());
    CHECK(resources->destroy(storage).has_value());
    CHECK(resources->destroy(upload).has_value());
    CHECK(resources->destroy(readback).has_value());
    return 0;
}
