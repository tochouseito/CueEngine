#include <FrameGraph/FrameGraphBuilder.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <utility>
#include <vector>

namespace cue
{
namespace
{
std::atomic<std::uint64_t> g_nextGraphId = 1;
std::atomic<std::uint64_t> g_nextPlanId = 1;

/// @brief Texture の形状と列挙値を物理生成前に検証する
bool is_valid_texture(GpuTexture2DDesc a_desc) noexcept
{
    if (a_desc.width == 0 || a_desc.height == 0 || a_desc.mipLevels == 0)
    {
        return false;
    }
    switch (a_desc.format)
    {
    case GpuTextureFormat::Rgba8Unorm:
    case GpuTextureFormat::Bgra8Unorm:
    case GpuTextureFormat::Rgba16Float:
    case GpuTextureFormat::R32Float:
        return true;
    }
    return false;
}

/// @brief Resource 種類と Heap 用途に対して宣言可能な境界 State か判定する
bool is_valid_boundary_state(FrameGraphResourceState a_state, GpuResourceKind a_kind,
                             GpuMemoryUsage a_memory) noexcept
{
    if (a_memory == GpuMemoryUsage::Upload)
    {
        return a_kind == GpuResourceKind::Buffer && a_state == FrameGraphResourceState::GenericRead;
    }
    if (a_memory == GpuMemoryUsage::Readback)
    {
        return a_kind == GpuResourceKind::Buffer && a_state == FrameGraphResourceState::CopyDestination;
    }
    switch (a_state)
    {
    case FrameGraphResourceState::Common:
    case FrameGraphResourceState::CopySource:
    case FrameGraphResourceState::CopyDestination:
    case FrameGraphResourceState::ShaderRead:
    case FrameGraphResourceState::UnorderedAccess:
        return true;
    case FrameGraphResourceState::GenericRead:
        return a_kind == GpuResourceKind::Buffer;
    case FrameGraphResourceState::RenderTarget:
    case FrameGraphResourceState::DepthRead:
    case FrameGraphResourceState::DepthWrite:
    case FrameGraphResourceState::Present:
        return a_kind == GpuResourceKind::Texture2D;
    }
    return false;
}

/// @brief Pass の Access と State が同じ GPU 操作を表すか検証する
bool is_valid_use_state(FrameGraphResourceState a_state, FrameGraphAccess a_access) noexcept
{
    switch (a_state)
    {
    case FrameGraphResourceState::GenericRead:
    case FrameGraphResourceState::CopySource:
    case FrameGraphResourceState::ShaderRead:
    case FrameGraphResourceState::DepthRead:
        return a_access == FrameGraphAccess::Read;
    case FrameGraphResourceState::CopyDestination:
    case FrameGraphResourceState::RenderTarget:
    case FrameGraphResourceState::DepthWrite:
        return a_access == FrameGraphAccess::Write;
    case FrameGraphResourceState::UnorderedAccess:
        return a_access == FrameGraphAccess::Read || a_access == FrameGraphAccess::Write;
    case FrameGraphResourceState::Common:
    case FrameGraphResourceState::Present:
        return false;
    }
    return false;
}

/// @brief DX12 で同じ State 値となる Common と Present を同値として扱う
bool is_same_state(FrameGraphResourceState a_left, FrameGraphResourceState a_right) noexcept
{
    if (a_left == a_right)
    {
        return true;
    }
    return (a_left == FrameGraphResourceState::Common && a_right == FrameGraphResourceState::Present) ||
           (a_left == FrameGraphResourceState::Present && a_right == FrameGraphResourceState::Common);
}
} // namespace

/// @brief 検証済みの Pass と Resource の Snapshot を受け取る
FrameGraphPlan::FrameGraphPlan(std::vector<FrameGraphPassPlan> a_passes,
                               std::vector<FrameGraphResourcePlan> a_resources,
                               std::vector<FrameGraphAliasSlotPlan> a_aliasSlots,
                               std::vector<FrameGraphBarrierPlan> a_finalBarriers,
                               std::uint64_t a_id) noexcept
    : m_id(a_id), m_passes(std::move(a_passes)), m_resources(std::move(a_resources)),
      m_aliasSlots(std::move(a_aliasSlots)), m_finalBarriers(std::move(a_finalBarriers))
{
}

/// @brief Resource 実体化時と記録時の Plan 同一性を照合する
std::uint64_t FrameGraphPlan::id() const noexcept
{
    return m_id;
}

/// @brief 物理実行へ渡す Pass の順序を返す
const std::vector<FrameGraphPassPlan>& FrameGraphPlan::passes() const noexcept
{
    return m_passes;
}

/// @brief Alias 計画に渡す Resource の寿命を返す
const std::vector<FrameGraphResourcePlan>& FrameGraphPlan::resources() const noexcept
{
    return m_resources;
}

/// @brief 直列実行で同じ物理領域へ置ける論理 Resource 群を返す
const std::vector<FrameGraphAliasSlotPlan>& FrameGraphPlan::alias_slots() const noexcept
{
    return m_aliasSlots;
}

/// @brief Present や呼出側指定 State に戻すための最後の Barrier を返す
const std::vector<FrameGraphBarrierPlan>& FrameGraphPlan::final_barriers() const noexcept
{
    return m_finalBarriers;
}

/// @brief create の内部でのみ空の Builder を構築する
FrameGraphBuilder::FrameGraphBuilder(CreateToken) noexcept
{
}

/// @brief 他の Builder で発行した Handle を混同しない ID を確保する
Result<std::unique_ptr<FrameGraphBuilder>> FrameGraphBuilder::create()
{
    using BuilderResult = Result<std::unique_ptr<FrameGraphBuilder>>;
    std::uint64_t id = g_nextGraphId.load(std::memory_order_relaxed);
    while (id != (std::numeric_limits<std::uint64_t>::max)() &&
           !g_nextGraphId.compare_exchange_weak(id, id + 1, std::memory_order_relaxed))
    {
    }
    if (id == (std::numeric_limits<std::uint64_t>::max)())
    {
        return BuilderResult::failure({ErrorCategory::Fatal, "FrameGraphBuilder.id_exhausted"});
    }
    try
    {
        auto builder = std::make_unique<FrameGraphBuilder>(CreateToken{});
        builder->m_graphId = id;
        return BuilderResult::success(std::move(builder));
    }
    catch (const std::bad_alloc&)
    {
        return BuilderResult::failure({ErrorCategory::PlatformFailure, "FrameGraphBuilder.create.allocation"});
    }
}

/// @brief RenderTarget と ShaderRead の両用途を持つ論理 Texture を先頭へ固定する
Result<std::unique_ptr<FrameGraphBuilder>> FrameGraphBuilder::create_main(GpuTexture2DDesc a_finalColor)
{
    auto builderResult = create();
    if (!builderResult.has_value())
    {
        return builderResult;
    }
    auto builder = builderResult.take_value();
    a_finalColor.isRenderTarget = true;
    a_finalColor.isShaderReadable = true;
    auto colorResult = builder->create_transient_texture2d("FinalColorTexture", a_finalColor);
    if (!colorResult.has_value())
    {
        return Result<std::unique_ptr<FrameGraphBuilder>>::failure(*colorResult.try_error());
    }
    builder->m_finalColor = colorResult.take_value();
    // 枠をまたいで使う物理 Texture は次回の開始 State へ戻す
    builder->m_resources[builder->m_finalColor.index].restoreFinalState = true;
    return Result<std::unique_ptr<FrameGraphBuilder>>::success(std::move(builder));
}

/// @brief 通常 Builder では無効 Handle、本番用 Builder では固定 Handle を返す
FrameGraphResourceHandle FrameGraphBuilder::final_color() const noexcept
{
    return m_finalColor;
}

/// @brief Default Buffer の論理 Resource を追加する
Result<FrameGraphResourceHandle> FrameGraphBuilder::create_transient_buffer(GpuBufferDesc a_desc)
{
    return create_transient_buffer({}, a_desc);
}

/// @brief 名前付きの Default Buffer を登録する
Result<FrameGraphResourceHandle> FrameGraphBuilder::create_transient_buffer(std::string a_name,
                                                                            GpuBufferDesc a_desc)
{
    if (a_desc.byteSize == 0 || a_desc.memory != GpuMemoryUsage::Default)
    {
        return Result<FrameGraphResourceHandle>::failure(
            {ErrorCategory::InvalidArgument, "FrameGraphBuilder.create_transient_buffer.desc"});
    }
    FrameGraphResourcePlan resource{};
    resource.name = std::move(a_name);
    resource.kind = GpuResourceKind::Buffer;
    resource.bufferDesc = a_desc;
    return add_resource(std::move(resource));
}

/// @brief Texture の論理 Resource を追加する
Result<FrameGraphResourceHandle> FrameGraphBuilder::create_transient_texture2d(GpuTexture2DDesc a_desc)
{
    return create_transient_texture2d({}, a_desc);
}

/// @brief 名前付きの二次元 Texture を登録する
Result<FrameGraphResourceHandle> FrameGraphBuilder::create_transient_texture2d(std::string a_name,
                                                                               GpuTexture2DDesc a_desc)
{
    if (!is_valid_texture(a_desc))
    {
        return Result<FrameGraphResourceHandle>::failure(
            {ErrorCategory::InvalidArgument, "FrameGraphBuilder.create_transient_texture2d.desc"});
    }
    FrameGraphResourcePlan resource{};
    resource.name = std::move(a_name);
    resource.kind = GpuResourceKind::Texture2D;
    resource.textureDesc = a_desc;
    return add_resource(std::move(resource));
}

/// @brief Pool 所有などの外部 Buffer の論理形状を登録する
Result<FrameGraphResourceHandle> FrameGraphBuilder::import_buffer(GpuBufferDesc a_desc,
                                                                 FrameGraphResourceState a_initial,
                                                                 FrameGraphResourceState a_final)
{
    return import_buffer({}, a_desc, a_initial, a_final);
}

/// @brief 名前付きの外部 Buffer を取り込む
Result<FrameGraphResourceHandle> FrameGraphBuilder::import_buffer(std::string a_name, GpuBufferDesc a_desc,
                                                                  FrameGraphResourceState a_initial,
                                                                  FrameGraphResourceState a_final)
{
    if (a_desc.byteSize == 0 ||
        (a_desc.memory != GpuMemoryUsage::Default && a_desc.memory != GpuMemoryUsage::Upload &&
         a_desc.memory != GpuMemoryUsage::Readback) ||
        !is_valid_boundary_state(a_initial, GpuResourceKind::Buffer, a_desc.memory) ||
        !is_valid_boundary_state(a_final, GpuResourceKind::Buffer, a_desc.memory))
    {
        return Result<FrameGraphResourceHandle>::failure(
            {ErrorCategory::InvalidArgument, "FrameGraphBuilder.import_buffer.desc"});
    }
    FrameGraphResourcePlan resource{};
    resource.name = std::move(a_name);
    resource.kind = GpuResourceKind::Buffer;
    resource.bufferDesc = a_desc;
    resource.isImported = true;
    resource.initialState = a_initial;
    resource.finalState = a_final;
    return add_resource(std::move(resource));
}

/// @brief SwapChain Back Buffer などの外部 Texture を論理的に登録する
Result<FrameGraphResourceHandle> FrameGraphBuilder::import_texture2d(GpuTexture2DDesc a_desc,
                                                                    FrameGraphResourceState a_initial,
                                                                    FrameGraphResourceState a_final)
{
    return import_texture2d({}, a_desc, a_initial, a_final);
}

/// @brief 名前付きの外部 Texture を取り込む
Result<FrameGraphResourceHandle> FrameGraphBuilder::import_texture2d(std::string a_name,
                                                                     GpuTexture2DDesc a_desc,
                                                                     FrameGraphResourceState a_initial,
                                                                     FrameGraphResourceState a_final)
{
    if (!is_valid_texture(a_desc) ||
        !is_valid_boundary_state(a_initial, GpuResourceKind::Texture2D, GpuMemoryUsage::Default) ||
        !is_valid_boundary_state(a_final, GpuResourceKind::Texture2D, GpuMemoryUsage::Default))
    {
        return Result<FrameGraphResourceHandle>::failure(
            {ErrorCategory::InvalidArgument, "FrameGraphBuilder.import_texture2d.desc"});
    }
    FrameGraphResourcePlan resource{};
    resource.name = std::move(a_name);
    resource.kind = GpuResourceKind::Texture2D;
    resource.textureDesc = a_desc;
    resource.isImported = true;
    resource.initialState = a_initial;
    resource.finalState = a_final;
    return add_resource(std::move(resource));
}

/// @brief 物理 Buffer の所有は Pool に残し、Graph は非所有 Handle を保持する
Result<FrameGraphResourceHandle> FrameGraphBuilder::import_pool_buffer(
    std::string a_name, IGpuResourcePool& a_pool, GpuResourceHandle a_poolHandle,
    GpuBufferDesc a_desc, FrameGraphResourceState a_initial, FrameGraphResourceState a_final)
{
    if (!a_poolHandle.is_valid())
    {
        return Result<FrameGraphResourceHandle>::failure(
            {ErrorCategory::InvalidArgument, "FrameGraphBuilder.import_pool_buffer.handle"});
    }
    auto result = import_buffer(std::move(a_name), a_desc, a_initial, a_final);
    if (result.has_value())
    {
        auto& resource = m_resources[result.try_value()->index];
        resource.pool = &a_pool;
        resource.poolHandle = a_poolHandle;
    }
    return result;
}

/// @brief 物理 Texture の所有は Pool に残し、Graph は非所有 Handle を保持する
Result<FrameGraphResourceHandle> FrameGraphBuilder::import_pool_texture2d(
    std::string a_name, IGpuResourcePool& a_pool, GpuResourceHandle a_poolHandle,
    GpuTexture2DDesc a_desc, FrameGraphResourceState a_initial, FrameGraphResourceState a_final)
{
    if (!a_poolHandle.is_valid())
    {
        return Result<FrameGraphResourceHandle>::failure(
            {ErrorCategory::InvalidArgument, "FrameGraphBuilder.import_pool_texture2d.handle"});
    }
    auto result = import_texture2d(std::move(a_name), a_desc, a_initial, a_final);
    if (result.has_value())
    {
        auto& resource = m_resources[result.try_value()->index];
        resource.pool = &a_pool;
        resource.poolHandle = a_poolHandle;
    }
    return result;
}

/// @brief 名前と種類が一致する Texture の Handle を返す
Result<FrameGraphResourceHandle> FrameGraphBuilder::get_texture(std::string_view a_name) const
{
    for (const auto& resource : m_resources)
    {
        if (!a_name.empty() && resource.name == a_name && resource.kind == GpuResourceKind::Texture2D)
        {
            return Result<FrameGraphResourceHandle>::success(resource.handle);
        }
    }
    return Result<FrameGraphResourceHandle>::failure(
        {ErrorCategory::InvalidArgument, "FrameGraphBuilder.get_texture"});
}

/// @brief 名前と種類が一致する Buffer の Handle を返す
Result<FrameGraphResourceHandle> FrameGraphBuilder::get_buffer(std::string_view a_name) const
{
    for (const auto& resource : m_resources)
    {
        if (!a_name.empty() && resource.name == a_name && resource.kind == GpuResourceKind::Buffer)
        {
            return Result<FrameGraphResourceHandle>::success(resource.handle);
        }
    }
    return Result<FrameGraphResourceHandle>::failure(
        {ErrorCategory::InvalidArgument, "FrameGraphBuilder.get_buffer"});
}

/// @brief 診断名を確保して新しい Pass Handle を発行する
Result<FrameGraphPassHandle> FrameGraphBuilder::add_pass(std::string a_name, QueueType a_queue)
{
    if (a_name.empty() || m_passes.size() >= (std::numeric_limits<std::uint32_t>::max)() ||
        (a_queue != QueueType::Graphics && a_queue != QueueType::Compute && a_queue != QueueType::Copy))
    {
        return Result<FrameGraphPassHandle>::failure({ErrorCategory::InvalidArgument, "FrameGraphBuilder.add_pass"});
    }
    const FrameGraphPassHandle handle{m_graphId, static_cast<std::uint32_t>(m_passes.size())};
    try
    {
        FrameGraphPassPlan pass{};
        pass.handle = handle;
        pass.name = std::move(a_name);
        pass.queue = a_queue;
        m_passes.push_back(std::move(pass));
        return Result<FrameGraphPassHandle>::success(handle);
    }
    catch (const std::bad_alloc&)
    {
        return Result<FrameGraphPassHandle>::failure(
            {ErrorCategory::PlatformFailure, "FrameGraphBuilder.add_pass.allocation"});
    }
}

/// @brief 同じ Pass での重複 Access と別 Graph の Handle を拒否して使用を記録する
Result<void> FrameGraphBuilder::use(FrameGraphPassHandle a_pass, FrameGraphResourceHandle a_resource,
                                    FrameGraphAccess a_access, FrameGraphResourceState a_state)
{
    if (!owns(a_pass) || !owns(a_resource) ||
        !is_valid_use_state(a_state, a_access) ||
        !is_valid_boundary_state(a_state, m_resources[a_resource.index].kind,
                                 m_resources[a_resource.index].bufferDesc.memory) ||
        (m_passes[a_pass.index].queue == QueueType::Copy &&
         a_state != FrameGraphResourceState::CopySource &&
         a_state != FrameGraphResourceState::CopyDestination) ||
        (m_passes[a_pass.index].queue == QueueType::Compute &&
         (a_state == FrameGraphResourceState::RenderTarget ||
          a_state == FrameGraphResourceState::DepthRead ||
          a_state == FrameGraphResourceState::DepthWrite ||
          a_state == FrameGraphResourceState::Present)))
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "FrameGraphBuilder.use.handle"});
    }
    auto& uses = m_passes[a_pass.index].uses;
    if (std::any_of(uses.begin(), uses.end(), [a_resource](const FrameGraphUse& a_use) {
            return a_use.resource.index == a_resource.index;
        }))
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "FrameGraphBuilder.use.duplicate"});
    }
    try
    {
        uses.push_back({a_resource, a_access, a_state});
        return Result<void>::success();
    }
    catch (const std::bad_alloc&)
    {
        return Result<void>::failure({ErrorCategory::PlatformFailure, "FrameGraphBuilder.use.allocation"});
    }
}

