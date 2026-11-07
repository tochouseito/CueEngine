#include <SceneRender/SceneRenderer.h>

#include <algorithm>
#include <array>
#include <map>
#include <mutex>
#include <new>
#include <optional>
#include <utility>

#include <Assets/BuiltinMesh.h>
#include <DX12/DX12Backend.h>
#include <DX12/DX12FrameGraphContext.h>
#include <DX12/DX12GpuResource.h>
#include <DX12/DX12PipelineManager.h>
#include <DX12/DX12RenderDevice.h>
#include <Math/Projection.h>
#include <Platform/Diagnostics.h>
#include <RHI/MeshPool.h>

namespace cue
{
namespace
{
constexpr std::size_t k_maxDraws = 4096;
constexpr std::size_t k_maxFrames = 64;

/// @brief Shader と Command Signature が共有する 24 Byte の描画レコード
struct IndirectDraw final
{
    std::uint32_t transformIndex = 0;
    D3D12_DRAW_INDEXED_ARGUMENTS arguments{};
};
static_assert(sizeof(IndirectDraw) == 24 && offsetof(IndirectDraw, arguments) == 4);

struct SceneRendererState final
{
    std::mutex mutex;
    IBackend *backend = nullptr;
    IMeshPool *meshes = nullptr;
    const dx12::DX12ResourceContext *resources = nullptr;
    MeshHandle cube;
    std::map<std::uint64_t, SceneRenderSnapshot> pending;
    std::optional<SceneRenderSnapshot> active;
    std::optional<std::uint64_t> lastPublished;
    bool isStopped = false;
    bool isRendering = false;
};

/// @brief Graph の GPU 枠ごとに行列・Indirect 引数と Mesh 借用を維持する
struct MeshFrame final
{
    std::unique_ptr<dx12::DX12GpuResource> matrixUpload;
    std::unique_ptr<dx12::DX12GpuResource> matrices;
    std::unique_ptr<dx12::DX12GpuResource> argumentUpload;
    std::unique_ptr<dx12::DX12GpuResource> arguments;
    std::vector<meshLease> meshes;
};

/// @brief Graph が寿命を所有し、FinalColor / Depth と静的 Mesh から 3D 描画を記録する
class MeshPass final : public FrameGraphPass
{
  public:
    /// @brief Renderer の共有設定を Graph の破棄まで保持する
    explicit MeshPass(std::shared_ptr<SceneRendererState> a_state) : m_state(std::move(a_state))
    {
    }
    /// @brief Graph の GPU 完了後に Frame 資源と CPU Pin を返す
    ~MeshPass() override = default;
    /// @brief PIX / GPU 計測で使う Pass 名を返す
    const char *name() const noexcept override
    {
        return "MeshPass";
    }
    /// @brief Copy と Draw を同じ Graphics Queue 上で直列に実行する
    QueueType type() const noexcept override
    {
        return QueueType::Graphics;
    }
    /// @brief Graph の色・Depth・Root / Shader / PSO を生成する
    Result<void> setup(FrameGraphBuilder &a_builder) override
    {
        auto color = a_builder.get_texture("FinalColorTexture");
        if (!color.has_value())
            return Result<void>::failure(*color.try_error());
        m_color = color.take_value();
        auto colorDesc = a_builder.texture_desc(m_color);
        if (!colorDesc.has_value())
            return Result<void>::failure(*colorDesc.try_error());
        m_format = colorDesc.try_value()->format;
        GpuTexture2DDesc depthDesc;
        depthDesc.width = colorDesc.try_value()->width;
        depthDesc.height = colorDesc.try_value()->height;
        depthDesc.format = GpuTextureFormat::D32Float;
        depthDesc.isDepthStencil = true;
        auto depth = a_builder.create_transient_texture2d("SceneDepth", depthDesc);
        if (!depth.has_value())
            return Result<void>::failure(*depth.try_error());
        m_depth = depth.take_value();

        RootSignatureDesc root;
        root.name = "MeshPass.Root";
        root.parameters = {{RootParameterType::Constants, ShaderVisibility::Vertex, 0, 0, 1},
                           {RootParameterType::ShaderResource, ShaderVisibility::Vertex, 0}};
        auto rootResult = a_builder.create_root_signature(std::move(root));
        if (!rootResult.has_value())
            return Result<void>::failure(*rootResult.try_error());
        auto vertex =
            a_builder.create_shader_blob({"MeshPass.VS", "Hlsl/StaticMesh.hlsl", "vs_main", ShaderStage::Vertex});
        if (!vertex.has_value())
            return Result<void>::failure(*vertex.try_error());
        auto pixel =
            a_builder.create_shader_blob({"MeshPass.PS", "Hlsl/StaticMesh.hlsl", "ps_main", ShaderStage::Pixel});
        if (!pixel.has_value())
            return Result<void>::failure(*pixel.try_error());
        GraphicsPipelineStateDesc pipeline;
        pipeline.name = "MeshPass.Pipeline";
        pipeline.rootSignature = rootResult.take_value();
        pipeline.vertexShader = vertex.take_value();
        pipeline.pixelShader = pixel.take_value();
        pipeline.renderTargetFormat = m_format;
        pipeline.isDepthEnabled = true;
        pipeline.depthFormat = GpuTextureFormat::D32Float;
        pipeline.cullMode = CullMode::Back;
        pipeline.isFrontCounterClockwise = true;
        pipeline.inputElements = {{"POSITION", 0, 0, 0, VertexElementFormat::Float3},
                                  {"NORMAL", 0, 1, 0, VertexElementFormat::Float3}};
        auto pipelineResult = a_builder.create_graphics_pipeline(std::move(pipeline));
        if (!pipelineResult.has_value())
            return Result<void>::failure(*pipelineResult.try_error());
        m_pipeline = pipelineResult.take_value();
        auto signature = m_state->resources->pipelines.create_mesh_command_signature(m_pipeline, 0);
        if (!signature.has_value())
            return Result<void>::failure(*signature.try_error());
        m_signature = signature.take_value();
        return Result<void>::success();
    }
    /// @brief Graph に色と Depth の書込みを宣言して Barrier と DSV の準備を依頼する
    Result<void> describe_resources(FrameGraphBuilder &a_builder) override
    {
        auto color = a_builder.use(m_color, FrameGraphAccess::Write, FrameGraphResourceState::RenderTarget);
        return color.has_value() ? a_builder.use(m_depth, FrameGraphAccess::Write, FrameGraphResourceState::DepthWrite)
                                 : std::move(color);
    }
    /// @brief 選択した所有 Snapshot だけを読み、Page ごとに Bind して ExecuteIndirect を記録する
    Result<void> execute(FrameGraphContext &a_context) override
    {
        auto pipeline = a_context.set_graphics_pipeline(m_pipeline);
        if (!pipeline.has_value())
            return pipeline;
        auto targets = a_context.set_render_targets(m_color, m_depth);
        if (!targets.has_value())
            return targets;
        auto clear = a_context.clear_depth(m_depth, 1.0f);
        if (!clear.has_value())
            return clear;
        auto viewport = a_context.set_viewport_scissor(a_context.width(), a_context.height());
        if (!viewport.has_value())
            return viewport;
        auto *native = dynamic_cast<dx12::DX12FrameGraphContext *>(&a_context);
        if (!native)
            return Result<void>::failure({ErrorCategory::InvalidArgument, "MeshPass.context"});
        // render_frame が active の寿命を囲む。同じ Render Thread からのみ参照する
        const auto &snapshot = m_state->active;
        if (!snapshot || snapshot->objects.empty())
            return Result<void>::success();
        if (!snapshot->camera || snapshot->objects.size() > k_maxDraws || a_context.frame_index() >= k_maxFrames)
            return Result<void>::failure({ErrorCategory::InvalidArgument, "MeshPass.snapshot"});
        auto projection = math::perspective_rh_zo(snapshot->camera->verticalFovRadians,
                                                  float(a_context.width()) / float(a_context.height()),
                                                  snapshot->camera->nearClip, snapshot->camera->farClip);
        if (!projection.has_value())
            return Result<void>::failure(*projection.try_error());
        try
        {
            auto &frame = m_frames[a_context.frame_index()];
            if (!frame)
            {
                auto created = create_frame();
                if (!created.has_value())
                    return Result<void>::failure(*created.try_error());
                frame = created.take_value();
            }
            // begin_frame 済みの同じ GPU 枠なので、前回参照が完了してから CPU Pin を返す
            frame->meshes.clear();
            std::vector<std::array<float, 16>> matrices;
            matrices.reserve(snapshot->objects.size());
            frame->meshes.reserve(snapshot->objects.size());
            for (const auto &object : snapshot->objects)
            {
                if (object.meshId != k_cubeMeshId)
                    return Result<void>::failure({ErrorCategory::InvalidArgument, "MeshPass.mesh_id"});
                auto lease = m_state->meshes->acquire(m_state->cube);
                if (!lease.has_value())
                    return Result<void>::failure(*lease.try_error());
                frame->meshes.push_back(lease.take_value());
                const auto wvp = object.world * snapshot->camera->view * *projection.try_value();
                if (!math::is_finite(wvp))
                    return Result<void>::failure({ErrorCategory::InvalidArgument, "MeshPass.matrix"});
                std::array<float, 16> packed{};
                for (std::size_t row = 0; row < 4; ++row)
                    for (std::size_t column = 0; column < 4; ++column)
                        packed[row * 4 + column] = wvp.values[row][column];
                matrices.push_back(packed);
            }
            // 同じ Page / Index 幅の Draw をまとめ、VB / IB の切替を Batch の境界へ限定する
            std::vector<std::size_t> order(matrices.size());
            for (std::size_t index = 0; index < order.size(); ++index)
                order[index] = index;
            std::stable_sort(
                order.begin(), order.end(), [&](std::size_t a_left, std::size_t a_right)
                { return frame->meshes[a_left]->draw_range().pageId < frame->meshes[a_right]->draw_range().pageId; });
            std::vector<IndirectDraw> arguments;
            arguments.reserve(order.size());
            for (const auto index : order)
            {
                const auto &draw = frame->meshes[index]->draw_range();
                arguments.push_back(
                    {static_cast<std::uint32_t>(index), {draw.indexCount, 1, draw.firstIndex, draw.baseVertex, 0}});
            }
            auto matrixWrite = frame->matrixUpload->write(0, std::as_bytes(std::span{matrices}));
            auto argumentWrite = frame->argumentUpload->write(0, std::as_bytes(std::span{arguments}));
            if (!matrixWrite.has_value() || !argumentWrite.has_value())
                return !matrixWrite.has_value() ? std::move(matrixWrite) : std::move(argumentWrite);
            return native->record_external_graphics(
                m_format,
                [&](ID3D12GraphicsCommandList &a_list)
                {
                    // 各 GPU 枠の常駐 Buffer を転送後にだけ読む。同じ Graphics Queue 内で順序を保証する
                    copy_buffer(a_list, *frame->matrices, *frame->matrixUpload, matrices.size() * sizeof(matrices[0]),
                                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                    copy_buffer(a_list, *frame->arguments, *frame->argumentUpload,
                                arguments.size() * sizeof(IndirectDraw), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
                    a_list.SetGraphicsRootShaderResourceView(1, frame->matrices->resource()->GetGPUVirtualAddress());
                    a_list.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                    for (std::size_t first = 0; first < order.size();)
                    {
                        const auto &draw = frame->meshes[order[first]]->draw_range();
                        std::size_t end = first + 1;
                        while (end < order.size() && frame->meshes[order[end]]->draw_range().pageId == draw.pageId)
                            ++end;
                        std::array<D3D12_VERTEX_BUFFER_VIEW, 2> vertices{};
                        for (std::size_t stream = 0; stream < vertices.size(); ++stream)
                        {
                            auto *buffer = dynamic_cast<dx12::DX12GpuResource *>(draw.vertices[stream].resource);
                            if (!buffer)
                                return Result<void>::failure({ErrorCategory::InvalidState, "MeshPass.vertex_stream"});
                            vertices[stream] = {buffer->resource()->GetGPUVirtualAddress(),
                                                draw.vertices[stream].byteSize, draw.vertices[stream].stride};
                        }
                        auto *indexBuffer = dynamic_cast<dx12::DX12GpuResource *>(draw.indices.resource);
                        if (!indexBuffer)
                            return Result<void>::failure({ErrorCategory::InvalidState, "MeshPass.index_stream"});
                        const D3D12_INDEX_BUFFER_VIEW indexView{
                            indexBuffer->resource()->GetGPUVirtualAddress(), draw.indices.byteSize,
                            draw.indexFormat == MeshIndexFormat::Uint16 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT};
                        a_list.IASetVertexBuffers(0, static_cast<UINT>(vertices.size()), vertices.data());
                        a_list.IASetIndexBuffer(&indexView);
                        a_list.ExecuteIndirect(m_signature.Get(), static_cast<UINT>(end - first),
                                               frame->arguments->resource(), first * sizeof(IndirectDraw), nullptr, 0);
                        first = end;
                    }
                    // 次回枠の転送開始条件を COMMON に固定する
                    transition(a_list, *frame->matrices, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                               D3D12_RESOURCE_STATE_COMMON);
                    transition(a_list, *frame->arguments, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,
                               D3D12_RESOURCE_STATE_COMMON);
                    return Result<void>::success();
                });
        }
        catch (const std::bad_alloc &)
        {
            return Result<void>::failure({ErrorCategory::PlatformFailure, "MeshPass.allocation"});
        }
    }

  private:
    /// @brief 同じ GPU 枠を再利用し、毎 Frame の Committed Resource 生成を避ける
    Result<std::unique_ptr<MeshFrame>> create_frame()
    {
        auto frame = std::make_unique<MeshFrame>();
        auto &device = *m_state->resources->device.device();
        const auto create = [&](std::unique_ptr<dx12::DX12GpuResource> &a_output, std::uint64_t a_bytes,
                                GpuMemoryUsage a_usage, const wchar_t *a_name) -> Result<void>
        {
            auto result = dx12::DX12GpuResource::create_buffer(device, {a_bytes, a_usage}, a_name);
            if (!result.has_value())
                return Result<void>::failure(*result.try_error());
            a_output = result.take_value();
            return Result<void>::success();
        };
        auto result =
            create(frame->matrixUpload, k_maxDraws * 64, GpuMemoryUsage::Upload, L"CueEngine Scene Matrix Upload");
        if (!result.has_value())
            return Result<std::unique_ptr<MeshFrame>>::failure(*result.try_error());
        result = create(frame->matrices, k_maxDraws * 64, GpuMemoryUsage::Default, L"CueEngine Scene Matrices");
        if (!result.has_value())
            return Result<std::unique_ptr<MeshFrame>>::failure(*result.try_error());
        result = create(frame->argumentUpload, k_maxDraws * sizeof(IndirectDraw), GpuMemoryUsage::Upload,
                        L"CueEngine Scene Indirect Upload");
        if (!result.has_value())
            return Result<std::unique_ptr<MeshFrame>>::failure(*result.try_error());
        result = create(frame->arguments, k_maxDraws * sizeof(IndirectDraw), GpuMemoryUsage::Default,
                        L"CueEngine Scene Indirect Arguments");
        if (!result.has_value())
            return Result<std::unique_ptr<MeshFrame>>::failure(*result.try_error());
        return Result<std::unique_ptr<MeshFrame>>::success(std::move(frame));
    }
    /// @brief 明示 State の Buffer Barrier を記録する
    static void transition(ID3D12GraphicsCommandList &a_list, dx12::DX12GpuResource &a_resource,
                           D3D12_RESOURCE_STATES a_before, D3D12_RESOURCE_STATES a_after)
    {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = a_resource.resource();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = a_before;
        barrier.Transition.StateAfter = a_after;
        a_list.ResourceBarrier(1, &barrier);
    }
    /// @brief COMMON の Default Buffer を更新し、同じ List の読取り State へ渡す
    static void copy_buffer(ID3D12GraphicsCommandList &a_list, dx12::DX12GpuResource &a_destination,
                            dx12::DX12GpuResource &a_source, std::uint64_t a_bytes, D3D12_RESOURCE_STATES a_readState)
    {
        transition(a_list, a_destination, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        a_list.CopyBufferRegion(a_destination.resource(), 0, a_source.resource(), 0, a_bytes);
        transition(a_list, a_destination, D3D12_RESOURCE_STATE_COPY_DEST, a_readState);
    }

    std::shared_ptr<SceneRendererState> m_state;
    FrameGraphResourceHandle m_color;
    FrameGraphResourceHandle m_depth;
    PipelineStateHandle m_pipeline;
    GpuTextureFormat m_format = GpuTextureFormat::Rgba8Unorm;
    Microsoft::WRL::ComPtr<ID3D12CommandSignature> m_signature;
    std::array<std::unique_ptr<MeshFrame>, k_maxFrames> m_frames;
};

class SceneRenderer final : public ISceneRenderer
{
  public:
    /// @brief Factory が準備した初期 Cube と Frame 転送状態を所有する
    explicit SceneRenderer(std::shared_ptr<SceneRendererState> a_state) : m_state(std::move(a_state))
    {
    }
    /// @brief Graph は呼出側で先に破棄し、未停止の Mesh を退役させる
    ~SceneRenderer() override
    {
        auto result = shutdown();
        if (!result.has_value())
            report_log_error("SceneRenderer.shutdown", *result.try_error(), LogLevel::Error);
    }
    /// @brief 非消費 Frame の容量と値を検証して公開する
    Result<void> publish_frame(std::uint64_t a_frame, SceneRenderSnapshot a_snapshot) override
    {
        const std::lock_guard lock(m_state->mutex);
        if (m_state->isStopped || m_state->pending.size() >= k_maxFrames ||
            (m_state->lastPublished && a_frame <= *m_state->lastPublished) || a_snapshot.objects.size() > k_maxDraws ||
            (!a_snapshot.objects.empty() && !a_snapshot.camera))
            return Result<void>::failure({ErrorCategory::InvalidState, "SceneRenderer.publish_frame"});
        for (const auto &object : a_snapshot.objects)
            if (!math::is_finite(object.world) || object.meshId != k_cubeMeshId)
                return Result<void>::failure({ErrorCategory::InvalidArgument, "SceneRenderer.object"});
        try
        {
            m_state->pending.emplace(a_frame, std::move(a_snapshot));
            m_state->lastPublished = a_frame;
            return Result<void>::success();
        }
        catch (const std::bad_alloc &)
        {
            return Result<void>::failure({ErrorCategory::PlatformFailure, "SceneRenderer.publish_allocation"});
        }
    }
    /// @brief 同 Frame の Snapshot を固定し、成功・取消・失敗のどの場合も借用を終える
    Result<void> render_frame(std::uint64_t a_frame, const std::function<Result<void>()> &a_record) override
    {
        {
            const std::lock_guard lock(m_state->mutex);
            auto found = m_state->pending.find(a_frame);
            if (m_state->isStopped || m_state->isRendering || !a_record || found == m_state->pending.end())
                return Result<void>::failure({ErrorCategory::InvalidState, "SceneRenderer.render_frame"});
            m_state->active = std::move(found->second);
            m_state->pending.erase(m_state->pending.begin(), std::next(found));
            m_state->isRendering = true;
        }
        auto result = Result<void>::success();
        try
        {
            result = a_record();
        }
        catch (...)
        {
            result = Result<void>::failure({ErrorCategory::PlatformFailure, "SceneRenderer.record_exception"});
        }
        {
            const std::lock_guard lock(m_state->mutex);
            m_state->active.reset();
            m_state->isRendering = false;
        }
        return result;
    }
    /// @brief Resize 後にも新しい論理 Handle と Frame 資源を使う Pass を作る
    Result<std::unique_ptr<FrameGraphPass>> create_pass() override
    {
        const std::lock_guard lock(m_state->mutex);
        if (m_state->isStopped)
            return Result<std::unique_ptr<FrameGraphPass>>::failure(
                {ErrorCategory::InvalidState, "SceneRenderer.create_pass"});
        try
        {
            return Result<std::unique_ptr<FrameGraphPass>>::success(std::make_unique<MeshPass>(m_state));
        }
        catch (const std::bad_alloc &)
        {
            return Result<std::unique_ptr<FrameGraphPass>>::failure(
                {ErrorCategory::PlatformFailure, "SceneRenderer.pass_allocation"});
        }
    }
    /// @brief 静止境界で未描画 Frame を捨て、単調な Frame ID は維持する
    Result<void> discard_pending() override
    {
        const std::lock_guard lock(m_state->mutex);
        if (m_state->isRendering)
            return Result<void>::failure({ErrorCategory::InvalidState, "SceneRenderer.discard_rendering"});
        m_state->pending.clear();
        return Result<void>::success();
    }
    /// @brief Graph の全 CPU / GPU 参照が消えた後で Cube の公開を止める
    Result<void> shutdown() override
    {
        const std::lock_guard lock(m_state->mutex);
        if (m_state->isStopped)
            return Result<void>::success();
        if (m_state->isRendering)
            return Result<void>::failure({ErrorCategory::InvalidState, "SceneRenderer.shutdown_rendering"});
        auto retired = m_state->meshes->retire(m_state->cube);
        if (!retired.has_value())
            return retired;
        m_state->isStopped = true;
        m_state->pending.clear();
        auto collected = m_state->meshes->collect();
        return collected.has_value() ? Result<void>::success() : Result<void>::failure(*collected.try_error());
    }

  private:
    std::shared_ptr<SceneRendererState> m_state;
};
} // namespace

Result<std::unique_ptr<ISceneRenderer>> create_scene_renderer(IBackend &a_backend)
{
    auto *native = dynamic_cast<dx12::DX12Backend *>(&a_backend);
    if (!native || !native->get_resource_context() || !native->get_mesh_pool())
        return Result<std::unique_ptr<ISceneRenderer>>::failure(
            {ErrorCategory::InvalidArgument, "SceneRenderer.backend"});
    auto cube = create_cube_mesh();
    if (!cube.has_value())
        return Result<std::unique_ptr<ISceneRenderer>>::failure(*cube.try_error());
    auto uploaded = native->get_mesh_pool()->upload(*cube.try_value());
    if (!uploaded.has_value())
        return Result<std::unique_ptr<ISceneRenderer>>::failure(*uploaded.try_error());
    const auto handle = uploaded.take_value();
    auto ready = native->get_mesh_pool()->wait_ready(handle);
    if (!ready.has_value())
    {
        auto retired = native->get_mesh_pool()->retire(handle);
        (void)retired;
        return Result<std::unique_ptr<ISceneRenderer>>::failure(*ready.try_error());
    }
    try
    {
        auto state = std::make_shared<SceneRendererState>();
        state->backend = &a_backend;
        state->resources = native->get_resource_context();
        state->meshes = native->get_mesh_pool();
        state->cube = handle;
        return Result<std::unique_ptr<ISceneRenderer>>::success(std::make_unique<SceneRenderer>(std::move(state)));
    }
    catch (const std::bad_alloc &)
    {
        auto retired = native->get_mesh_pool()->retire(handle);
        (void)retired;
        return Result<std::unique_ptr<ISceneRenderer>>::failure(
            {ErrorCategory::PlatformFailure, "SceneRenderer.create_allocation"});
    }
}
} // namespace cue
