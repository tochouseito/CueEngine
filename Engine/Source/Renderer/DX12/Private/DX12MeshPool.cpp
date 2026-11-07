#include <DX12/DX12MeshPool.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <limits>
#include <mutex>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include <DX12/DX12CommandPool.h>
#include <DX12/DX12GpuResource.h>
#include <DX12/DX12QueuePool.h>
#include <DX12/DX12RenderDevice.h>
#include <Platform/Diagnostics.h>
#include <RHI/CommandCompletion.h>

namespace cue::dx12
{
namespace
{
struct Range final
{
    std::uint32_t first = 0;
    std::uint32_t count = 0;
};

/// @brief 空き範囲から要素数の予約候補を探す。公開成功までは変更しない
std::size_t find_range(const std::vector<Range> &a_ranges, std::uint32_t a_count)
{
    for (std::size_t index = 0; index < a_ranges.size(); ++index)
        if (a_ranges[index].count >= a_count)
            return index;
    return a_ranges.size();
}

/// @brief 転送成功後にだけ予約候補を消費する。追加の Allocation は行わない
void consume_range(std::vector<Range> &a_ranges, std::size_t a_index, std::uint32_t a_count) noexcept
{
    auto &range = a_ranges[a_index];
    range.first += a_count;
    range.count -= a_count;
    if (range.count == 0)
        a_ranges.erase(a_ranges.begin() + a_index);
}

/// @brief 事前に容量確保した空きリストへ戻し、隣接範囲を結合する
void return_range(std::vector<Range> &a_ranges, Range a_range)
{
    a_ranges.push_back(a_range);
    std::sort(a_ranges.begin(), a_ranges.end(),
              [](Range a_left, Range a_right) { return a_left.first < a_right.first; });
    for (std::size_t index = 1; index < a_ranges.size();)
    {
        auto &previous = a_ranges[index - 1];
        if (previous.first + previous.count == a_ranges[index].first)
        {
            previous.count += a_ranges[index].count;
            a_ranges.erase(a_ranges.begin() + index);
        }
        else
            ++index;
    }
}

/// @brief 完了済み Token だけを解放し、Device Lost を未完了として扱わない
Result<void> collect_completions(std::vector<std::shared_ptr<ICommandCompletion>> &a_completions)
{
    for (auto iterator = a_completions.begin(); iterator != a_completions.end();)
    {
        auto status = (*iterator)->completion_status();
        if (!status.has_value())
            return Result<void>::failure(*status.try_error());
        if (*status.try_value())
            iterator = a_completions.erase(iterator);
        else
            ++iterator;
    }
    return Result<void>::success();
}

/// @brief Pool ID はプロセス内で再利用せず、枯渇時は 0 を返す
std::uint64_t issue_pool_id() noexcept
{
    static std::atomic<std::uint64_t> next{1};
    auto value = next.load(std::memory_order_relaxed);
    while (value != (std::numeric_limits<std::uint64_t>::max)())
        if (next.compare_exchange_weak(value, value + 1, std::memory_order_relaxed))
            return value;
    return 0;
}

/// @brief Math の Memory Layout に依存せず、頂点属性を float32 の連続成分へ変換する
template <class V> std::vector<float> pack_vertices(std::span<const V> a_values)
{
    std::vector<float> packed;
    packed.reserve(a_values.size() * V::k_dimension);
    for (const auto &value : a_values)
        for (std::size_t component = 0; component < V::k_dimension; ++component)
            packed.push_back(value[component]);
    return packed;
}
} // namespace

struct DX12MeshPool::State final
{
    struct Upload final
    {
        std::array<std::unique_ptr<DX12GpuResource>, 4> staging;
        std::shared_ptr<SharedCommandCompletion> completion;
    };
    struct Page final
    {
        std::uint64_t id = 0;
        MeshIndexFormat format = MeshIndexFormat::Uint16;
        bool hasNormals = false;
        bool hasUv = false;
        std::uint32_t vertexCapacity = 0;
        std::uint32_t indexCapacity = 0;
        std::array<std::unique_ptr<DX12GpuResource>, 4> buffers;
        std::vector<Range> freeVertices;
        std::vector<Range> freeIndices;
        std::unique_ptr<Upload> upload;
        std::vector<std::shared_ptr<ICommandCompletion>> reads;
        std::size_t cpuLeases = 0;
    };
    struct Slot final
    {
        std::shared_ptr<Page> page;
        Range vertices;
        Range indices;
        MeshDrawRange draw;
        std::shared_ptr<ICommandCompletion> upload;
        std::vector<std::shared_ptr<ICommandCompletion>> reads;
        std::size_t cpuLeases = 0;
        bool isRetired = false;
    };
    struct Record final
    {
        std::uint64_t generation = 1;
        std::shared_ptr<Slot> slot;
    };

