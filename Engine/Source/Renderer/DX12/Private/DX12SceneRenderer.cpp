#include <SceneRender/SceneRenderer.h>

#include "DX12SceneMaterials.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <map>
#include <mutex>
#include <new>
#include <numeric>
#include <optional>
#include <span>
#include <string_view>
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
constexpr std::uint32_t k_maxDraws = 4096;
constexpr std::size_t k_maxFrames = 64;
constexpr std::uint32_t k_depthBins = 8;
constexpr std::uint32_t k_computeGroupSize = 64;

using matrixRecord = std::array<float, 16>;

// HLSL と Command Signature が共有する Buffer Record
struct Candidate final
{
    std::uint32_t transformIndex;
    std::uint32_t groupIndex;
    std::uint32_t materialId;
};
struct MeshRange final
{
    std::uint32_t indexCount;
    std::uint32_t firstIndex;
    std::int32_t baseVertex;
};
struct Page final
{
    std::uint32_t firstGroup;
    std::uint32_t groupCount;
    std::uint32_t firstCommand;
};
struct Batch final
{
    std::uint32_t count;
    std::uint32_t instanceStart;
    std::uint32_t written;
};
struct IndirectDraw final
{
    std::uint32_t instanceStart;
    D3D12_DRAW_INDEXED_ARGUMENTS arguments;
};
static_assert(sizeof(Candidate) == 12 && sizeof(MeshRange) == 12 && sizeof(Page) == 12 && sizeof(Batch) == 12);
static_assert(sizeof(IndirectDraw) == 24 && offsetof(IndirectDraw, arguments) == 4);

/// @brief Project Mesh の Pool Handle を複数の公開世代で共有する
/// Backend が Pool を所有し、記録済み GPU Frame の Lease は退役後も転送・描画完了まで有効
struct RegisteredMesh final
{
    /// @brief 同じ Geometry を再登録した場合の比較に CPU 正本を保持する
    RegisteredMesh(IMeshPool &a_pool, std::shared_ptr<const MeshData> a_source)
        : pool(a_pool), source(std::move(a_source))
    {
    }

    /// @brief 最後の公開世代が参照を返した時点で、新規 Lease の取得を止める
    ~RegisteredMesh()
    {
        if (handle.is_valid())
        {
            auto retired = pool.retire(handle);
            if (!retired.has_value())
                report_log_error("SceneRenderer.retire_mesh", *retired.try_error(), LogLevel::Error);
        }
    }

    IMeshPool &pool;
    std::shared_ptr<const MeshData> source;
    MeshHandle handle;
};

using sceneMeshSet = std::map<std::string, std::shared_ptr<const RegisteredMesh>, std::less<>>;

/// @brief Asset を読み直しても同じ値なら、現在の GPU Upload と Page を再利用できる
bool is_same_geometry(const MeshData &a_left, const MeshData &a_right)
{
    return &a_left == &a_right ||
           (a_left.index_format() == a_right.index_format() &&
            std::ranges::equal(a_left.positions(), a_right.positions()) &&
            std::ranges::equal(a_left.normals(), a_right.normals()) &&
            std::ranges::equal(a_left.uv0(), a_right.uv0()) &&
            std::ranges::equal(a_left.indices16(), a_right.indices16()) &&
            std::ranges::equal(a_left.indices32(), a_right.indices32()));
}

/// @brief 全 Project Mesh を検証し、Copy Fence 完了済みの次世代を部分公開せずに作る
Result<std::shared_ptr<const sceneMeshSet>>
prepare_meshes(IMeshPool &a_pool, const sceneMeshSet &a_current, std::span<const SceneMeshDesc> a_meshes)
{
    using MeshSetResult = Result<std::shared_ptr<const sceneMeshSet>>;
    try
    {
        auto next = std::make_shared<sceneMeshSet>();
        for (const auto &asset : a_meshes)
        {
            // 組み込み Cube は Project Mesh と独立して保持する。描画は全頂点 Stream を必要とする
            if (!is_valid_uuid_text(asset.assetId) || !asset.mesh || asset.mesh->positions().empty() ||
                asset.mesh->normals().empty() || asset.mesh->uv0().empty() || asset.mesh->index_count() == 0 ||
                !next->try_emplace(asset.assetId).second)
                return MeshSetResult::failure(
                    {ErrorCategory::InvalidArgument, "SceneRenderer.mesh_definition:" + asset.assetId});
        }
        for (const auto &asset : a_meshes)
        {
            auto &registration = next->at(asset.assetId);
            const auto previous = a_current.find(asset.assetId);
            if (previous != a_current.end() && is_same_geometry(*previous->second->source, *asset.mesh))
            {
                registration = previous->second;
                continue;
            }
            auto uploaded = std::make_shared<RegisteredMesh>(a_pool, asset.mesh);
            auto handle = a_pool.upload(*asset.mesh);
            if (!handle.has_value())
                return MeshSetResult::failure(*handle.try_error());
            uploaded->handle = handle.take_value();
            auto ready = a_pool.wait_ready(uploaded->handle);
            if (!ready.has_value())
                return MeshSetResult::failure(*ready.try_error());
            registration = std::move(uploaded);
        }
        return MeshSetResult::success(std::move(next));
    }
    catch (const std::bad_alloc &)
    {
        return MeshSetResult::failure({ErrorCategory::PlatformFailure, "SceneRenderer.mesh_allocation"});
    }
}

