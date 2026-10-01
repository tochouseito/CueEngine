#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include <d3d12.h>

#include <Foundation/Result.h>
#include <RHI/GpuResource.h>

namespace cue::dx12
{
class DX12RenderDevice;
class DX12GpuResource;

/// @brief Default Heap の配置状況を検証するための値
struct DX12PlacedAllocatorStats final
{
    std::uint32_t heapCount = 0;
    std::uint64_t reservedBytes = 0;
    std::uint64_t occupiedBytes = 0;
};

/// @brief Transient Buffer／Texture を重複しない Heap 領域へ配置する
///
/// 生成と回収は直列化される。返した Resource は呼出側が所有し、GPU 完了まで維持する
/// Heap は Resource の最後の参照まで保持する。初回 GPU 利用前の Activation Barrier は呼出側が記録する
class DX12PlacedResourceAllocator final
{
    struct CreateToken final
    {
    };

    struct State;
    struct Allocation;

public:
    /// @brief create の内部でのみ空の Allocator を構築する
    explicit DX12PlacedResourceAllocator(CreateToken) noexcept;

    /// @brief Default Heap のページサイズを指定して Allocator を作る
    ///
    /// 0 は拒否する。実際の Heap Size は Native Alignment に合わせる
    [[nodiscard]] static Result<std::unique_ptr<DX12PlacedResourceAllocator>> create(
        DX12RenderDevice& a_device, std::uint64_t a_pageSize = 16ull * 1024 * 1024);

    /// @brief 未返却の Resource に Heap の寿命を委ねる
    ~DX12PlacedResourceAllocator() = default;

    DX12PlacedResourceAllocator(const DX12PlacedResourceAllocator&) = delete;
    DX12PlacedResourceAllocator& operator=(const DX12PlacedResourceAllocator&) = delete;

    /// @brief Default Buffer を専有領域に配置する
    [[nodiscard]] Result<std::unique_ptr<DX12GpuResource>> create_buffer(GpuBufferDesc a_desc);

    /// @brief 二次元 Texture を専有領域に配置する
    [[nodiscard]] Result<std::unique_ptr<DX12GpuResource>> create_texture2d(GpuTexture2DDesc a_desc);

    /// @brief 同時に使わない Buffer 群を一つの領域へ重複配置する
    ///
    /// 全 Resource が同じ領域の所有 Token を共有する。GPU 完了まで全員を保持する
    [[nodiscard]] Result<std::vector<std::unique_ptr<DX12GpuResource>>> create_alias_buffers(
        std::span<const GpuBufferDesc> a_descs);

    /// @brief 同時に使わない Texture 群を一つの領域へ重複配置する
    [[nodiscard]] Result<std::vector<std::unique_ptr<DX12GpuResource>>> create_alias_texture2ds(
        std::span<const GpuTexture2DDesc> a_descs);

    /// @brief 現在の Heap 数、確保量、Resource 占有量を返す
    [[nodiscard]] DX12PlacedAllocatorStats stats() const noexcept;

private:
    /// @brief Native 定義に合う空き領域を探して Resource を作る
    [[nodiscard]] Result<std::unique_ptr<DX12GpuResource>> create_resource(
        const D3D12_RESOURCE_DESC& a_desc, GpuResourceKind a_kind, std::uint64_t a_bufferSize,
        D3D12_HEAP_FLAGS a_heapFlags, const wchar_t* a_name);

    /// @brief 最大 Size と Alignment の一領域に Resource 群を作る
    [[nodiscard]] Result<std::vector<std::unique_ptr<DX12GpuResource>>> create_resources(
        std::span<const D3D12_RESOURCE_DESC> a_descs, GpuResourceKind a_kind,
        std::span<const std::uint64_t> a_bufferSizes, D3D12_HEAP_FLAGS a_heapFlags, const wchar_t* a_name);

    std::shared_ptr<State> m_state;
};
} // namespace cue::dx12
