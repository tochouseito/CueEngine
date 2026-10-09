#include <DX12/DX12PlacedResourceAllocator.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <span>
#include <utility>
#include <vector>

#include <wrl/client.h>

#include <DX12/DX12GpuResource.h>
#include <DX12/DX12RenderDevice.h>
#include <Platform/Diagnostics.h>

namespace cue::dx12
{
namespace
{
constexpr std::uint64_t k_heapAlignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;

/// @brief Overflow を検証しながら値を Alignment へ切り上げる
bool align_up(std::uint64_t a_value, std::uint64_t a_alignment, std::uint64_t& a_result) noexcept
{
    if (a_alignment == 0 || (a_alignment & (a_alignment - 1)) != 0 ||
        a_value > (std::numeric_limits<std::uint64_t>::max)() - (a_alignment - 1))
    {
        return false;
    }
    a_result = (a_value + a_alignment - 1) & ~(a_alignment - 1);
    return true;
}

/// @brief Native HRESULT と操作名を共通 Error に変換する
Error placed_error(const char* a_operation, HRESULT a_result)
{
    return {ErrorCategory::PlatformFailure, a_operation, static_cast<std::int64_t>(a_result)};
}
} // namespace

/// @brief Heap Page と空き区間を共有所有する
struct DX12PlacedResourceAllocator::State final
{
    struct Range final
    {
        std::uint64_t offset = 0;
        std::uint64_t size = 0;
    };

    struct Page final
    {
        Microsoft::WRL::ComPtr<ID3D12Heap> heap;
        std::vector<Range> freeRanges;
        std::uint64_t size = 0;
        std::uint64_t heapAlignment = 0;
        std::uint64_t occupiedBytes = 0;
        std::size_t liveAllocations = 0;
        D3D12_HEAP_FLAGS flags = D3D12_HEAP_FLAG_NONE;
    };

    /// @brief Resource 解放後に領域を戻し、隣接する空き区間を結合する
    void release(const std::shared_ptr<Page>& a_page, std::uint64_t a_offset, std::uint64_t a_size) noexcept
    {
        std::lock_guard lock(mutex);
        auto position = std::lower_bound(a_page->freeRanges.begin(), a_page->freeRanges.end(), a_offset,
                                         [](const Range& a_range, std::uint64_t a_value) {
                                             return a_range.offset < a_value;
                                         });
        position = a_page->freeRanges.insert(position, {a_offset, a_size});
        if (position != a_page->freeRanges.begin())
        {
            auto previous = position - 1;
            if (previous->offset + previous->size == position->offset)
            {
                previous->size += position->size;
                position = a_page->freeRanges.erase(position);
                position = previous;
            }
        }
        while (position + 1 != a_page->freeRanges.end() &&
               position->offset + position->size == (position + 1)->offset)
        {
            position->size += (position + 1)->size;
            a_page->freeRanges.erase(position + 1);
        }
        --a_page->liveAllocations;
        a_page->occupiedBytes -= a_size;

        // 大きな専用 Heap と余分な空 Heap は常駐させず、通常 Page は一つを再利用する
        if (a_page->liveAllocations == 0)
        {
            std::size_t sameKindPages = 0;
            for (const auto& page : pages)
            {
                sameKindPages += page->flags == a_page->flags ? 1 : 0;
            }
            if (a_page->size > pageSize || sameKindPages > 1)
            {
                std::erase_if(pages, [&a_page](const auto& a_candidate) { return a_candidate == a_page; });
            }
        }
    }

    std::mutex mutex;
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    std::vector<std::shared_ptr<Page>> pages;
    std::uint64_t pageSize = 0;
};

/// @brief Native Resource より後に Heap 領域を返す所有 Token
struct DX12PlacedResourceAllocator::Allocation final
{
    /// @brief 配置先 Page と回収先 State を保持する
    Allocation(std::shared_ptr<State> a_state, std::shared_ptr<State::Page> a_page,
               std::uint64_t a_offset, std::uint64_t a_size) noexcept
        : state(std::move(a_state)), page(std::move(a_page)), offset(a_offset), size(a_size)
    {
    }

    /// @brief GPU 完了後に Resource が破棄された領域だけを返す
    ~Allocation()
    {
        if (isActive)
        {
            state->release(page, offset, size);
        }
    }