    std::mutex mutex;
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    ICommandPool *commands = nullptr;
    IQueuePool *queues = nullptr;
    MeshPoolConfig config;
    std::uint64_t poolId = 0;
    std::uint64_t nextPageId = 1;
    bool isStopped = false;
    std::vector<Record> records;
    std::vector<std::shared_ptr<Page>> pages;

    /// @brief 最後の Lease が返った後にも全提出の完了を確認してから Buffer を破棄する
    ~State()
    {
        auto result = wait_gpu();
        if (!result.has_value())
            report_log_error("DX12MeshPool.destroy", *result.try_error(), LogLevel::Error);
    }

    /// @brief 全 Upload / Read Token を待つ。CPU Lease の解放待機は行わない
    Result<void> wait_gpu()
    {
        auto first = Result<void>::success();
        for (const auto &page : pages)
        {
            if (page->upload)
            {
                auto result = page->upload->completion->wait();
                if (!result.has_value() && first.has_value())
                    first = std::move(result);
            }
            for (const auto &read : page->reads)
            {
                auto result = read->wait();
                if (!result.has_value() && first.has_value())
                    first = std::move(result);
            }
        }
        return first;
    }

    /// @brief 所属・世代・退役・停止を検証し、生存中の Slot だけを返す
    std::shared_ptr<Slot> resolve(MeshHandle a_handle) const
    {
        if (isStopped || a_handle.poolId != poolId || a_handle.index >= records.size())
            return {};
        const auto &record = records[a_handle.index];
        return record.generation == a_handle.generation && record.slot && !record.slot->isRetired ? record.slot
                                                                                                  : nullptr;
    }

    /// @brief Page の転送・描画完了を確認し、不要になった一時 Upload を解放する
    Result<void> poll_pages()
    {
        for (const auto &page : pages)
        {
            if (page->upload)
            {
                auto ready = page->upload->completion->completion_status();
                if (!ready.has_value())
                    return Result<void>::failure(*ready.try_error());
                if (*ready.try_value())
                    page->upload.reset();
            }
            auto result = collect_completions(page->reads);
            if (!result.has_value())
                return result;
        }
        // 生存中 Mesh の毎 Frame の提出も回収し、退役するまで Token が増え続けることを防ぐ
        for (const auto &record : records)
        {
            if (!record.slot)
                continue;
            auto result = collect_completions(record.slot->reads);
            if (!result.has_value())
                return result;
        }
        return Result<void>::success();
    }

    /// @brief 回収条件が揃った退役 Slot の領域を返し、世代を進める
    Result<std::size_t> collect_slots()
    {
        auto pagesResult = poll_pages();
        if (!pagesResult.has_value())
            return Result<std::size_t>::failure(*pagesResult.try_error());
        std::size_t collected = 0;
        for (auto &record : records)
        {
            auto &slot = record.slot;
            if (!slot || !slot->isRetired || slot->cpuLeases != 0)
                continue;
            auto uploaded = slot->upload->completion_status();
            if (!uploaded.has_value())
                return Result<std::size_t>::failure(*uploaded.try_error());
            auto reads = collect_completions(slot->reads);
            if (!reads.has_value())
                return Result<std::size_t>::failure(*reads.try_error());
            if (!*uploaded.try_value() || !slot->reads.empty())
                continue;
            // 両方のリスト容量を確保してから変更し、Allocation 失敗時にも二重返却を防ぐ
            auto &page = *slot->page;
            page.freeVertices.reserve(page.freeVertices.size() + 1);
            page.freeIndices.reserve(page.freeIndices.size() + 1);
            return_range(page.freeVertices, slot->vertices);
            return_range(page.freeIndices, slot->indices);
            slot.reset();
            record.generation =
                record.generation == (std::numeric_limits<std::uint64_t>::max)() ? 0 : record.generation + 1;
            ++collected;
        }
        return Result<std::size_t>::success(collected);
    }

