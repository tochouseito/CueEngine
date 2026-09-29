#include "D3D12StaticMeshPool.h"

#include <array>
#include <cstddef>
#include <cstring>
#include <utility>

namespace cue::detail
{
namespace
{
struct Vertex final
{
    float position[3];
    float color[3];
};

constexpr std::array<Vertex, 3> k_vertices = {{{{-0.6f, -0.5f, 0.5f}, {1.0f, 0.2f, 0.2f}},
                                                {{0.0f, 0.6f, 0.5f}, {0.2f, 1.0f, 0.2f}},
                                                {{0.6f, -0.5f, 0.5f}, {0.2f, 0.4f, 1.0f}}}};
constexpr std::array<std::uint16_t, 3> k_indices = {0, 1, 2};
constexpr UINT64 k_vertexBytes = sizeof(k_vertices);
constexpr UINT64 k_indexBytes = sizeof(k_indices);
constexpr UINT64 k_totalBytes = k_vertexBytes + k_indexBytes;

/// @brief 連続する Vertex と Index Buffer に共通する Resource 設定を作る
D3D12_RESOURCE_DESC buffer_desc()
{
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = k_totalBytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return desc;
}
} // namespace

/// @brief Triangle を Pool 所有 Queue に転送し、Upload を Mesh の破棄まで保持する
Result<std::unique_ptr<D3D12StaticMeshPool>> D3D12StaticMeshPool::create(D3D12DeviceContext& a_device,
                                                                          D3D12ResourcePool& a_resources)
{
    using PoolResult = Result<std::unique_ptr<D3D12StaticMeshPool>>;
    auto pool = std::make_unique<D3D12StaticMeshPool>();
    pool->m_resources = &a_resources;
    const auto desc = buffer_desc();
    auto geometryResult = a_resources.create_buffer({k_totalBytes, GpuMemory::Device});
    if (!geometryResult.has_value())
    {
        return PoolResult::failure(*geometryResult.try_error());
    }
    pool->m_geometry = geometryResult.take_value();
    auto physicalResult = a_resources.resource(pool->m_geometry);
    if (!physicalResult.has_value())
    {
        return PoolResult::failure(*physicalResult.try_error());
    }
    ID3D12Resource* geometry = physicalResult.take_value();
    HRESULT result = geometry->SetName(L"CueEngine Fixed Mesh Geometry");
    if (FAILED(result))
    {
        return PoolResult::failure(gpu_error("ID3D12Resource.SetName.Mesh", result));
    }
    D3D12_HEAP_PROPERTIES uploadHeap{};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
    Microsoft::WRL::ComPtr<ID3D12Resource> upload;
    result = a_device.device()->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload));
    if (FAILED(result))
    {
        return PoolResult::failure(gpu_error("ID3D12Device.CreateCommittedResource.MeshUpload", result));
    }
    result = upload->SetName(L"CueEngine Fixed Mesh Upload");
    if (FAILED(result))
    {
        return PoolResult::failure(gpu_error("ID3D12Resource.SetName.MeshUpload", result));
    }
    void* mapped = nullptr;
    result = upload->Map(0, nullptr, &mapped);
    if (FAILED(result))
    {
        return PoolResult::failure(gpu_error("ID3D12Resource.Map.MeshUpload", result));
    }
    std::memcpy(mapped, k_vertices.data(), k_vertexBytes);
    std::memcpy(static_cast<std::byte*>(mapped) + k_vertexBytes, k_indices.data(), k_indexBytes);
    upload->Unmap(0, nullptr);

    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    result = a_device.device()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
    if (FAILED(result))
    {
        return PoolResult::failure(gpu_error("ID3D12Device.CreateCommandAllocator.MeshUpload", result));
    }
    result = allocator->SetName(L"CueEngine Fixed Mesh Upload Allocator");
    if (FAILED(result))
    {
        return PoolResult::failure(gpu_error("ID3D12CommandAllocator.SetName.MeshUpload", result));
    }
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    result = a_device.device()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                                  IID_PPV_ARGS(&list));
    if (FAILED(result))
    {
        return PoolResult::failure(gpu_error("ID3D12Device.CreateCommandList.MeshUpload", result));
    }
    result = list->SetName(L"CueEngine Fixed Mesh Upload Command List");
    if (FAILED(result))
    {
        return PoolResult::failure(gpu_error("ID3D12GraphicsCommandList.SetName.MeshUpload", result));
    }
    list->CopyBufferRegion(geometry, 0, upload.Get(), 0, k_totalBytes);
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = geometry;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER |
                                    D3D12_RESOURCE_STATE_INDEX_BUFFER;
    list->ResourceBarrier(1, &barrier);
    result = list->Close();
    if (FAILED(result))
    {
        return PoolResult::failure(gpu_error("ID3D12GraphicsCommandList.Close.MeshUpload", result));
    }
    ID3D12CommandList* lists[] = {list.Get()};
    // 後続の描画は同じ Graphics Queue に積まれるため、転送完了まで資源を保持すれば CPU 待機は不要
    a_resources.graphics_queue().queue()->ExecuteCommandLists(1, lists);
    pool->m_upload = std::move(upload);
    pool->m_uploadAllocator = std::move(allocator);
    pool->m_uploadList = std::move(list);
    pool->m_isAllocated = true;
    return PoolResult::success(std::move(pool));
}

/// @brief Graph の GPU 作業終了後に Resource Pool へ幾何 Buffer を返す
D3D12StaticMeshPool::~D3D12StaticMeshPool()
{
    if (m_resources && m_geometry.owner)
    {
        [[maybe_unused]] auto result = m_resources->destroy(m_geometry);
    }
}

/// @brief Pool と同じ寿命の Triangle Handle を返す
StaticMeshHandle D3D12StaticMeshPool::triangle() const noexcept
{
    return {0, m_isAllocated ? m_generation : 0};
}

/// @brief 登録済み Handle のみ描画して古い参照を拒否する
Result<void> D3D12StaticMeshPool::draw(IGpuCommandRecorder& a_commands, StaticMeshHandle a_handle) const
{
    if (!m_isAllocated || a_handle.index != 0 || a_handle.generation != m_generation)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "D3D12StaticMeshPool.draw"});
    }
    auto vertexResult = a_commands.bind_vertex_buffer(m_geometry, 0, sizeof(Vertex),
                                                       static_cast<std::uint32_t>(k_vertexBytes));
    if (!vertexResult.has_value())
    {
        return vertexResult;
    }
    auto indexResult = a_commands.bind_index_buffer(m_geometry, k_vertexBytes,
                                                     static_cast<std::uint32_t>(k_indexBytes));
    if (!indexResult.has_value())
    {
        return indexResult;
    }
    return a_commands.draw_indexed(static_cast<std::uint32_t>(k_indices.size()));
}

/// @brief Queue 完了を確認してから Handle と Buffer を破棄する
Result<void> D3D12StaticMeshPool::destroy(StaticMeshHandle a_handle)
{
    if (!m_isAllocated || a_handle.index != 0 || a_handle.generation != m_generation)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "D3D12StaticMeshPool.destroy"});
    }
    auto released = m_resources->destroy(m_geometry);
    if (!released.has_value())
    {
        return released;
    }
    m_geometry = {};
    m_uploadList.Reset();
    m_uploadAllocator.Reset();
    m_upload.Reset();
    m_isAllocated = false;
    ++m_generation;
    return Result<void>::success();
}
} // namespace cue::detail