struct SceneRendererState final
{
    /// @brief 公開時点の Mesh と Material の世代を Scene の値と一緒に固定する
    struct PublishedFrame final
    {
        SceneRenderSnapshot snapshot;
        std::shared_ptr<const sceneMeshSet> meshAssets;
        std::shared_ptr<const dx12::SceneMaterialSet> materials;
    };

    std::mutex mutex;
    IMeshPool *meshes = nullptr;
    dx12::DX12Backend *backend = nullptr;
    const dx12::DX12ResourceContext *resources = nullptr;
    MeshHandle cube;
    std::shared_ptr<const sceneMeshSet> meshAssets;
    std::shared_ptr<const dx12::SceneMaterialSet> materials;
    std::map<std::uint64_t, PublishedFrame> pending;
    std::optional<PublishedFrame> active;
    std::optional<std::uint64_t> lastPublished;
    bool isStopped = false;
    bool isRendering = false;
};

enum Buffer
{
    Transforms,
    Candidates,
    MeshRanges,
    Pages,
    Batches,
    Instances,
    Arguments,
    CommandCounts,
    BufferCount,
};

struct BufferSpec final
{
    const char *name;
    GpuBufferDesc desc;
};
constexpr std::array<BufferSpec, BufferCount> k_buffers = {{
    {"Scene.Transforms", {k_maxDraws * sizeof(matrixRecord)}},
    {"Scene.Candidates", {k_maxDraws * sizeof(Candidate)}},
    {"Scene.MeshRanges", {k_maxDraws * sizeof(MeshRange)}},
    {"Scene.Pages", {k_maxDraws * sizeof(Page)}},
    {"Scene.Batches", {k_maxDraws * k_depthBins * sizeof(Batch), GpuMemoryUsage::Default, true}},
    {"Scene.Instances", {k_maxDraws * sizeof(std::uint32_t), GpuMemoryUsage::Default, true}},
    {"Scene.IndirectArguments", {k_maxDraws * sizeof(IndirectDraw), GpuMemoryUsage::Default, true}},
    {"Scene.IndirectCounts", {k_maxDraws * sizeof(std::uint32_t), GpuMemoryUsage::Default, true}},
}};
constexpr std::array k_uploadBuffers = {Transforms, Candidates, MeshRanges, Pages};

struct DrawPage final
{
    Page gpu;
    std::uint32_t maxCommands;
    std::uint32_t meshLeaseIndex;
};

/// @brief GPU 枠の完了まで Upload と Page の借用を保持する
struct MeshFrame final
{
    std::unique_ptr<dx12::DX12GpuResource> upload;
    std::vector<meshLease> meshes;
    std::vector<DrawPage> pages;
    std::shared_ptr<const dx12::SceneMaterialSet> materials;
    std::uint32_t candidateCount = 0;
    std::uint32_t groupCount = 0;
};

/// @brief 描画 Pass 群が同じ Graph の資源と記録中の GPU 枠を参照する
struct MeshGraph final
{
    std::shared_ptr<SceneRendererState> scene;
    std::array<FrameGraphResourceHandle, BufferCount> buffers{};
    std::array<MeshFrame, k_maxFrames> frames{};
};

/// @brief Ready から転送中に変わった Page は今の Frame だけ見送る
Result<meshLease> acquire_ready(IMeshPool &a_pool, MeshHandle a_handle)
{
    auto status = a_pool.status(a_handle);
    if (!status.has_value())
        return Result<meshLease>::failure(*status.try_error());
    if (*status.try_value() == MeshStatus::Uploading)
        return Result<meshLease>::success(nullptr);
    auto lease = a_pool.acquire(a_handle);
    if (!lease.has_value())
    {
        status = a_pool.status(a_handle);
        if (status.has_value() && *status.try_value() == MeshStatus::Uploading)
            return Result<meshLease>::success(nullptr);
    }
    return lease;
}

/// @brief 同じ Mesh の範囲を再利用し、Compute が読む連続した候補を GPU 枠に転送する
class MeshUploadPass final : public FrameGraphPass
{
  public:
    /// @brief 間接描画 Pass 群と同じ Graph の資源を借用する
    explicit MeshUploadPass(std::shared_ptr<MeshGraph> a_graph) : m_graph(std::move(a_graph))
    {
    }

    /// @brief PIX とエラーに表示する Pass 名を返す
    const char *name() const noexcept override
    {
        return "Scene.Upload";
    }
    /// @brief Copy・Compute・Draw は同じ Queue 上で順序付ける
    QueueType type() const noexcept override
    {
        return QueueType::Graphics;
    }

