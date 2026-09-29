#pragma once

#include "D3D12DeviceContext.h"
#include "D3D12QueuePool.h"

#include <Cue/Renderer/RHI/GpuResources.h>

#include <array>
#include <memory>
#include <vector>

namespace cue::detail
{
/// @brief D3D12 の Buffer、Texture と View を世代付き Handle で所有する
class D3D12ResourcePool final : public IGpuResources
{
public:
    using IGpuResources::create_view;
    /// @brief Device と Queue Pool を借用して Descriptor Heap を生成する
    [[nodiscard]] static Result<std::unique_ptr<D3D12ResourcePool>> create(D3D12DeviceContext& a_device,
                                                                             D3D12QueuePool& a_queues,
                                                                             UINT a_viewCapacity = 32);

    /// @brief Buffer の物理資源を生成して世代付き Handle を返す
    [[nodiscard]] Result<GpuResourceHandle> create_buffer(GpuBufferDesc a_desc) override;

    /// @brief Texture の物理資源を生成して世代付き Handle を返す
    [[nodiscard]] Result<GpuResourceHandle> create_texture(GpuTextureDesc a_desc) override;

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

    /// @brief 有効な Handle の D3D12 資源を Pool の寿命内で借用する
    [[nodiscard]] Result<ID3D12Resource*> resource(GpuResourceHandle a_resource) const;

    /// @brief 有効な View の CPU Descriptor を借用する
    [[nodiscard]] Result<D3D12_CPU_DESCRIPTOR_HANDLE> cpu_handle(GpuViewHandle a_view) const;

    /// @brief ShaderResource View の GPU Descriptor を借用する
    [[nodiscard]] Result<D3D12_GPU_DESCRIPTOR_HANDLE> gpu_handle(GpuViewHandle a_view) const;

    /// @brief ShaderResource View の Heap を Bind の間だけ借用する
    [[nodiscard]] ID3D12DescriptorHeap* shader_heap() const noexcept;

    /// @brief Pool が寿命を管理する Graphics Queue を内部転送に貸す
    [[nodiscard]] D3D12QueueContext& graphics_queue() const noexcept;

private:
    struct ResourceRecord final
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        GpuTextureFormat format = GpuTextureFormat::Rgba8Unorm;
        GpuMemory memory = GpuMemory::Device;
        std::uint64_t generation = 0;
        bool isTexture = false;
        bool allowUnorderedAccess = false;
    };

    struct ViewRecord final
    {
        GpuResourceHandle resource;
        GpuViewKind kind = GpuViewKind::RenderTarget;
        std::uint64_t generation = 0;
        bool isAllocated = false;
    };

    /// @brief Descriptor 種別を検証する
    [[nodiscard]] static bool is_view_kind(GpuViewKind a_kind) noexcept;

    /// @brief RTV、DSV、共通 CBV／SRV／UAV Heap の添字へ変換する
    [[nodiscard]] static std::size_t heap_index(GpuViewKind a_kind) noexcept;

    /// @brief Pool、Index、世代と所有資源の一致を検証する
    [[nodiscard]] bool owns(GpuResourceHandle a_resource) const noexcept;

    /// @brief Pool、Index、世代と割当状態の一致を検証する
    [[nodiscard]] bool owns(GpuViewHandle a_view) const noexcept;

    /// @brief 作成済み ComPtr を空き Slot に格納する
    [[nodiscard]] GpuResourceHandle store(Microsoft::WRL::ComPtr<ID3D12Resource> a_resource,
                                          bool a_isTexture, GpuTextureFormat a_format, GpuMemory a_memory,
                                          bool a_allowUnorderedAccess);

    ID3D12Device* m_device = nullptr;
    D3D12QueuePool* m_queues = nullptr;
    std::vector<ResourceRecord> m_resources;
    std::array<std::vector<ViewRecord>, 3> m_views;
    std::array<Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>, 3> m_heaps;
    std::array<UINT, 3> m_strides{};
};
} // namespace cue::detail