/// @brief 旧 Builder と同様、現在構築中の Pass に Resource 使用を登録する
Result<void> FrameGraphBuilder::use(FrameGraphResourceHandle a_resource, FrameGraphAccess a_access,
                                    FrameGraphResourceState a_state)
{
    if (!m_currentPass)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "FrameGraphBuilder.use.no_pass"});
    }
    return use(*m_currentPass, a_resource, a_access, a_state);
}

/// @brief describe_resources の期間だけ現在の Pass を設定する
void FrameGraphBuilder::begin_pass(FrameGraphPassHandle a_pass) noexcept
{
    m_currentPass = a_pass;
}

/// @brief 別 Pass への Resource 宣言の漏出を防ぐ
void FrameGraphBuilder::end_pass() noexcept
{
    m_currentPass.reset();
}

/// @brief Hazard 以外の実行順制約を後から追加する
Result<void> FrameGraphBuilder::depends_on(FrameGraphPassHandle a_pass, FrameGraphPassHandle a_before)
{
    if (!owns(a_pass) || !owns(a_before) || a_pass.index == a_before.index)
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "FrameGraphBuilder.depends_on.handle"});
    }
    const std::pair dependency{a_before.index, a_pass.index};
    if (std::find(m_dependencies.begin(), m_dependencies.end(), dependency) != m_dependencies.end())
    {
        return Result<void>::success();
    }
    try
    {
        m_dependencies.push_back(dependency);
        return Result<void>::success();
    }
    catch (const std::bad_alloc&)
    {
        return Result<void>::failure({ErrorCategory::PlatformFailure, "FrameGraphBuilder.depends_on.allocation"});
    }
}

