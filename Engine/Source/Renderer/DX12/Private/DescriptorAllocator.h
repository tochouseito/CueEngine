#pragma once

#include "DX12RenderDevice.h"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace cue::detail
{
enum class DescriptorHeapKind : std::uint8_t
{
    RenderTarget,
    DepthStencil,
    ShaderResource
};

/// @brief Heap 内の一つの割当を世代とともに識別する
struct DescriptorSlot final
{
    DescriptorHeapKind kind = DescriptorHeapKind::RenderTarget;
    UINT index = 0;
    std::uint64_t generation = 0;
    std::uint64_t ownerId = 0;
};

/// @brief Legacy と同じく RTV、DSV、CBV／SRV／UAV の Heap と空き Slot を一か所で所有する
/// @details 解放は呼出側が該当 GPU 作業の完了を確認してから行う。ImGui 用 Heap は別 Owner とする
class DescriptorAllocator final
{
public:
    /// @brief static create が完全初期化するまでは外へ公開しない
    DescriptorAllocator();

    /// @brief Heap を全て生成してから公開し、途中失敗では部分状態を破棄する
    [[nodiscard]] static Result<std::unique_ptr<DescriptorAllocator>> create(
        DX12RenderDevice& a_device, UINT a_rtvCapacity, UINT a_dsvCapacity, UINT a_shaderCapacity);

    DescriptorAllocator(const DescriptorAllocator&) = delete;
    DescriptorAllocator& operator=(const DescriptorAllocator&) = delete;

    /// @brief 空き Slot を世代更新して貸し出す
    [[nodiscard]] Result<DescriptorSlot> allocate(DescriptorHeapKind a_kind);

    /// @brief Texture Table の予約領域から shader-visible Slot を割り当てる
    [[nodiscard]] Result<DescriptorSlot> allocate_texture();

    /// @brief GPU 完了済み Slot を返し、古い世代の再解放を拒否する
    [[nodiscard]] Result<void> release(DescriptorSlot a_slot);

    /// @brief 生存する Slot の CPU Handle を貸す
    [[nodiscard]] Result<D3D12_CPU_DESCRIPTOR_HANDLE> cpu_handle(DescriptorSlot a_slot) const;

    /// @brief CPU 専用 Descriptor を同じ Index の Shader-visible Heap へ転送する
    [[nodiscard]] Result<void> sync_shader_descriptor(DescriptorSlot a_slot);

    /// @brief Shader-visible Heap の生存 Slot の GPU Handle を貸す
    [[nodiscard]] Result<D3D12_GPU_DESCRIPTOR_HANDLE> gpu_handle(DescriptorSlot a_slot) const;

    /// @brief Shader-visible Heap を Bind 中に借用する
    [[nodiscard]] ID3D12DescriptorHeap* shader_heap() const noexcept;

    /// @brief ImGui 専用 Heap を借用する。Renderer Table とは併用しない
    [[nodiscard]] ID3D12DescriptorHeap* imgui_heap() const noexcept;

    /// @brief Texture Table に割り当てた Heap の先頭を返す
    [[nodiscard]] D3D12_GPU_DESCRIPTOR_HANDLE texture_table_base() const noexcept;

    /// @brief Kind ごとの Slot 数を返す
    [[nodiscard]] UINT capacity(DescriptorHeapKind a_kind) const noexcept;

    /// @brief Shader Heap 先頭からの Texture Table 最大要素数を返す
    [[nodiscard]] UINT texture_capacity() const noexcept;

    /// @brief Pool 内 Handle を DescriptorSlot へ戻す際の Owner 識別子を返す
    [[nodiscard]] std::uint64_t owner_id() const noexcept;

private:
    struct SlotState final
    {
        std::uint64_t generation = 0;
        bool isAllocated = false;
    };

    struct HeapState final
    {
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap;
        std::vector<SlotState> slots;
        UINT stride = 0;
    };

    /// @brief 無効 Kind を配列添字へ使わない
    [[nodiscard]] static bool is_kind(DescriptorHeapKind a_kind) noexcept;

    /// @brief Pool と世代が一致する割当だけを受け付ける
    [[nodiscard]] bool owns(DescriptorSlot a_slot) const noexcept;

    std::array<HeapState, 3> m_heaps;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_shaderCpuHeap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_imguiHeap;
    UINT m_textureCapacity = 0;
    ID3D12Device* m_device = nullptr;
    std::uint64_t m_ownerId = 0;
};
} // namespace cue::detail