    /// @brief 新しい属性構成の Committed Page を部分公開せず生成する
    Result<std::shared_ptr<Page>> create_page(const MeshData &a_mesh)
    {
        auto page = std::make_shared<Page>();
        page->id = nextPageId;
        page->format = a_mesh.index_format();
        page->hasNormals = !a_mesh.normals().empty();
        page->hasUv = !a_mesh.uv0().empty();
        page->vertexCapacity =
            (std::max)(config.verticesPerPage, static_cast<std::uint32_t>(a_mesh.positions().size()));
        page->indexCapacity = (std::max)(config.indicesPerPage, a_mesh.index_count());
        const std::uint32_t indexStride = page->format == MeshIndexFormat::Uint16 ? 2 : 4;
        constexpr auto k_maxViewBytes = (std::numeric_limits<std::uint32_t>::max)();
        if (page->vertexCapacity > k_maxViewBytes / 12 || page->indexCapacity > k_maxViewBytes / indexStride ||
            nextPageId == (std::numeric_limits<std::uint64_t>::max)())
            return Result<std::shared_ptr<Page>>::failure(
                {ErrorCategory::InvalidArgument, "DX12MeshPool.page.capacity"});
        const std::array<std::uint64_t, 4> sizes = {std::uint64_t(page->vertexCapacity) * 12,
                                                    page->hasNormals ? std::uint64_t(page->vertexCapacity) * 12 : 0,
                                                    page->hasUv ? std::uint64_t(page->vertexCapacity) * 8 : 0,
                                                    std::uint64_t(page->indexCapacity) * indexStride};
        constexpr std::array names = {L" Position", L" Normal", L" UV0", L" Index"};
        for (std::size_t index = 0; index < sizes.size(); ++index)
        {
            if (sizes[index] == 0)
                continue;
            auto buffer =
                DX12GpuResource::create_buffer(*device.Get(), {sizes[index], GpuMemoryUsage::Default},
                                               L"CueEngine Mesh Page " + std::to_wstring(page->id) + names[index]);
            if (!buffer.has_value())
                return Result<std::shared_ptr<Page>>::failure(*buffer.try_error());
            page->buffers[index] = buffer.take_value();
        }
        page->freeVertices.push_back({0, page->vertexCapacity});
        page->freeIndices.push_back({0, page->indexCapacity});
        return Result<std::shared_ptr<Page>>::success(std::move(page));
    }
};

struct DX12MeshPool::Lease final : IMeshLease
{
    std::shared_ptr<State> state;
    std::shared_ptr<State::Slot> slot;
    bool hasSubmitted = false;