/// @brief 宣言順の Hazard と明示依存を整列し、Transient の初回 Write と寿命を確定する
Result<FrameGraphPlan> FrameGraphBuilder::build() const
{
    using PlanResult = Result<FrameGraphPlan>;
    try
    {
        const std::size_t passCount = m_passes.size();
        std::vector<std::vector<bool>> edges(passCount, std::vector<bool>(passCount, false));
        for (const auto& [before, after] : m_dependencies)
        {
            edges[before][after] = true;
        }

        // 同じ Resource の Write に接する Access は宣言順を守り、Read 同士だけ並べ替えを許す
        struct AccessRecord final
        {
            std::uint32_t passIndex = 0;
            FrameGraphAccess access = FrameGraphAccess::Read;
            FrameGraphResourceState state = FrameGraphResourceState::Common;
        };
        std::vector<std::vector<AccessRecord>> accesses(m_resources.size());
        for (const auto& pass : m_passes)
        {
            for (const auto& use : pass.uses)
            {
                accesses[use.resource.index].push_back({pass.handle.index, use.access, use.state});
            }
        }
        for (const auto& resourceAccesses : accesses)
        {
            for (std::size_t earlier = 0; earlier < resourceAccesses.size(); ++earlier)
            {
                for (std::size_t later = earlier + 1; later < resourceAccesses.size(); ++later)
                {
                    const auto& first = resourceAccesses[earlier];
                    const auto& second = resourceAccesses[later];
                    if (first.access == FrameGraphAccess::Write || second.access == FrameGraphAccess::Write ||
                        first.state != second.state ||
                        m_passes[first.passIndex].queue != m_passes[second.passIndex].queue)
                    {
                        edges[first.passIndex][second.passIndex] = true;
                    }
                }
            }
        }

        std::vector<std::size_t> indegree(passCount, 0);
        for (std::size_t before = 0; before < passCount; ++before)
        {
            for (std::size_t after = 0; after < passCount; ++after)
            {
                indegree[after] += edges[before][after] ? 1 : 0;
            }
        }
        std::vector<bool> emitted(passCount, false);
        std::vector<FrameGraphPassPlan> ordered;
        ordered.reserve(passCount);
        for (std::size_t order = 0; order < passCount; ++order)
        {
            std::size_t next = passCount;
            for (std::size_t index = 0; index < passCount; ++index)
            {
                if (!emitted[index] && indegree[index] == 0)
                {
                    next = index;
                    break;
                }
            }
            if (next == passCount)
            {
                return PlanResult::failure({ErrorCategory::InvalidState, "FrameGraphBuilder.build.cycle"});
            }
            emitted[next] = true;
            FrameGraphPassPlan pass = m_passes[next];
            for (std::size_t before = 0; before < passCount; ++before)
            {
                if (edges[before][next])
                {
                    pass.dependencies.push_back(m_passes[before].handle);
                }
                if (edges[next][before])
                {
                    --indegree[before];
                }
            }
            ordered.push_back(std::move(pass));
        }

        auto resources = m_resources;
        for (std::size_t order = 0; order < ordered.size(); ++order)
        {
            for (const auto& use : ordered[order].uses)
            {
                auto& resource = resources[use.resource.index];
                if (!resource.firstUse)
                {
                    if (!resource.isImported && use.access != FrameGraphAccess::Write)
                    {
                        return PlanResult::failure(
                            {ErrorCategory::InvalidState, "FrameGraphBuilder.build.uninitialized_read"});
                    }
                    resource.firstUse = order;
                }
                resource.lastUse = order;
            }
        }

        // Pass 開始前に State を合わせ、同じ UAV State の依存する Access だけを同期する
        std::vector<FrameGraphResourceState> currentStates;
        currentStates.reserve(resources.size());
        for (const auto& resource : resources)
        {
            currentStates.push_back(resource.initialState);
        }
        std::vector<std::optional<FrameGraphAccess>> previousAccess(resources.size());
        for (std::size_t order = 0; order < ordered.size(); ++order)
        {
            auto& pass = ordered[order];
            for (const auto& use : pass.uses)
            {
                const std::size_t index = use.resource.index;
                const auto before = currentStates[index];
                if (!is_same_state(before, use.state))
                {
                    pass.barriersBefore.push_back({FrameGraphBarrierKind::Transition, use.resource,
                                                   before, use.state});
                }
                else if (use.state == FrameGraphResourceState::UnorderedAccess && previousAccess[index] &&
                         (*previousAccess[index] == FrameGraphAccess::Write ||
                          use.access == FrameGraphAccess::Write))
                {
                    pass.barriersBefore.push_back({FrameGraphBarrierKind::UnorderedAccess, use.resource,
                                                   use.state, use.state});
                }
                currentStates[index] = use.state;
                previousAccess[index] = use.access;
            }
            // 最終使用直後に戻し、次の Alias Resource を有効化する前に State を確定する
            for (const auto& use : pass.uses)
            {
                const std::size_t index = use.resource.index;
                const auto& resource = resources[index];
                if (!resource.isImported && resource.lastUse == order &&
                    !is_same_state(currentStates[index], resource.initialState))
                {
                    pass.barriersAfter.push_back({FrameGraphBarrierKind::Transition, use.resource,
                                                  currentStates[index], resource.initialState});
                    currentStates[index] = resource.initialState;
                }
            }
        }
        std::vector<FrameGraphBarrierPlan> finalBarriers;
        for (std::size_t index = 0; index < resources.size(); ++index)
        {
            const auto& resource = resources[index];
            if ((resource.isImported || resource.restoreFinalState) &&
                !is_same_state(currentStates[index], resource.finalState))
            {
                finalBarriers.push_back({FrameGraphBarrierKind::Transition, resource.handle,
                                         currentStates[index], resource.finalState});
            }
        }

        // 初回使用順に処理し、最後の使用が次の初回使用より前の Slot だけを再利用する
        std::vector<std::uint32_t> transientIndices;
        for (std::uint32_t index = 0; index < resources.size(); ++index)
        {
            if (!resources[index].isImported && resources[index].firstUse)
            {
                transientIndices.push_back(index);
            }
        }
        std::stable_sort(transientIndices.begin(), transientIndices.end(), [&resources](std::uint32_t a_left,
                                                                                         std::uint32_t a_right) {
            return resources[a_left].firstUse < resources[a_right].firstUse;
        });
        std::vector<FrameGraphAliasSlotPlan> slots;
        for (std::uint32_t index : transientIndices)
        {
            const auto& resource = resources[index];
            auto available = std::find_if(slots.begin(), slots.end(), [&resources, &resource](const auto& a_slot) {
                const auto& last = resources[a_slot.resources.back().index];
                const bool isSameTextureHeap = resource.kind != GpuResourceKind::Texture2D ||
                                               last.textureDesc.isRenderTarget == resource.textureDesc.isRenderTarget;
                return a_slot.kind == resource.kind && isSameTextureHeap &&
                       *last.lastUse < *resource.firstUse;
            });
            if (available == slots.end())
            {
                slots.push_back({resource.kind, {resource.handle}});
            }
            else
            {
                available->resources.push_back(resource.handle);
            }
        }
        // Alias Slot の再利用は Resource Hazard がなくても GPU の実行順を要求する
        for (const auto& slot : slots)
        {
            for (std::size_t index = 1; index < slot.resources.size(); ++index)
            {
                const auto& previous = resources[slot.resources[index - 1].index];
                const auto& next = resources[slot.resources[index].index];
                auto& dependencies = ordered[*next.firstUse].dependencies;
                const auto before = ordered[*previous.lastUse].handle;
                if (std::none_of(dependencies.begin(), dependencies.end(), [before](FrameGraphPassHandle a_handle)
                                 { return a_handle.index == before.index; }))
                {
                    dependencies.push_back(before);
                }
            }
        }
        std::uint64_t planId = g_nextPlanId.load(std::memory_order_relaxed);
        while (planId != (std::numeric_limits<std::uint64_t>::max)() &&
               !g_nextPlanId.compare_exchange_weak(planId, planId + 1, std::memory_order_relaxed))
        {
        }
        if (planId == (std::numeric_limits<std::uint64_t>::max)())
        {
            return PlanResult::failure({ErrorCategory::Fatal, "FrameGraphBuilder.plan_id_exhausted"});
        }
        return PlanResult::success(FrameGraphPlan(std::move(ordered), std::move(resources),
                                                  std::move(slots), std::move(finalBarriers), planId));
    }
    catch (const std::bad_alloc&)
    {
        return PlanResult::failure({ErrorCategory::PlatformFailure, "FrameGraphBuilder.build.allocation"});
    }
}