    /// @brief 後続 Pass の入出力となる Default Buffer を確保する
    Result<void> setup(FrameGraphBuilder &a_builder) override
    {
        for (std::size_t index = 0; index < k_buffers.size(); ++index)
        {
            auto created = a_builder.create_transient_buffer(k_buffers[index].name, k_buffers[index].desc);
            if (!created.has_value())
                return Result<void>::failure(*created.try_error());
            m_graph->buffers[index] = created.take_value();
        }
        return Result<void>::success();
    }

    /// @brief Upload 先だけを宣言し、固定 State の Upload Heap は GPU 枠で保持する
    Result<void> describe_resources(FrameGraphBuilder &a_builder) override
    {
        for (const auto buffer : k_uploadBuffers)
        {
            auto used = a_builder.use(m_graph->buffers[buffer], FrameGraphAccess::Write,
                                      FrameGraphResourceState::CopyDestination);
            if (!used.has_value())
                return used;
        }
        return Result<void>::success();
    }

    /// @brief Snapshot の所有値から Mesh と Page を解決し、候補の参照を GPU へ公開する
    Result<void> execute(FrameGraphContext &a_context) override
    {
        auto *native = dynamic_cast<dx12::DX12FrameGraphContext *>(&a_context);
        if (!native || a_context.frame_index() >= m_graph->frames.size())
            return Result<void>::failure({ErrorCategory::InvalidArgument, "Scene.Upload.context"});
        auto &frame = m_graph->frames[a_context.frame_index()];
        // Graph が同じ GPU 枠の Fence を待った後なので、前回の Page を返せる
        frame.meshes.clear();
        frame.pages.clear();
        frame.materials.reset();
        frame.candidateCount = 0;
        frame.groupCount = 0;
        const auto &scene = *m_graph->scene;
        if (!scene.active || scene.active->snapshot.objects.empty())
            return Result<void>::success();
        const auto &snapshot = scene.active->snapshot;
        const auto &meshAssets = *scene.active->meshAssets;
        frame.materials = scene.active->materials;
        if (!snapshot.camera || !frame.materials)
            return Result<void>::failure({ErrorCategory::InvalidArgument, "Scene.Upload.camera"});
        const auto projection = math::perspective_rh_zo(snapshot.camera->verticalFovRadians,
                                                        float(a_context.width()) / float(a_context.height()),
                                                        snapshot.camera->nearClip, snapshot.camera->farClip);
        if (!projection.has_value())
            return Result<void>::failure(*projection.try_error());

        std::map<std::string_view, std::optional<std::uint32_t>> resolved;
        // Mesh と Material の存在する組だけを密に並べ、最大数の積で Batch を確保しない
        struct DrawGroup final
        {
            std::uint32_t meshLeaseIndex;
            std::uint32_t candidateCount = 0;
        };
        std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t> groupIndices;
        std::vector<DrawGroup> groups;
        std::vector<Candidate> candidates;
        std::vector<matrixRecord> transforms;
        candidates.reserve(snapshot.objects.size());
        transforms.reserve(snapshot.objects.size());
        for (const auto &object : snapshot.objects)
        {
            auto [entry, inserted] = resolved.try_emplace(object.meshId);
            if (inserted)
            {
                auto handle = scene.cube;
                if (object.meshId != k_cubeMeshId)
                {
                    const auto asset = meshAssets.find(object.meshId);
                    if (asset == meshAssets.end())
                        return Result<void>::failure({ErrorCategory::InvalidArgument, "Scene.Upload.mesh_id"});
                    handle = asset->second->handle;
                }
                auto acquired = acquire_ready(*scene.meshes, handle);
                if (!acquired.has_value())
                    return Result<void>::failure(*acquired.try_error());
                auto lease = acquired.take_value();
                if (lease)
                {
                    entry->second = static_cast<std::uint32_t>(frame.meshes.size());
                    frame.meshes.push_back(std::move(lease));
                }
            }
            if (!entry->second)
                continue;
            const auto meshIndex = *entry->second;
            const auto materialId = object.materialId ? frame.materials->ids.at(*object.materialId) : 0u;
            const auto [group, newGroup] = groupIndices.try_emplace(
                std::pair{meshIndex, materialId}, static_cast<std::uint32_t>(groups.size()));
            if (newGroup)
                groups.push_back({meshIndex});
            ++groups[group->second].candidateCount;
            const auto wvp = object.world * snapshot.camera->view * *projection.try_value();
            if (!math::is_finite(wvp))
                return Result<void>::failure({ErrorCategory::InvalidArgument, "Scene.Upload.transform"});
            matrixRecord packed{};
            for (std::size_t row = 0; row < 4; ++row)
                for (std::size_t column = 0; column < 4; ++column)
                    packed[row * 4 + column] = wvp.values[row][column];
            candidates.push_back({static_cast<std::uint32_t>(transforms.size()), group->second, materialId});
            transforms.push_back(packed);
        }
        if (candidates.empty())
            return Result<void>::success();

        std::vector<std::uint32_t> sortedGroups(groups.size());
        std::iota(sortedGroups.begin(), sortedGroups.end(), 0u);
        // CPU は VB/IB の Bind 単位を決める。Page 内の深度バッチは Compute で生成する
        std::stable_sort(
            sortedGroups.begin(), sortedGroups.end(), [&](std::uint32_t a_left, std::uint32_t a_right)
            {
                return frame.meshes[groups[a_left].meshLeaseIndex]->draw_range().pageId <
                       frame.meshes[groups[a_right].meshLeaseIndex]->draw_range().pageId;
            });

        std::vector<std::uint32_t> remap(groups.size());
        std::vector<MeshRange> meshRanges;
        std::vector<Page> pages;
        std::uint32_t commandOffset = 0;
        for (const auto index : sortedGroups)
        {
            const auto &group = groups[index];
            const auto &range = frame.meshes[group.meshLeaseIndex]->draw_range();
            if (frame.pages.empty() ||
                frame.meshes[frame.pages.back().meshLeaseIndex]->draw_range().pageId != range.pageId)
                frame.pages.push_back(
                    {{static_cast<std::uint32_t>(meshRanges.size()), 0, commandOffset}, 0, group.meshLeaseIndex});
            auto &page = frame.pages.back();
            ++page.gpu.groupCount;
            page.maxCommands += group.candidateCount;
            commandOffset += group.candidateCount;
            remap[index] = static_cast<std::uint32_t>(meshRanges.size());
            meshRanges.push_back({range.indexCount, range.firstIndex, range.baseVertex});
        }
        pages.reserve(frame.pages.size());
        for (const auto &page : frame.pages)
            pages.push_back(page.gpu);
        for (auto &candidate : candidates)
            candidate.groupIndex = remap[candidate.groupIndex];

        const std::array payloads = {std::as_bytes(std::span<const matrixRecord>{transforms}),
                                     std::as_bytes(std::span<const Candidate>{candidates}),
                                     std::as_bytes(std::span<const MeshRange>{meshRanges}),
                                     std::as_bytes(std::span<const Page>{pages})};
        if (!frame.upload)
        {
            std::uint64_t capacity = 0;
            for (const auto buffer : k_uploadBuffers)
                capacity += k_buffers[buffer].desc.byteSize;
            auto created = dx12::DX12GpuResource::create_buffer(
                *scene.resources->device.device(), {capacity, GpuMemoryUsage::Upload}, L"CueEngine Scene Upload");
            if (!created.has_value())
                return Result<void>::failure(*created.try_error());
            frame.upload = created.take_value();
        }
        std::uint64_t offset = 0;
        for (std::size_t index = 0; index < payloads.size(); ++index)
        {
            const auto payload = payloads[index];
            auto written = frame.upload->write(offset, payload);
            if (!written.has_value())
                return written;
            auto copied = native->copy_upload_buffer(*frame.upload, m_graph->buffers[k_uploadBuffers[index]],
                                                     payload.size(), offset);
            if (!copied.has_value())
                return copied;
            offset += payload.size();
        }
        frame.candidateCount = static_cast<std::uint32_t>(candidates.size());
        frame.groupCount = static_cast<std::uint32_t>(groups.size());
        return Result<void>::success();
    }