    /// @brief 生成中は Pin せず、Factory が成功を確定した後に参照数を増やす
    Lease(std::shared_ptr<State> a_state, std::shared_ptr<State::Slot> a_slot)
        : state(std::move(a_state)), slot(std::move(a_slot))
    {
    }
    /// @brief CPU Pin を返す。State の最終解放は Mutex を離してから行う
    ~Lease() override
    {
        const std::lock_guard lock(state->mutex);
        --slot->cpuLeases;
        --slot->page->cpuLeases;
    }
    /// @brief Pin 中に不変の Page View を返す
    const MeshDrawRange &draw_range() const noexcept override
    {
        return slot->draw;
    }
    /// @brief 退役済みでも停止前なら最終提出を登録し、Page 全体の更新も抑止する
    Result<void> mark_submitted(std::shared_ptr<ICommandCompletion> a_completion) override
    {
        const std::lock_guard lock(state->mutex);
        if (!a_completion || hasSubmitted || state->isStopped)
            return Result<void>::failure({ErrorCategory::InvalidState, "DX12MeshPool.lease.submit"});
        try
        {
            // 提出後の確保失敗でも Lease を維持して再試行できる
            slot->reads.reserve(slot->reads.size() + 1);
            slot->page->reads.reserve(slot->page->reads.size() + 1);
            slot->reads.push_back(a_completion);
            slot->page->reads.push_back(std::move(a_completion));
            hasSubmitted = true;
            return Result<void>::success();
        }
        catch (const std::bad_alloc &)
        {
            return Result<void>::failure({ErrorCategory::PlatformFailure, "DX12MeshPool.lease.allocation"});
        }
    }
};

DX12MeshPool::DX12MeshPool(CreateToken) noexcept
{
}

Result<std::unique_ptr<DX12MeshPool>> DX12MeshPool::create(DX12RenderDevice &a_device, ICommandPool &a_commands,
                                                           IQueuePool &a_queues, const MeshPoolConfig &a_config)
{
    using PoolResult = Result<std::unique_ptr<DX12MeshPool>>;
    constexpr auto k_maxViewBytes = (std::numeric_limits<std::uint32_t>::max)();
    if (!a_config.verticesPerPage || !a_config.indicesPerPage || !a_config.maxMeshes || !a_config.maxPages ||
        a_config.verticesPerPage > k_maxViewBytes / 12 || a_config.indicesPerPage > k_maxViewBytes / 4)
        return PoolResult::failure({ErrorCategory::InvalidArgument, "DX12MeshPool.create.config"});
    try
    {
        auto pool = std::make_unique<DX12MeshPool>(CreateToken{});
        pool->m_state = std::make_shared<State>();
        auto &state = *pool->m_state;
        state.device = a_device.device();
        state.commands = &a_commands;
        state.queues = &a_queues;
        state.config = a_config;
        state.poolId = issue_pool_id();
        if (state.poolId == 0)
            return PoolResult::failure({ErrorCategory::Fatal, "DX12MeshPool.create.identity_exhausted"});
        state.records.resize(a_config.maxMeshes);
        state.pages.reserve(a_config.maxPages);
        return PoolResult::success(std::move(pool));
    }
    catch (const std::bad_alloc &)
    {
        return PoolResult::failure({ErrorCategory::PlatformFailure, "DX12MeshPool.create.allocation"});
    }
}

DX12MeshPool::~DX12MeshPool()
{
    if (m_state)
    {
        auto result = shutdown();
        if (!result.has_value())
            report_log_error("DX12MeshPool.shutdown", *result.try_error(), LogLevel::Error);
    }
}

Result<MeshHandle> DX12MeshPool::upload(const MeshData &a_mesh)
{
    auto &state = *m_state;
    const std::lock_guard lock(state.mutex);
    if (state.isStopped)
        return Result<MeshHandle>::failure({ErrorCategory::InvalidState, "DX12MeshPool.upload.stopped"});
    // 移動元 MeshData の誤利用と Native View の表現上限を記録前に拒否する
    if (a_mesh.positions().empty() || a_mesh.index_count() == 0 ||
        a_mesh.positions().size() > (std::numeric_limits<std::uint32_t>::max)() / 12)
        return Result<MeshHandle>::failure({ErrorCategory::InvalidArgument, "DX12MeshPool.upload.mesh"});
    try
    {
        auto collected = state.collect_slots();
        if (!collected.has_value())
            return Result<MeshHandle>::failure(*collected.try_error());
        const auto recordIterator = std::find_if(state.records.begin(), state.records.end(), [](const auto &a_record)
                                                 { return !a_record.slot && a_record.generation != 0; });
        if (recordIterator == state.records.end())
            return Result<MeshHandle>::failure({ErrorCategory::InvalidState, "DX12MeshPool.upload.mesh_capacity"});

        const auto vertexCount = static_cast<std::uint32_t>(a_mesh.positions().size());
        const auto indexCount = a_mesh.index_count();
        std::shared_ptr<State::Page> page;
        for (const auto &candidate : state.pages)
        {
            // Buffer の State は Page 全体なので、別範囲への転送でも既存の Read と競合する
            if (!candidate->upload && candidate->reads.empty() && candidate->cpuLeases == 0 &&
                candidate->format == a_mesh.index_format() && candidate->hasNormals == !a_mesh.normals().empty() &&
                candidate->hasUv == !a_mesh.uv0().empty() &&
                find_range(candidate->freeVertices, vertexCount) < candidate->freeVertices.size() &&
                find_range(candidate->freeIndices, indexCount) < candidate->freeIndices.size())
            {
                page = candidate;
                break;
            }
        }
        bool isNewPage = false;
        if (!page)
        {
            if (state.pages.size() == state.config.maxPages)
            {
                // 全 Mesh が回収済みの Page だけを置き換え、異なる Layout でも容量を再利用する
                const auto empty =
                    std::find_if(state.pages.begin(), state.pages.end(), [](const auto &a_page)
                                 { return a_page.use_count() == 1 && !a_page->upload && a_page->reads.empty(); });
                if (empty == state.pages.end())
                    return Result<MeshHandle>::failure(
                        {ErrorCategory::InvalidState, "DX12MeshPool.upload.page_capacity"});
                state.pages.erase(empty);
            }
            auto created = state.create_page(a_mesh);
            if (!created.has_value())
                return Result<MeshHandle>::failure(*created.try_error());
            page = created.take_value();
            isNewPage = true;
        }
        const auto vertexRange = find_range(page->freeVertices, vertexCount);
        const auto indexRange = find_range(page->freeIndices, indexCount);
        auto slot = std::make_shared<State::Slot>();
        slot->page = page;
        slot->vertices = {page->freeVertices[vertexRange].first, vertexCount};
        slot->indices = {page->freeIndices[indexRange].first, indexCount};
        slot->draw.pageId = page->id;
        slot->draw.indexFormat = page->format;
        slot->draw.indexCount = indexCount;
        slot->draw.firstIndex = slot->indices.first;
        slot->draw.baseVertex = static_cast<std::int32_t>(slot->vertices.first);
        constexpr std::array<std::uint32_t, 3> strides = {12, 12, 8};
        for (std::size_t index = 0; index < strides.size(); ++index)
            if (page->buffers[index])
                slot->draw.vertices[index] = {page->buffers[index].get(),
                                              static_cast<std::uint32_t>(page->buffers[index]->buffer_size()),
                                              strides[index]};
        const std::uint32_t indexStride = page->format == MeshIndexFormat::Uint16 ? 2 : 4;
        slot->draw.indices = {page->buffers[3].get(), static_cast<std::uint32_t>(page->buffers[3]->buffer_size()),
                              indexStride};

        // CPU 元 Data を GPU の転送形式へ詰め、全一時資源と Token を提出前に所有する
        const auto positions = pack_vertices(a_mesh.positions());
        const auto normals = pack_vertices(a_mesh.normals());
        const auto uv = pack_vertices(a_mesh.uv0());
        const std::array<std::span<const std::byte>, 4> source = {
            std::as_bytes(std::span{positions}), std::as_bytes(std::span{normals}), std::as_bytes(std::span{uv}),
            page->format == MeshIndexFormat::Uint16 ? std::as_bytes(a_mesh.indices16())
                                                    : std::as_bytes(a_mesh.indices32())};
        auto job = std::make_unique<State::Upload>();
        job->completion = std::make_shared<SharedCommandCompletion>();
        slot->upload = job->completion;
        for (std::size_t index = 0; index < source.size(); ++index)
        {
            if (source[index].empty())
                continue;
            auto staging = DX12GpuResource::create_buffer(
                *state.device.Get(), {source[index].size(), GpuMemoryUsage::Upload},
                L"CueEngine Mesh Upload " + std::to_wstring(page->id) + L" Stream " + std::to_wstring(index));
            if (!staging.has_value())
                return Result<MeshHandle>::failure(*staging.try_error());
            job->staging[index] = staging.take_value();
            auto written = job->staging[index]->write(0, source[index]);
            if (!written.has_value())
                return Result<MeshHandle>::failure(*written.try_error());
        }
        // SwapChain の Graphics Lease を奪わず、独立した Copy Queue に提出する
        auto queueResult = state.queues->acquire(QueueType::Copy);
        if (!queueResult.has_value())
            return Result<MeshHandle>::failure(*queueResult.try_error());
        auto queue = queueResult.take_value();
        auto *nativeQueue = dynamic_cast<DX12GpuCommandQueue *>(queue.get());
        if (!nativeQueue || nativeQueue->device() != state.device.Get())
            return Result<MeshHandle>::failure({ErrorCategory::InvalidArgument, "DX12MeshPool.upload.queue_device"});
        auto commandResult = state.commands->acquire(QueueType::Copy);
        if (!commandResult.has_value())
            return Result<MeshHandle>::failure(*commandResult.try_error());
        auto command = commandResult.take_value();
        auto *native = dynamic_cast<DX12GpuCommandContext *>(command.get());
        if (!native)
            return Result<MeshHandle>::failure({ErrorCategory::InvalidArgument, "DX12MeshPool.upload.command_backend"});
        Microsoft::WRL::ComPtr<ID3D12Device> commandDevice;
        // 別 Backend の Pool の混用を Native 記録より前に拒否する
        if (FAILED(native->command_list()->GetDevice(IID_PPV_ARGS(&commandDevice))) ||
            commandDevice.Get() != state.device.Get())
            return Result<MeshHandle>::failure({ErrorCategory::InvalidArgument, "DX12MeshPool.upload.command_device"});
        const std::array<std::uint64_t, 4> offsets = {
            std::uint64_t(slot->vertices.first) * 12, std::uint64_t(slot->vertices.first) * 12,
            std::uint64_t(slot->vertices.first) * 8, std::uint64_t(slot->indices.first) * indexStride};
        for (std::size_t index = 0; index < source.size(); ++index)
        {
            if (source[index].empty())
                continue;
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = page->buffers[index]->resource();
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            native->command_list()->ResourceBarrier(1, &barrier);
            native->command_list()->CopyBufferRegion(page->buffers[index]->resource(), offsets[index],
                                                     job->staging[index]->resource(), 0, source[index].size());
            std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
            native->command_list()->ResourceBarrier(1, &barrier);
        }
        auto closed = command->close();
        if (!closed.has_value())
            return Result<MeshHandle>::failure(*closed.try_error());
        auto submitted = state.commands->submit(*queue, *command);
        if (!submitted.has_value())
            return Result<MeshHandle>::failure(*submitted.try_error());

        // ここから先は Allocation しない。転送資源を保持してから Handle を公開する
        job->completion->set(submitted.take_value());
        page->upload = std::move(job);
        consume_range(page->freeVertices, vertexRange, vertexCount);
        consume_range(page->freeIndices, indexRange, indexCount);
        if (isNewPage)
        {
            state.pages.push_back(page);
            ++state.nextPageId;
        }
        recordIterator->slot = std::move(slot);
        return Result<MeshHandle>::success({static_cast<std::uint32_t>(recordIterator - state.records.begin()),
                                            recordIterator->generation, state.poolId});
    }
    catch (const std::bad_alloc &)
    {
        return Result<MeshHandle>::failure({ErrorCategory::PlatformFailure, "DX12MeshPool.upload.allocation"});
    }
}

Result<MeshStatus> DX12MeshPool::status(MeshHandle a_handle)
{
    const std::lock_guard lock(m_state->mutex);
    auto slot = m_state->resolve(a_handle);
    if (!slot)
        return Result<MeshStatus>::failure({ErrorCategory::InvalidArgument, "DX12MeshPool.status.handle"});
    auto completed = slot->upload->completion_status();
    if (!completed.has_value())
        return Result<MeshStatus>::failure(*completed.try_error());
    if (!*completed.try_value())
        return Result<MeshStatus>::success(MeshStatus::Uploading);
    // 同 Page の別範囲への転送中も、新規描画を開始できる状態とは扱わない
    if (slot->page->upload)
    {
        auto pageReady = slot->page->upload->completion->completion_status();
        if (!pageReady.has_value())
            return Result<MeshStatus>::failure(*pageReady.try_error());
        if (!*pageReady.try_value())
            return Result<MeshStatus>::success(MeshStatus::Uploading);
    }
    return Result<MeshStatus>::success(MeshStatus::Ready);
}

Result<void> DX12MeshPool::wait_ready(MeshHandle a_handle)
{
    const std::lock_guard lock(m_state->mutex);
    auto slot = m_state->resolve(a_handle);
    if (!slot)
        return Result<void>::failure({ErrorCategory::InvalidArgument, "DX12MeshPool.wait_ready.handle"});
    auto waited = slot->upload->wait();
    if (!waited.has_value())
        return waited;
    return slot->page->upload ? slot->page->upload->completion->wait() : Result<void>::success();
}

Result<meshLease> DX12MeshPool::acquire(MeshHandle a_handle)
{
    const std::lock_guard lock(m_state->mutex);
    auto slot = m_state->resolve(a_handle);
    if (!slot)
        return Result<meshLease>::failure({ErrorCategory::InvalidArgument, "DX12MeshPool.acquire.handle"});
    auto completed = slot->upload->completion_status();
    if (!completed.has_value())
        return Result<meshLease>::failure(*completed.try_error());
    if (!*completed.try_value())
        return Result<meshLease>::failure({ErrorCategory::InvalidState, "DX12MeshPool.acquire.uploading"});
    // 同 Page への別 Mesh の転送も全 Buffer の State を変えるため、完了まで新規 Read を禁止する
    if (slot->page->upload)
    {
        auto pageReady = slot->page->upload->completion->completion_status();
        if (!pageReady.has_value())
            return Result<meshLease>::failure(*pageReady.try_error());
        if (!*pageReady.try_value())
            return Result<meshLease>::failure({ErrorCategory::InvalidState, "DX12MeshPool.acquire.page_uploading"});
    }
    try
    {
        auto lease = std::make_unique<Lease>(m_state, slot);
        ++slot->cpuLeases;
        ++slot->page->cpuLeases;
        return Result<meshLease>::success(std::move(lease));
    }
    catch (const std::bad_alloc &)
    {
        return Result<meshLease>::failure({ErrorCategory::PlatformFailure, "DX12MeshPool.acquire.allocation"});
    }
}

Result<void> DX12MeshPool::retire(MeshHandle a_handle)
{
    const std::lock_guard lock(m_state->mutex);
    auto slot = m_state->resolve(a_handle);
    if (!slot)
        return Result<void>::failure({ErrorCategory::InvalidArgument, "DX12MeshPool.retire.handle"});
    slot->isRetired = true;
    return Result<void>::success();
}

Result<std::size_t> DX12MeshPool::collect()
{
    const std::lock_guard lock(m_state->mutex);
    if (m_state->isStopped)
        return Result<std::size_t>::failure({ErrorCategory::InvalidState, "DX12MeshPool.collect.stopped"});
    try
    {
        return m_state->collect_slots();
    }
    catch (const std::bad_alloc &)
    {
        return Result<std::size_t>::failure({ErrorCategory::PlatformFailure, "DX12MeshPool.collect.allocation"});
    }
}

Result<void> DX12MeshPool::shutdown()
{
    const std::lock_guard lock(m_state->mutex);
    m_state->isStopped = true;
    for (const auto &page : m_state->pages)
        if (page->cpuLeases != 0)
            return Result<void>::failure({ErrorCategory::InvalidState, "DX12MeshPool.shutdown.leases"});
    auto waited = m_state->wait_gpu();
    if (!waited.has_value())
        return waited;
    m_state->records.clear();
    m_state->pages.clear();
    return Result<void>::success();
}
} // namespace cue::dx12
