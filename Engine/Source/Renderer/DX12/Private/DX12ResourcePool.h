#pragma once

#include "DX12RenderDevice.h"
#include "DX12QueuePool.h"
#include "DescriptorAllocator.h"

#include <Cue/Renderer/RHI/GpuResources.h>

#include <array>
#include <cstddef>
#include <memory>
#include <span>
#include <vector>

namespace cue::detail
{
/// @brief DX12 の Buffer、Texture と View を世代付き Handle で所有する
class DX12ResourcePool final : public IGpuResources
{
public:
    using IGpuResources::create_view;
    ~DX12ResourcePool() override;
    /// @brief Device と Queue Pool を借用して Descriptor Heap を生成する
    [[nodiscard]] static Result<std::unique_ptr<DX12ResourcePool>> create(DX12RenderDevice& a_device,
                                                                             DX12QueuePool& a_queues,
                                                                             UINT a_viewCapacity = 32);

    /// @brief Backend が所有する共通 DescriptorAllocator を借用して Resource と View を生成する
    [[nodiscard]] static Result<std::unique_ptr<DX12ResourcePool>> create(DX12RenderDevice& a_device,
                                                                             DX12QueuePool& a_queues,
                                                                             DescriptorAllocator& a_allocator);

    /// @brief Buffer の物理資源を生成して世代付き Handle を返す
    [[nodiscard]] Result<GpuResourceHandle> create_buffer(GpuBufferDesc a_desc) override;

    /// @brief Texture の物理資源を生成して世代付き Handle を返す
    [[nodiscard]] Result<GpuResourceHandle> create_texture(GpuTextureDesc a_desc) override;

    /// @brief 各 Subresource の初期 Data を即時転送して Texture を生成する
    [[nodiscard]] Result<GpuResourceHandle> create_texture(
        GpuTextureDesc a_desc, std::span<const GpuTextureSubresourceData> a_initialData);

    /// @brief Upload Buffer への範囲書込を Map と Unmap で完結する
    [[nodiscard]] Result<void> write_buffer(GpuResourceHandle a_buffer, std::uint64_t a_offset,
                                            const void* a_data, std::uint64_t a_size) override;

    /// @brief Readback Buffer の GPU 完了を待ってから範囲を読む
    [[nodiscard]] Result<void> read_buffer(GpuResourceHandle a_buffer, std::uint64_t a_offset,
                                           void* a_data, std::uint64_t a_size) override;

    /// @brief Resource の種類と Buffer 範囲に合う Descriptor を生成する
    [[nodiscard]] Result<GpuViewHandle> create_view(GpuResourceHandle a_resource,
                                                      GpuViewDesc a_desc) override;

    /// @brief GPU 完了後に Descriptor Slot を返す
    [[nodiscard]] Result<void> destroy_view(GpuViewHandle a_view) override;

    /// @brief GPU 完了後に View を持たない資源を破棄する
    [[nodiscard]] Result<void> destroy(GpuResourceHandle a_resource) override;

    /// @brief 有効な Handle の DX12 資源を Pool の寿命内で借用する
    [[nodiscard]] Result<ID3D12Resource*> resource(GpuResourceHandle a_resource) const;

    /// @brief Buffer の指定 Heap と Slice の物理資源を借用する
    [[nodiscard]] Result<ID3D12Resource*> resource(GpuResourceHandle a_resource, GpuMemory a_memory,
                                                     std::uint32_t a_index) const;

    /// @brief 複数 Buffer を持つ Texture の物理資源を借用する
    [[nodiscard]] Result<ID3D12Resource*> texture_resource(GpuResourceHandle a_texture,
                                                             std::uint32_t a_index) const;

    /// @brief 作成時に確定した Texture 記述子を返す
    [[nodiscard]] Result<GpuTextureDesc> texture_desc(GpuResourceHandle a_texture) const;

    /// @brief Upload または Readback Slice の Map 済み領域を借用する
    [[nodiscard]] Result<GpuBufferCpuView> buffer_cpu_view(GpuResourceHandle a_buffer, GpuMemory a_memory);