  private:
    std::shared_ptr<MeshGraph> m_graph;
};

struct ComputeBinding final
{
    Buffer buffer;
    RootParameterType type;
    std::uint32_t shaderRegister;
};
constexpr std::array k_resetBindings = {ComputeBinding{Batches, RootParameterType::UnorderedAccess, 0}};
constexpr std::array k_countBindings = {
    ComputeBinding{Transforms, RootParameterType::ShaderResource, 0},
    ComputeBinding{Candidates, RootParameterType::ShaderResource, 1},
    ComputeBinding{Batches, RootParameterType::UnorderedAccess, 0},
};
constexpr std::array k_prefixBindings = {
    ComputeBinding{Pages, RootParameterType::ShaderResource, 3},
    ComputeBinding{Batches, RootParameterType::UnorderedAccess, 0},
};
constexpr std::array k_fillBindings = {
    ComputeBinding{Transforms, RootParameterType::ShaderResource, 0},
    ComputeBinding{Candidates, RootParameterType::ShaderResource, 1},
    ComputeBinding{Batches, RootParameterType::UnorderedAccess, 0},
    ComputeBinding{Instances, RootParameterType::UnorderedAccess, 1},
};
constexpr std::array k_emitBindings = {
    ComputeBinding{MeshRanges, RootParameterType::ShaderResource, 2},
    ComputeBinding{Pages, RootParameterType::ShaderResource, 3},
    ComputeBinding{Batches, RootParameterType::ShaderResource, 4},
    ComputeBinding{Arguments, RootParameterType::UnorderedAccess, 2},
    ComputeBinding{CommandCounts, RootParameterType::UnorderedAccess, 3},
};

