#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include <d3d12.h>
#include <wrl/client.h>

#include <Foundation/Result.h>

namespace cue::dx12
{
/// @brief Allocator と Slot の世代を識別する非所有 Handle
struct DX12DescriptorHandle final
{
    std::uint32_t index = 0;
    std::uint64_t generation = 0;
    std::uint64_t allocatorId = 0;

    /// @brief 初期値のままの Handle か判定する
    [[nodiscard]] bool is_valid() const noexcept;
};

/// @brief 単一種類の DX12 Descriptor Heap と Slot の割り当て状態を所有する
///
/// Heap は Allocator の破棄まで、Handle は Slot の返却まで有効とし、Native Device は生成時だけ借用する
/// 公開操作は複数 Thread から呼べるが、取得した Native Handle の利用中に同じ Slot を解放しない
class DX12DescriptorAllocator final
{
    struct CreateToken final
    {
    };

public:
    /// @brief create だけが部分生成状態を構築する
    explicit DX12DescriptorAllocator(CreateToken) noexcept;

    /// @brief 指定種類と容量の Heap を生成し、成功時だけ Allocator を公開する
    ///
    /// Shader 可視 Heap は CBV/SRV/UAV または Sampler のみ受け付ける
    [[nodiscard]] static Result<std::unique_ptr<DX12DescriptorAllocator>> create(
        ID3D12Device& a_device, D3D12_DESCRIPTOR_HEAP_TYPE a_type, std::uint32_t a_capacity,
        bool a_isShaderVisible = false);

    /// @brief 所有する Heap を解放する。呼出側は先に GPU の参照完了を確認する
    ~DX12DescriptorAllocator() = default;

    DX12DescriptorAllocator(const DX12DescriptorAllocator&) = delete;
    DX12DescriptorAllocator& operator=(const DX12DescriptorAllocator&) = delete;

    /// @brief 空き Slot を取得し、満杯なら InvalidState を返す
    [[nodiscard]] Result<DX12DescriptorHandle> allocate();

    /// @brief 所属と世代を検証して Slot を返却する
    ///
    /// Shader 可視 Descriptor は GPU の最終参照を Fence で確認してから返却する
    /// 失敗時は割り当て状態を変更しない
    [[nodiscard]] Result<void> release(DX12DescriptorHandle a_handle);

    /// @brief 有効な Slot の CPU Handle を返す
    ///
    /// 返した Native Handle は Slot の返却または Allocator の破棄で失効する
    [[nodiscard]] Result<D3D12_CPU_DESCRIPTOR_HANDLE> cpu_handle(DX12DescriptorHandle a_handle) const;

    /// @brief Shader 可視 Heap の有効な Slot の GPU Handle を返す
    ///
    /// GPU が参照する間は Slot の内容を上書き・返却しない
    [[nodiscard]] Result<D3D12_GPU_DESCRIPTOR_HANDLE> gpu_handle(DX12DescriptorHandle a_handle) const;

    /// @brief Command List へ設定する Heap を Allocator の生存中だけ借用させる
    [[nodiscard]] ID3D12DescriptorHeap* heap() const noexcept;

    /// @brief 作成時に固定した Heap の種類を返す
    [[nodiscard]] D3D12_DESCRIPTOR_HEAP_TYPE type() const noexcept;

    /// @brief Command List に設定可能な Shader 可視 Heap か返す
    [[nodiscard]] bool is_shader_visible() const noexcept;

private:
    struct Slot final
    {
        std::uint64_t generation = 1;
        bool isAllocated = false;
    };

    /// @brief Mutex 保持中に Handle の所属、範囲、世代と貸出状態を確認する
    [[nodiscard]] bool is_allocated_locked(DX12DescriptorHandle a_handle) const noexcept;

    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_heap;
    std::vector<Slot> m_slots;
    std::vector<std::uint32_t> m_freeSlots;
    mutable std::mutex m_mutex;
    std::uint64_t m_allocatorId = 0;
    std::uint32_t m_nextIndex = 0;
    std::uint32_t m_incrementSize = 0;
    D3D12_DESCRIPTOR_HEAP_TYPE m_type = D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES;
    bool m_isShaderVisible = false;
};
} // namespace cue::dx12