/// @brief 宣言済み Pass 数を返す
std::size_t FrameGraphBuilder::pass_count() const noexcept
{
    return m_passes.size();
}

/// @brief 失敗時は既存 Resource Index を変えずに論理形状だけ追加する
Result<FrameGraphResourceHandle> FrameGraphBuilder::add_resource(FrameGraphResourcePlan a_resource)
{
    if (m_resources.size() >= (std::numeric_limits<std::uint32_t>::max)())
    {
        return Result<FrameGraphResourceHandle>::failure(
            {ErrorCategory::InvalidState, "FrameGraphBuilder.add_resource.exhausted"});
    }
    if (!a_resource.name.empty() && std::any_of(m_resources.begin(), m_resources.end(),
        [&a_resource](const FrameGraphResourcePlan& a_existing) { return a_existing.name == a_resource.name; }))
    {
        return Result<FrameGraphResourceHandle>::failure(
            {ErrorCategory::InvalidArgument, "FrameGraphBuilder.add_resource.duplicate_name"});
    }
    const FrameGraphResourceHandle handle{m_graphId, static_cast<std::uint32_t>(m_resources.size())};
    a_resource.handle = handle;
    try
    {
        m_resources.push_back(std::move(a_resource));
        return Result<FrameGraphResourceHandle>::success(handle);
    }
    catch (const std::bad_alloc&)
    {
        return Result<FrameGraphResourceHandle>::failure(
            {ErrorCategory::PlatformFailure, "FrameGraphBuilder.add_resource.allocation"});
    }
}

/// @brief Graph ID と Resource Index の両方を検証する
bool FrameGraphBuilder::owns(FrameGraphResourceHandle a_handle) const noexcept
{
    return a_handle.graphId == m_graphId && a_handle.graphId != 0 && a_handle.index < m_resources.size();
}

/// @brief Graph ID と Pass Index の両方を検証する
bool FrameGraphBuilder::owns(FrameGraphPassHandle a_handle) const noexcept
{
    return a_handle.graphId == m_graphId && a_handle.graphId != 0 && a_handle.index < m_passes.size();
}
} // namespace cue