    std::shared_ptr<State> state;
    std::shared_ptr<State::Page> page;
    std::uint64_t offset;
    std::uint64_t size;
    bool isActive = false;
};

/// @brief create の内部でのみ空の Allocator を構築する
DX12PlacedResourceAllocator::DX12PlacedResourceAllocator(CreateToken) noexcept
{
}

/// @brief Device と整列済み Page Size を持つ Allocator を構築する
Result<std::unique_ptr<DX12PlacedResourceAllocator>> DX12PlacedResourceAllocator::create(
    DX12RenderDevice& a_device, std::uint64_t a_pageSize)
{
    using AllocatorResult = Result<std::unique_ptr<DX12PlacedResourceAllocator>>;
    std::uint64_t alignedSize = 0;
    if (!a_device.device() || a_pageSize == 0 || !align_up(a_pageSize, k_heapAlignment, alignedSize))
    {
        return AllocatorResult::failure({ErrorCategory::InvalidArgument, "DX12PlacedResourceAllocator.create"});
    }
    try
    {
        auto allocator = std::make_unique<DX12PlacedResourceAllocator>(CreateToken{});
        allocator->m_state = std::make_shared<State>();
        allocator->m_state->device = a_device.device();
        allocator->m_state->pageSize = alignedSize;
        return AllocatorResult::success(std::move(allocator));
    }
    catch (const std::bad_alloc&)
    {
        return AllocatorResult::failure({ErrorCategory::PlatformFailure,
                                         "DX12PlacedResourceAllocator.create.allocation"});
    }
}

/// @brief 常駐用 Upload／Readback を拒否し、Default Buffer を配置する
Result<std::unique_ptr<DX12GpuResource>> DX12PlacedResourceAllocator::create_buffer(GpuBufferDesc a_desc)
{
    using ResourceResult = Result<std::unique_ptr<DX12GpuResource>>;
    if (a_desc.memory != GpuMemoryUsage::Default)
    {
        return ResourceResult::failure({ErrorCategory::InvalidArgument,
                                        "DX12PlacedResourceAllocator.create_buffer.memory"});
    }
    auto descResult = DX12GpuResource::buffer_desc(a_desc);
    if (!descResult.has_value())
    {
        return ResourceResult::failure(*descResult.try_error());
    }
    return create_resource(*descResult.try_value(), GpuResourceKind::Buffer, a_desc.byteSize,
                           D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS, L"CueEngine DX12 Placed Buffer");
}

/// @brief Texture の RenderTarget 用途に合う Heap 種類へ配置する
Result<std::unique_ptr<DX12GpuResource>> DX12PlacedResourceAllocator::create_texture2d(GpuTexture2DDesc a_desc)
{
    using ResourceResult = Result<std::unique_ptr<DX12GpuResource>>;
    auto descResult = DX12GpuResource::texture2d_desc(a_desc);
    if (!descResult.has_value())
    {
        return ResourceResult::failure(*descResult.try_error());
    }
    const auto heapFlags = a_desc.isRenderTarget ? D3D12_HEAP_FLAG_ALLOW_ONLY_RT_DS_TEXTURES
                                                 : D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES;
    return create_resource(*descResult.try_value(), GpuResourceKind::Texture2D, 0, heapFlags,
                           L"CueEngine DX12 Placed Texture2D",
                           a_desc.isRenderTarget ? &a_desc.clearColor : nullptr);
}

/// @brief 同時使用しない Default Buffer 群の Native 定義を検証して一領域へ作る
Result<std::vector<std::unique_ptr<DX12GpuResource>>> DX12PlacedResourceAllocator::create_alias_buffers(
    std::span<const GpuBufferDesc> a_descs)
{
    using GroupResult = Result<std::vector<std::unique_ptr<DX12GpuResource>>>;
    if (a_descs.empty())
    {
        return GroupResult::failure({ErrorCategory::InvalidArgument,
                                     "DX12PlacedResourceAllocator.alias_buffers.empty"});
    }
    try
    {
        std::vector<D3D12_RESOURCE_DESC> nativeDescs;
        std::vector<std::uint64_t> sizes;
        nativeDescs.reserve(a_descs.size());
        sizes.reserve(a_descs.size());
        for (const auto& desc : a_descs)
        {
            if (desc.memory != GpuMemoryUsage::Default)
            {
                return GroupResult::failure(
                    {ErrorCategory::InvalidArgument, "DX12PlacedResourceAllocator.alias_buffers.memory"});
            }
            auto nativeResult = DX12GpuResource::buffer_desc(desc);
            if (!nativeResult.has_value())
            {
                return GroupResult::failure(*nativeResult.try_error());
            }
            nativeDescs.push_back(nativeResult.take_value());
            sizes.push_back(desc.byteSize);
        }
        return create_resources(nativeDescs, GpuResourceKind::Buffer, sizes,
                                D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS, L"CueEngine DX12 Alias Buffer");
    }
    catch (const std::bad_alloc&)
    {
        return GroupResult::failure(
            {ErrorCategory::PlatformFailure, "DX12PlacedResourceAllocator.alias_buffers.allocation"});
    }
}

/// @brief 同時使用しない Texture 群の Native 定義を検証して一領域へ作る
Result<std::vector<std::unique_ptr<DX12GpuResource>>> DX12PlacedResourceAllocator::create_alias_texture2ds(
    std::span<const GpuTexture2DDesc> a_descs)
{
    using GroupResult = Result<std::vector<std::unique_ptr<DX12GpuResource>>>;
    if (a_descs.empty())
    {
        return GroupResult::failure(
            {ErrorCategory::InvalidArgument, "DX12PlacedResourceAllocator.alias_texture2ds.empty"});
    }
    try
    {
        const bool isRenderTarget = a_descs.front().isRenderTarget;
        std::vector<D3D12_RESOURCE_DESC> nativeDescs;
        nativeDescs.reserve(a_descs.size());
        std::vector<std::uint64_t> sizes(a_descs.size(), 0);
        std::vector<std::array<float, 4>> clearColors;
        if (isRenderTarget)
        {
            clearColors.reserve(a_descs.size());
        }
        for (const auto& desc : a_descs)
        {
            if (desc.isRenderTarget != isRenderTarget)
            {
                return GroupResult::failure({ErrorCategory::InvalidArgument,
                                             "DX12PlacedResourceAllocator.alias_texture2ds.heap_type"});
            }
            auto nativeResult = DX12GpuResource::texture2d_desc(desc);
            if (!nativeResult.has_value())
            {
                return GroupResult::failure(*nativeResult.try_error());
            }
            nativeDescs.push_back(nativeResult.take_value());
            if (isRenderTarget)
            {
                clearColors.push_back(desc.clearColor);
            }
        }
        const auto heapFlags = isRenderTarget ? D3D12_HEAP_FLAG_ALLOW_ONLY_RT_DS_TEXTURES
                                              : D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES;
        return create_resources(nativeDescs, GpuResourceKind::Texture2D, sizes, heapFlags,
                                L"CueEngine DX12 Alias Texture2D", clearColors);
    }
    catch (const std::bad_alloc&)
    {
        return GroupResult::failure(
            {ErrorCategory::PlatformFailure, "DX12PlacedResourceAllocator.alias_texture2ds.allocation"});
    }
}

/// @brief Heap の実確保量と生きている Resource の占有量を Snapshot する
DX12PlacedAllocatorStats DX12PlacedResourceAllocator::stats() const noexcept
{
    std::lock_guard lock(m_state->mutex);
    DX12PlacedAllocatorStats result{};
    result.heapCount = static_cast<std::uint32_t>(m_state->pages.size());
    for (const auto& page : m_state->pages)
    {
        result.reservedBytes += page->size;
        result.occupiedBytes += page->occupiedBytes;
    }
    return result;
}

/// @brief Adapter 固有の配置要件を調べ、適合する Heap の空き区間に Resource を生成する
Result<std::unique_ptr<DX12GpuResource>> DX12PlacedResourceAllocator::create_resource(
    const D3D12_RESOURCE_DESC& a_desc, GpuResourceKind a_kind, std::uint64_t a_bufferSize,
    D3D12_HEAP_FLAGS a_heapFlags, const wchar_t* a_name,
    const std::array<float, 4>* a_clearColor)
{
    using ResourceResult = Result<std::unique_ptr<DX12GpuResource>>;
    const std::array descs{a_desc};
    const std::array sizes{a_bufferSize};
    const std::span<const std::array<float, 4>> clearColors =
        a_clearColor ? std::span{a_clearColor, 1} : std::span<const std::array<float, 4>>{};
    auto groupResult = create_resources(descs, a_kind, sizes, a_heapFlags, a_name, clearColors);
    if (!groupResult.has_value())
    {
        return ResourceResult::failure(*groupResult.try_error());
    }
    auto group = groupResult.take_value();
    return ResourceResult::success(std::move(group.front()));
}

/// @brief Group の最大配置要件で領域を一度予約し、全 Native Resource を重複生成する
Result<std::vector<std::unique_ptr<DX12GpuResource>>> DX12PlacedResourceAllocator::create_resources(
    std::span<const D3D12_RESOURCE_DESC> a_descs, GpuResourceKind a_kind,
    std::span<const std::uint64_t> a_bufferSizes, D3D12_HEAP_FLAGS a_heapFlags, const wchar_t* a_name,
    std::span<const std::array<float, 4>> a_clearColors)
{
    using GroupResult = Result<std::vector<std::unique_ptr<DX12GpuResource>>>;
    if (a_descs.empty() || a_descs.size() != a_bufferSizes.size() ||
        (!a_clearColors.empty() && a_clearColors.size() != a_descs.size()))
    {
        return GroupResult::failure({ErrorCategory::InvalidArgument,
                                     "DX12PlacedResourceAllocator.create_resources.descs"});
    }
    std::uint64_t requiredSize = 0;
    std::uint64_t requiredAlignment = 0;
    for (const auto& desc : a_descs)
    {
        const D3D12_RESOURCE_ALLOCATION_INFO info = m_state->device->GetResourceAllocationInfo(0, 1, &desc);
        if (info.SizeInBytes == 0 || info.SizeInBytes == (std::numeric_limits<std::uint64_t>::max)() ||
            info.Alignment == 0 || (info.Alignment & (info.Alignment - 1)) != 0)
        {
            return GroupResult::failure({ErrorCategory::PlatformFailure,
                                         "ID3D12Device.GetResourceAllocationInfo"});
        }
        requiredSize = (std::max)(requiredSize, info.SizeInBytes);
        requiredAlignment = (std::max)(requiredAlignment, info.Alignment);
    }

    std::lock_guard lock(m_state->mutex);
    try
    {
        std::shared_ptr<State::Page> selected;
        std::size_t rangeIndex = 0;
        std::uint64_t offset = 0;
        bool isNewPage = false;
        for (const auto& page : m_state->pages)
        {
            if (page->flags != a_heapFlags || page->heapAlignment < requiredAlignment)
            {
                continue;
            }
            for (std::size_t index = 0; index < page->freeRanges.size(); ++index)
            {
                const State::Range& range = page->freeRanges[index];
                std::uint64_t aligned = 0;
                if (align_up(range.offset, requiredAlignment, aligned) &&
                    aligned <= range.offset + range.size &&
                    requiredSize <= range.offset + range.size - aligned)
                {
                    selected = page;
                    rangeIndex = index;
                    offset = aligned;
                    break;
                }
            }
            if (selected)
            {
                break;
            }
        }

        if (!selected)
        {
            const std::uint64_t heapAlignment = (std::max)(requiredAlignment, k_heapAlignment);
            const std::uint64_t desiredSize = (std::max)(requiredSize, m_state->pageSize);
            std::uint64_t heapSize = 0;
            if (!align_up(desiredSize, heapAlignment, heapSize))
            {
                return GroupResult::failure({ErrorCategory::InvalidArgument,
                                             "DX12PlacedResourceAllocator.create_resources.heap_size"});
            }
            // 新 Page は Resource 生成が成功するまで公開しない
            const std::size_t requiredPages = m_state->pages.size() + 1;
            if (m_state->pages.capacity() < requiredPages)
            {
                const std::size_t doubled = m_state->pages.capacity() == 0 ? 1 : m_state->pages.capacity() * 2;
                m_state->pages.reserve(doubled > requiredPages ? doubled : requiredPages);
            }
            selected = std::make_shared<State::Page>();
            selected->size = heapSize;
            selected->heapAlignment = heapAlignment;
            selected->flags = a_heapFlags;
            selected->freeRanges.reserve(4);
            selected->freeRanges.push_back({0, heapSize});
            D3D12_HEAP_DESC heapDesc{};
            heapDesc.SizeInBytes = heapSize;
            heapDesc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
            heapDesc.Properties.CreationNodeMask = 1;
            heapDesc.Properties.VisibleNodeMask = 1;
            heapDesc.Alignment = heapAlignment;
            heapDesc.Flags = a_heapFlags;
            const HRESULT heapResult = m_state->device->CreateHeap(&heapDesc, IID_PPV_ARGS(&selected->heap));
            if (FAILED(heapResult))
            {
                return GroupResult::failure(placed_error("ID3D12Device.CreateHeap", heapResult));
            }
            const wchar_t* heapName = a_kind == GpuResourceKind::Buffer
                                          ? L"CueEngine DX12 Transient Buffer Heap"
                                          : L"CueEngine DX12 Transient Texture Heap";
            const HRESULT nameResult = selected->heap->SetName(heapName);
            if (FAILED(nameResult))
            {
                report_log_error("DX12PlacedResourceAllocator",
                             placed_error("ID3D12Heap.SetName", nameResult), LogLevel::Warning);
            }
            isNewPage = true;
        }

        // 将来の返却が Allocation Destructor 内で再確保を起こさない容量を先取りする
        const std::size_t required = selected->freeRanges.size() + selected->liveAllocations + 2;
        if (selected->freeRanges.capacity() < required)
        {
            const std::size_t doubled = selected->freeRanges.capacity() * 2;
            selected->freeRanges.reserve(doubled > required ? doubled : required);
        }
        auto allocation = std::make_shared<Allocation>(m_state, selected, offset, requiredSize);
        std::vector<std::unique_ptr<DX12GpuResource>> resources;
        resources.reserve(a_descs.size());
        for (std::size_t index = 0; index < a_descs.size(); ++index)
        {
            const auto& desc = a_descs[index];
            auto resource = std::make_unique<DX12GpuResource>(DX12GpuResource::CreateToken{});
            D3D12_CLEAR_VALUE clearValue{};
            const D3D12_CLEAR_VALUE* optimizedClear = nullptr;
            if ((desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0)
            {
                if (a_clearColors.empty())
                {
                    return GroupResult::failure({ErrorCategory::InvalidArgument,
                                                 "DX12PlacedResourceAllocator.create_resources.clear_color"});
                }
                clearValue.Format = desc.Format;
                for (std::size_t channel = 0; channel < 4; ++channel)
                {
                    clearValue.Color[channel] = a_clearColors[index][channel];
                }
                optimizedClear = &clearValue;
            }
            const HRESULT resourceResult = m_state->device->CreatePlacedResource(
                selected->heap.Get(), offset, &desc, D3D12_RESOURCE_STATE_COMMON, optimizedClear,
                IID_PPV_ARGS(&resource->m_resource));
            if (FAILED(resourceResult))
            {
                return GroupResult::failure(placed_error("ID3D12Device.CreatePlacedResource", resourceResult));
            }
            const HRESULT nameResult = resource->m_resource->SetName(a_name);
            if (FAILED(nameResult))
            {
                report_log_error("DX12PlacedResourceAllocator",
                             placed_error("ID3D12Resource.SetName", nameResult), LogLevel::Warning);
            }
            resources.push_back(std::move(resource));
        }

        // Native Resource と Allocation の生成に成功した後だけ空き区間を書き換える
        State::Range& range = selected->freeRanges[rangeIndex];
        const std::uint64_t rangeEnd = range.offset + range.size;
        const std::uint64_t placedEnd = offset + requiredSize;
        if (offset > range.offset && placedEnd < rangeEnd)
        {
            const std::uint64_t originalOffset = range.offset;
            range.size = offset - originalOffset;
            selected->freeRanges.insert(selected->freeRanges.begin() + rangeIndex + 1,
                                        {placedEnd, rangeEnd - placedEnd});
        }
        else if (offset > range.offset)
        {
            range.size = offset - range.offset;
        }
        else if (placedEnd < rangeEnd)
        {
            range.offset = placedEnd;
            range.size = rangeEnd - placedEnd;
        }
        else
        {
            selected->freeRanges.erase(selected->freeRanges.begin() + rangeIndex);
        }
        if (isNewPage)
        {
            m_state->pages.push_back(selected);
        }
        ++selected->liveAllocations;
        selected->occupiedBytes += requiredSize;
        allocation->isActive = true;
        for (std::size_t index = 0; index < resources.size(); ++index)
        {
            auto& resource = resources[index];
            resource->m_placementLifetime = allocation;
            resource->m_placementHeap = selected->heap.Get();
            resource->m_placementOffset = offset;
            resource->m_kind = a_kind;
            resource->m_memory = GpuMemoryUsage::Default;
            resource->m_bufferSize = a_bufferSizes[index];
        }
        return GroupResult::success(std::move(resources));
    }
    catch (const std::bad_alloc&)
    {
        return GroupResult::failure({ErrorCategory::PlatformFailure,
                                     "DX12PlacedResourceAllocator.create_resources.allocation"});
    }
}
} // namespace cue::dx12
