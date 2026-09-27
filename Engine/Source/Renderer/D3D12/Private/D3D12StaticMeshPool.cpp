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

/// @brief Triangle を Upload Heap から Default Heap に転送し、完了後に Upload を解放する
Result<std::unique_ptr<D3D12StaticMeshPool>> D3D12StaticMeshPool::create(D3D12DeviceContext& a_device)
{
    using PoolResult = Result<std::unique_ptr<D3D12StaticMeshPool>>;
    auto pool = std::make_unique<D3D12StaticMeshPool>();
    const auto desc = buffer_desc();
    D3D12_HEAP_PROPERTIES defaultHeap{};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    HRESULT result = a_device.device()->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&pool->m_geometry));
    if (FAILED(result))
    {
        return PoolResult::failure(gpu_error("ID3D12Device.CreateCommittedResource.Mesh", result));
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
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    result = a_device.device()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                                  IID_PPV_ARGS(&list));
    if (FAILED(result))
    {
        return PoolResult::failure(gpu_error("ID3D12Device.CreateCommandList.MeshUpload", result));
    }
    list->CopyBufferRegion(pool->m_geometry.Get(), 0, upload.Get(), 0, k_totalBytes);
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = pool->m_geometry.Get();
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
    a_device.queue()->ExecuteCommandLists(1, lists);
    auto waitResult = a_device.wait_idle();
    if (!waitResult.has_value())
    {
        return PoolResult::failure(*waitResult.try_error());
    }
    pool->m_isAllocated = true;
    return PoolResult::success(std::move(pool));
}

/// @brief Pool と同じ寿命の Triangle Handle を返す
StaticMeshHandle D3D12StaticMeshPool::triangle() const noexcept
{
    return {0, m_isAllocated ? m_generation : 0};
}

/// @brief 登録済み Handle のみ描画して古い参照を拒否する
Result<void> D3D12StaticMeshPool::draw(ID3D12GraphicsCommandList* a_list, StaticMeshHandle a_handle) const
{
    if (!a_list || !m_isAllocated || a_handle.index != 0 || a_handle.generation != m_generation)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "D3D12StaticMeshPool.draw"});
    }
    D3D12_VERTEX_BUFFER_VIEW vertex{};
    vertex.BufferLocation = m_geometry->GetGPUVirtualAddress();
    vertex.SizeInBytes = static_cast<UINT>(k_vertexBytes);
    vertex.StrideInBytes = sizeof(Vertex);
    D3D12_INDEX_BUFFER_VIEW index{};
    index.BufferLocation = m_geometry->GetGPUVirtualAddress() + k_vertexBytes;
    index.SizeInBytes = static_cast<UINT>(k_indexBytes);
    index.Format = DXGI_FORMAT_R16_UINT;
    a_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    a_list->IASetVertexBuffers(0, 1, &vertex);
    a_list->IASetIndexBuffer(&index);
    a_list->DrawIndexedInstanced(static_cast<UINT>(k_indices.size()), 1, 0, 0, 0);
    return Result<void>::success();
}

/// @brief Queue 完了を確認してから Handle と Buffer を破棄する
Result<void> D3D12StaticMeshPool::destroy(D3D12DeviceContext& a_device, StaticMeshHandle a_handle)
{
    if (!m_isAllocated || a_handle.index != 0 || a_handle.generation != m_generation)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "D3D12StaticMeshPool.destroy"});
    }
    auto waitResult = a_device.wait_idle();
    if (!waitResult.has_value())
    {
        return waitResult;
    }
    m_geometry.Reset();
    m_isAllocated = false;
    ++m_generation;
    return Result<void>::success();
}
} // namespace cue::detail