enum class Work
{
    Batch,
    Candidate,
    Page,
};
struct ComputeStage final
{
    const char *name;
    const char *entry;
    std::span<const ComputeBinding> bindings;
    Work work;
};
constexpr std::array<ComputeStage, 5> k_computeStages = {{
    {"Scene.ResetBatches", "reset_batches", k_resetBindings, Work::Batch},
    {"Scene.CountBatches", "count_batches", k_countBindings, Work::Candidate},
    {"Scene.PrefixBatches", "prefix_batches", k_prefixBindings, Work::Page},
    {"Scene.FillBatches", "fill_batches", k_fillBindings, Work::Candidate},
    {"Scene.EmitCommands", "emit_commands", k_emitBindings, Work::Page},
}};

class MeshComputePass final : public FrameGraphPass
{
  public:
    /// @brief 一つの Compute 段階と、その段階だけが使う Binding を設定する
    MeshComputePass(std::shared_ptr<MeshGraph> a_graph, ComputeStage a_stage)
        : m_graph(std::move(a_graph)), m_stage(a_stage)
    {
    }
    /// @brief GPU 計測で各段階を区別する
    const char *name() const noexcept override
    {
        return m_stage.name;
    }
    /// @brief Graphics Queue 上の Compute として Copy・Draw と同じ Timeline で動かす
    QueueType type() const noexcept override
    {
        return QueueType::Graphics;
    }

    /// @brief Shader の各 Entry に必要な Root と Compute PSO を生成する
    Result<void> setup(FrameGraphBuilder &a_builder) override
    {
        RootSignatureDesc root;
        root.name = std::string(name()) + ".Root";
        root.parameters = {{RootParameterType::Constants, ShaderVisibility::All, 0, 0, 3}};
        for (const auto &binding : m_stage.bindings)
            root.parameters.push_back({binding.type, ShaderVisibility::All, binding.shaderRegister});
        auto signature = a_builder.create_root_signature(std::move(root));
        if (!signature.has_value())
            return Result<void>::failure(*signature.try_error());
        auto shader =
            a_builder.create_shader_blob({name(), "Hlsl/IndirectStaticMesh.hlsl", m_stage.entry, ShaderStage::Compute});
        if (!shader.has_value())
            return Result<void>::failure(*shader.try_error());
        auto pipeline = a_builder.create_compute_pipeline({name(), signature.take_value(), shader.take_value()});
        if (!pipeline.has_value())
            return Result<void>::failure(*pipeline.try_error());
        m_pipeline = pipeline.take_value();
        return Result<void>::success();
    }

    /// @brief UAV の段階間依存と読み取りへの遷移を Graph に伝える
    Result<void> describe_resources(FrameGraphBuilder &a_builder) override
    {
        for (const auto &binding : m_stage.bindings)
        {
            const bool isUav = binding.type == RootParameterType::UnorderedAccess;
            auto used = a_builder.use(
                m_graph->buffers[binding.buffer], isUav ? FrameGraphAccess::Write : FrameGraphAccess::Read,
                isUav ? FrameGraphResourceState::UnorderedAccess : FrameGraphResourceState::NonPixelShaderRead);
            if (!used.has_value())
                return used;
        }
        return Result<void>::success();
    }

    /// @brief 実データ件数だけ Dispatch し、空 Frame の GPU 作業を省く
    Result<void> execute(FrameGraphContext &a_context) override
    {
        const auto &frame = m_graph->frames[a_context.frame_index()];
        if (frame.candidateCount == 0)
            return Result<void>::success();
        auto pipeline = a_context.set_compute_pipeline(m_pipeline);
        if (!pipeline.has_value())
            return pipeline;
        const auto pageCount = static_cast<std::uint32_t>(frame.pages.size());
        const std::array counts = {frame.candidateCount, frame.groupCount, pageCount};
        auto constants = a_context.set_root_constants(0, counts);
        if (!constants.has_value())
            return constants;
        for (std::uint32_t index = 0; index < m_stage.bindings.size(); ++index)
        {
            const auto &binding = m_stage.bindings[index];
            const auto handle = m_graph->buffers[binding.buffer];
            auto bound = binding.type == RootParameterType::UnorderedAccess
                             ? a_context.bind_buffer_uav(handle, index + 1)
                             : a_context.bind_buffer_srv(handle, index + 1);
            if (!bound.has_value())
                return bound;
        }
        std::uint32_t workCount = frame.candidateCount;
        if (m_stage.work == Work::Batch)
            workCount = frame.groupCount * k_depthBins;
        else if (m_stage.work == Work::Page)
            workCount = pageCount;
        return a_context.dispatch((workCount + k_computeGroupSize - 1) / k_computeGroupSize, 1, 1);
    }

  private:
    std::shared_ptr<MeshGraph> m_graph;
    ComputeStage m_stage;
    PipelineStateHandle m_pipeline;
};

/// @brief Compute が作成したコマンドを Page ごとに再生し、FinalColor と Depth を描画する
class MeshPass final : public FrameGraphPass
{
  public:
    /// @brief Mesh の借用と GPU 用バッチを生成側の Pass と共有する
    explicit MeshPass(std::shared_ptr<MeshGraph> a_graph) : m_graph(std::move(a_graph))
    {
    }
    /// @brief PIX と GPU 計測で使う Pass 名を返す
    const char *name() const noexcept override
    {
        return "MeshPass";
    }
    /// @brief 記録先は引数生成と同じ Graphics Queue
    QueueType type() const noexcept override
    {
        return QueueType::Graphics;
    }