    /// @brief 有効な Resource Handle が Texture を指すか返す
    [[nodiscard]] Result<bool> is_texture(GpuResourceHandle a_resource) const;

    /// @brief 有効な View の CPU Descriptor を借用する
    [[nodiscard]] Result<D3D12_CPU_DESCRIPTOR_HANDLE> cpu_handle(GpuViewHandle a_view) const;

    /// @brief View の元 Texture Format を検証付きで返す
    [[nodiscard]] Result<GpuTextureFormat> view_texture_format(GpuViewHandle a_view) const;

    /// @brief View が参照する物理 Resource を View の寿命内で借用する
    [[nodiscard]] Result<ID3D12Resource*> view_resource(GpuViewHandle a_view) const;

    /// @brief ShaderResource View の GPU Descriptor を借用する
    [[nodiscard]] Result<D3D12_GPU_DESCRIPTOR_HANDLE> gpu_handle(GpuViewHandle a_view) const;

    /// @brief ShaderResource View の Heap を Bind の間だけ借用する
    [[nodiscard]] ID3D12DescriptorHeap* shader_heap() const noexcept;

    /// @brief Texture Table に確保した連続 Descriptor 数を返す
    [[nodiscard]] UINT texture_table_capacity() const noexcept;

    /// @brief Pool が寿命を管理する Graphics Queue を内部転送に貸す
    [[nodiscard]] DX12GpuCommandQueue& graphics_queue() const noexcept;

private:
    struct ResourceRecord final
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        std::array<std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>, 3> bufferSlices;
        std::array<std::vector<std::byte*>, 3> mappedSlices;
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> textureSlices;
        GpuTextureDesc textureDesc;
        GpuTextureFormat format = GpuTextureFormat::Rgba8Unorm;
        GpuMemory memory = GpuMemory::Device;
        std::uint32_t alignment = 0;
        std::uint32_t stride = 0;
        std::uint32_t elementCount = 0;
        std::uint64_t generation = 0;
        bool isTexture = false;
        bool allowUnorderedAccess = false;
    };

    struct ViewRecord final
    {
        GpuResourceHandle resource;
        ID3D12Resource* physicalResource = nullptr;
        GpuViewKind kind = GpuViewKind::RenderTarget;
        std::uint64_t generation = 0;
        bool isAllocated = false;
    };

    /// @brief Descriptor 種別を検証する
    [[nodiscard]] static bool is_view_kind(GpuViewKind a_kind) noexcept;

    /// @brief RTV、DSV、共通 CBV／SRV／UAV Heap の添字へ変換する
    [[nodiscard]] static std::size_t heap_index(GpuViewKind a_kind) noexcept;

    /// @brief 公開 View 種別を共通 Allocator の Heap 種別へ変換する
    [[nodiscard]] static DescriptorHeapKind heap_kind(GpuViewKind a_kind) noexcept;

    /// @brief Pool、Index、世代と所有資源の一致を検証する
    [[nodiscard]] bool owns(GpuResourceHandle a_resource) const noexcept;

    /// @brief Pool、Index、世代と割当状態の一致を検証する
    [[nodiscard]] bool owns(GpuViewHandle a_view) const noexcept;

    /// @brief 作成済み ComPtr を空き Slot に格納する
    [[nodiscard]] GpuResourceHandle store(Microsoft::WRL::ComPtr<ID3D12Resource> a_resource,
                                          bool a_isTexture, GpuTextureFormat a_format, GpuMemory a_memory,
                                          bool a_allowUnorderedAccess);

    /// @brief Pool が永続 Map した Slice を全て Unmap する
    static void unmap_slices(ResourceRecord& a_record) noexcept;

    ID3D12Device* m_device = nullptr;
    DX12QueuePool* m_queues = nullptr;
    std::unique_ptr<DescriptorAllocator> m_ownedAllocator;
    DescriptorAllocator* m_allocator = nullptr;
    std::vector<ResourceRecord> m_resources;
    std::array<std::vector<ViewRecord>, 3> m_views;
};
} // namespace cue::detail