    /// @brief Color・Depth・Material と ExecuteIndirect の描画設定を作る
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
        const auto &scene = *m_graph->scene;

        RootSignatureDesc root;
        root.name = "MeshPass.Root";
        root.parameters = {{RootParameterType::Constants, ShaderVisibility::Vertex, 0, 0, 1},
                           {RootParameterType::ShaderResource, ShaderVisibility::Vertex, 0},
                           {RootParameterType::ShaderResource, ShaderVisibility::Vertex, 1},
                           {RootParameterType::ShaderResource, ShaderVisibility::Vertex, 2},
                           {RootParameterType::ShaderResource, ShaderVisibility::Pixel, 3},
                           {RootParameterType::SrvTable, ShaderVisibility::Pixel, 4, 0, dx12::k_sceneTextureCount}};
        root.samplers = {{0, 0, ShaderVisibility::Pixel, SamplerFilter::Linear, SamplerAddressMode::Clamp}};
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
                                  {"NORMAL", 0, 1, 0, VertexElementFormat::Float3},
                                  {"TEXCOORD", 0, 2, 0, VertexElementFormat::Float2}};
        auto created = a_builder.create_graphics_pipeline(std::move(pipeline));
        if (!created.has_value())
            return Result<void>::failure(*created.try_error());
        m_pipeline = created.take_value();
        auto signature = scene.resources->pipelines.create_mesh_command_signature(m_pipeline, 0);
        if (!signature.has_value())
            return Result<void>::failure(*signature.try_error());
        m_signature = signature.take_value();
        return Result<void>::success();
    }

    /// @brief 描画先と Compute の書き込み結果を Graph に宣言する
    Result<void> describe_resources(FrameGraphBuilder &a_builder) override
    {
        const std::array uses = {
            FrameGraphUse{m_color, FrameGraphAccess::Write, FrameGraphResourceState::RenderTarget},
            FrameGraphUse{m_depth, FrameGraphAccess::Write, FrameGraphResourceState::DepthWrite},
            FrameGraphUse{m_graph->buffers[Transforms], FrameGraphAccess::Read,
                          FrameGraphResourceState::NonPixelShaderRead},
            FrameGraphUse{m_graph->buffers[Candidates], FrameGraphAccess::Read,
                          FrameGraphResourceState::NonPixelShaderRead},
            FrameGraphUse{m_graph->buffers[Instances], FrameGraphAccess::Read,
                          FrameGraphResourceState::NonPixelShaderRead},
            FrameGraphUse{m_graph->buffers[Arguments], FrameGraphAccess::Read,
                          FrameGraphResourceState::IndirectArgument},
            FrameGraphUse{m_graph->buffers[CommandCounts], FrameGraphAccess::Read,
                          FrameGraphResourceState::IndirectArgument},
        };
        for (const auto &use : uses)
        {
            auto used = a_builder.use(use.resource, use.access, use.state);
            if (!used.has_value())
                return used;
        }
        return Result<void>::success();
    }

    /// @brief バッチの先頭と描画インスタンス件数を GPU 生成の引数・Count Buffer から取得する
    Result<void> execute(FrameGraphContext &a_context) override
    {
        auto *native = dynamic_cast<dx12::DX12FrameGraphContext *>(&a_context);
        if (!native)
            return Result<void>::failure({ErrorCategory::InvalidArgument, "MeshPass.context"});
        const auto &frame = m_graph->frames[a_context.frame_index()];
        auto pipeline = a_context.set_graphics_pipeline(m_pipeline);
        if (!pipeline.has_value())
            return pipeline;
        auto targets = a_context.set_render_targets(m_color, m_depth);
        if (!targets.has_value())
            return targets;
        auto clear = a_context.clear_depth(m_depth, 1.0f);
        if (!clear.has_value() || frame.candidateCount == 0)
            return clear;
        auto viewport = a_context.set_viewport_scissor(a_context.width(), a_context.height());
        if (!viewport.has_value())
            return viewport;
        const std::array srvs = {m_graph->buffers[Transforms], m_graph->buffers[Candidates],
                                 m_graph->buffers[Instances]};
        for (std::uint32_t index = 0; index < srvs.size(); ++index)
        {
            auto bound = a_context.bind_buffer_srv(srvs[index], index + 1);
            if (!bound.has_value())
                return bound;
        }
        auto *arguments = native->indirect_buffer(m_graph->buffers[Arguments]);
        auto *counts = native->indirect_buffer(m_graph->buffers[CommandCounts]);
        if (!arguments || !counts)
            return Result<void>::failure({ErrorCategory::InvalidState, "MeshPass.indirect_buffers"});
        return native->record_external_graphics(
            m_format,
            [&](ID3D12GraphicsCommandList &a_list)
            {
                // Copy の完了後に公開した不変資源を Frame が保持し、COMMON は読取時に昇格する
                const auto &materials = *frame.materials;
                ID3D12DescriptorHeap *heaps[] = {materials.heap.Get()};
                a_list.SetDescriptorHeaps(1, heaps);
                a_list.SetGraphicsRootShaderResourceView(4, materials.records->resource()->GetGPUVirtualAddress());
                a_list.SetGraphicsRootDescriptorTable(5, materials.heap->GetGPUDescriptorHandleForHeapStart());
                a_list.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                for (std::size_t index = 0; index < frame.pages.size(); ++index)
                {
                    const auto &page = frame.pages[index];
                    const auto &draw = frame.meshes[page.meshLeaseIndex]->draw_range();
                    std::array<D3D12_VERTEX_BUFFER_VIEW, 3> vertices{};
                    for (std::size_t stream = 0; stream < vertices.size(); ++stream)
                    {
                        auto *buffer = dynamic_cast<dx12::DX12GpuResource *>(draw.vertices[stream].resource);
                        if (!buffer)
                            return Result<void>::failure({ErrorCategory::InvalidState, "MeshPass.vertex_stream"});
                        vertices[stream] = {buffer->resource()->GetGPUVirtualAddress(), draw.vertices[stream].byteSize,
                                            draw.vertices[stream].stride};
                    }
                    auto *indexBuffer = dynamic_cast<dx12::DX12GpuResource *>(draw.indices.resource);
                    if (!indexBuffer)
                        return Result<void>::failure({ErrorCategory::InvalidState, "MeshPass.index_stream"});
                    const D3D12_INDEX_BUFFER_VIEW indexView{
                        indexBuffer->resource()->GetGPUVirtualAddress(), draw.indices.byteSize,
                        draw.indexFormat == MeshIndexFormat::Uint16 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT};
                    a_list.IASetVertexBuffers(0, static_cast<UINT>(vertices.size()), vertices.data());
                    a_list.IASetIndexBuffer(&indexView);
                    a_list.ExecuteIndirect(m_signature.Get(), page.maxCommands, arguments,
                                           page.gpu.firstCommand * sizeof(IndirectDraw), counts,
                                           index * sizeof(std::uint32_t));
                }
                return Result<void>::success();
            });
    }

  private:
    std::shared_ptr<MeshGraph> m_graph;
    FrameGraphResourceHandle m_color;
    FrameGraphResourceHandle m_depth;
    PipelineStateHandle m_pipeline;
    GpuTextureFormat m_format = GpuTextureFormat::Rgba8Unorm;
    Microsoft::WRL::ComPtr<ID3D12CommandSignature> m_signature;
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
    /// @brief Copy Fence 完了済みの世代を、これから公開する Frame にだけ使用する
    Result<void> set_materials(std::span<const SceneMaterialDesc> a_materials) override
    {
        return replace_assets(std::nullopt, a_materials);
    }
    /// @brief Material を保持して Project Mesh の世代だけ置き換える
    Result<void> set_meshes(std::span<const SceneMeshDesc> a_meshes) override
    {
        return replace_assets(a_meshes, std::nullopt);
    }
    /// @brief Mesh と Material を一つの公開境界で置き換える
    Result<void> set_assets(std::span<const SceneMeshDesc> a_meshes,
                            std::span<const SceneMaterialDesc> a_materials) override
    {
        return replace_assets(a_meshes, a_materials);
    }
    /// @brief 非消費 Frame の容量と値を検証して公開する
    Result<void> publish_frame(std::uint64_t a_frame, SceneRenderSnapshot a_snapshot) override
    {
        const std::lock_guard lock(m_state->mutex);
        if (m_state->isStopped || !m_state->materials || !m_state->meshAssets || m_state->pending.size() >= k_maxFrames ||
            (m_state->lastPublished && a_frame <= *m_state->lastPublished) || a_snapshot.objects.size() > k_maxDraws ||
            (!a_snapshot.objects.empty() && !a_snapshot.camera))
            return Result<void>::failure({ErrorCategory::InvalidState, "SceneRenderer.publish_frame"});
        try
        {
            for (const auto &object : a_snapshot.objects)
            {
                if (!math::is_finite(object.world))
                    return Result<void>::failure({ErrorCategory::InvalidArgument, "SceneRenderer.object"});
                if (object.meshId != k_cubeMeshId && !m_state->meshAssets->contains(object.meshId))
                    return Result<void>::failure(
                        {ErrorCategory::InvalidArgument, "SceneRenderer.mesh_id:" + object.meshId});
                if (object.materialId && !m_state->materials->ids.contains(*object.materialId))
                    return Result<void>::failure(
                        {ErrorCategory::InvalidArgument, "SceneRenderer.material_id:" + *object.materialId});
            }
            m_state->pending.emplace(
                a_frame, SceneRendererState::PublishedFrame{std::move(a_snapshot), m_state->meshAssets, m_state->materials});
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
    /// @brief Resize 後にも新しい論理 Handle と Frame 資源を使う Pass 群を登録する
    Result<void> add_passes(FrameGraph &a_graph) override
    {
        const std::lock_guard lock(m_state->mutex);
        if (m_state->isStopped)
            return Result<void>::failure({ErrorCategory::InvalidState, "SceneRenderer.add_passes"});
        try
        {
            auto graph = std::make_shared<MeshGraph>();
            graph->scene = m_state;
            auto added = a_graph.add_pass(std::make_unique<MeshUploadPass>(graph));
            if (!added.has_value())
                return added;
            for (const auto &stage : k_computeStages)
            {
                added = a_graph.add_pass(std::make_unique<MeshComputePass>(graph, stage));
                if (!added.has_value())
                    return added;
            }
            return a_graph.add_pass(std::make_unique<MeshPass>(std::move(graph)));
        }
        catch (const std::bad_alloc &)
        {
            return Result<void>::failure({ErrorCategory::PlatformFailure, "SceneRenderer.pass_allocation"});
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
    /// @brief Graph の全 CPU / GPU 参照が消えた後で Mesh と Material の公開を止める
    Result<void> shutdown() override
    {
        const std::lock_guard lock(m_state->mutex);
        if (m_state->isStopped)
            return Result<void>::success();
        if (m_state->isRendering)
            return Result<void>::failure({ErrorCategory::InvalidState, "SceneRenderer.shutdown_rendering"});
        if (m_state->cube.is_valid())
        {
            auto retired = m_state->meshes->retire(m_state->cube);
            if (!retired.has_value())
                return retired;
            m_state->cube = {};
        }
        m_state->isStopped = true;
        m_state->pending.clear();
        m_state->meshAssets.reset();
        m_state->materials.reset();
        auto collected = m_state->meshes->collect();
        return collected.has_value() ? Result<void>::success() : Result<void>::failure(*collected.try_error());
    }

  private:
    /// @brief Owner の準備中も Render は前の世代を借用できる。成功後にだけ一括公開する
    Result<void> replace_assets(std::optional<std::span<const SceneMeshDesc>> a_meshes,
                                std::optional<std::span<const SceneMaterialDesc>> a_materials)
    {
        std::shared_ptr<const sceneMeshSet> meshes;
        std::shared_ptr<const dx12::SceneMaterialSet> materials;
        {
            const std::lock_guard lock(m_state->mutex);
            if (m_state->isStopped)
                return Result<void>::failure({ErrorCategory::InvalidState, "SceneRenderer.set_assets"});
            meshes = m_state->meshAssets;
            materials = m_state->materials;
        }
        if (a_meshes && (!a_meshes->empty() || !meshes->empty()))
        {
            auto created = prepare_meshes(*m_state->meshes, *meshes, *a_meshes);
            if (!created.has_value())
                return Result<void>::failure(*created.try_error());
            meshes = created.take_value();
        }
        if (a_materials && (!a_materials->empty() || !materials || !materials->ids.empty()))
        {
            auto created = dx12::create_scene_materials(*m_state->backend, *a_materials);
            if (!created.has_value())
                return Result<void>::failure(*created.try_error());
            materials = created.take_value();
        }
        const std::lock_guard lock(m_state->mutex);
        if (m_state->isStopped)
            return Result<void>::failure({ErrorCategory::InvalidState, "SceneRenderer.set_assets"});
        m_state->meshAssets = std::move(meshes);
        m_state->materials = std::move(materials);
        return Result<void>::success();
    }

    std::shared_ptr<SceneRendererState> m_state;
};
} // namespace

Result<std::unique_ptr<ISceneRenderer>> create_scene_renderer(IBackend &a_backend)
{
    using RendererResult = Result<std::unique_ptr<ISceneRenderer>>;
    auto *native = dynamic_cast<dx12::DX12Backend *>(&a_backend);
    if (!native || !native->get_resource_context() || !native->get_mesh_pool() ||
        !native->get_queue_pool() || !native->get_command_pool())
        return RendererResult::failure({ErrorCategory::InvalidArgument, "SceneRenderer.backend"});
    try
    {
        auto state = std::make_shared<SceneRendererState>();
        state->resources = native->get_resource_context();
        state->meshes = native->get_mesh_pool();
        state->backend = native;
        state->meshAssets = std::make_shared<const sceneMeshSet>();
        auto renderer = std::make_unique<SceneRenderer>(state);
        // 失敗や例外でも Renderer の停止処理が登録済み資源を回収する
        auto cube = create_cube_mesh();
        if (!cube.has_value())
            return RendererResult::failure(*cube.try_error());
        auto uploaded = state->meshes->upload(*cube.try_value());
        if (!uploaded.has_value())
            return RendererResult::failure(*uploaded.try_error());
        state->cube = uploaded.take_value();
        auto ready = state->meshes->wait_ready(state->cube);
        if (!ready.has_value())
            return RendererResult::failure(*ready.try_error());
        auto material = renderer->set_materials({});
        if (!material.has_value())
            return RendererResult::failure(*material.try_error());
        return RendererResult::success(std::move(renderer));
    }
    catch (const std::bad_alloc &)
    {
        return RendererResult::failure({ErrorCategory::PlatformFailure, "SceneRenderer.create_allocation"});
    }
}
} // namespace cue
