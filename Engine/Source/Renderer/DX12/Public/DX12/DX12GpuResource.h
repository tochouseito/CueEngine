#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

#include <d3d12.h>
#include <wrl/client.h>

#include <Foundation/Result.h>
#include <RHI/GpuResource.h>

namespace cue::dx12
{
class DX12PlacedResourceAllocator;

/// @brief Committed または Placed の Buffer／Texture を一意所有する
///
/// Native Pointer は本体の生存中だけ借用できる。生成、CPU アクセス、破棄は呼出側で同期する
/// GPU 提出後は最終参照の Fence 完了まで本体を破棄しない
class DX12GpuResource final : public IGpuResource
{
    struct CreateToken final
    {
    };

public:
    /// @brief create の内部でのみ未生成状態を構築する
    explicit DX12GpuResource(CreateToken) noexcept;

    /// @brief 用途に対応する Heap と初期 State で Buffer を生成する
    ///
    /// byteSize が 0 または用途が不正なら InvalidArgument を返し、部分生成物は公開しない
    [[nodiscard]] static Result<std::unique_ptr<DX12GpuResource>> create_buffer(
        ID3D12Device& a_device, GpuBufferDesc a_desc, std::wstring_view a_name = {});

    /// @brief Default Heap に二次元 Texture を生成する
    ///
    /// 転送には Upload／Readback Buffer を使う。形状または形式が不正なら InvalidArgument を返す
    [[nodiscard]] static Result<std::unique_ptr<DX12GpuResource>> create_texture2d(
        ID3D12Device& a_device, GpuTexture2DDesc a_desc, std::wstring_view a_name = {});

    /// @brief 所有する Native Resource を解放する
    ~DX12GpuResource() override = default;

    DX12GpuResource(const DX12GpuResource&) = delete;
    DX12GpuResource& operator=(const DX12GpuResource&) = delete;

    /// @brief Resource の形状を返す
    [[nodiscard]] GpuResourceKind kind() const noexcept override;

    /// @brief Resource の Heap 用途を返す
    [[nodiscard]] GpuMemoryUsage memory_usage() const noexcept override;

    /// @brief Buffer の容量を返し、Texture では 0 を返す
    [[nodiscard]] std::uint64_t buffer_size() const noexcept;

    /// @brief Native Resource を本体の生存中だけ借用する
    [[nodiscard]] ID3D12Resource* resource() const noexcept;

    /// @brief Transient 用の Placed Resource か返す
    [[nodiscard]] bool is_placed() const noexcept;

    /// @brief Placed Resource の Heap を本体の生存中だけ借用する。Committed では nullptr
    [[nodiscard]] ID3D12Heap* placement_heap() const noexcept;

    /// @brief Placed Resource の Heap 内 Offset を返す。Committed では 0
    [[nodiscard]] std::uint64_t placement_offset() const noexcept;

    /// @brief Upload Buffer の指定範囲へ CPU Data を書き込む
    ///
    /// 範囲と用途を先に検証する。GPU が同範囲を参照中なら呼出側で同期する
    [[nodiscard]] Result<void> write(std::uint64_t a_offset, std::span<const std::byte> a_data);

    /// @brief Readback Buffer の指定範囲を CPU Data へ読み出す
    ///
    /// GPU の書き込み完了を Fence で確認してから呼ぶ。範囲と用途の失敗時は出力を変更しない
    [[nodiscard]] Result<void> read(std::uint64_t a_offset, std::span<std::byte> a_data);

private:
    friend class DX12PlacedResourceAllocator;

    /// @brief Buffer の Native 定義を検証して構築する
    [[nodiscard]] static Result<D3D12_RESOURCE_DESC> buffer_desc(GpuBufferDesc a_desc);

    /// @brief Texture の Native 定義を検証して構築する
    [[nodiscard]] static Result<D3D12_RESOURCE_DESC> texture2d_desc(GpuTexture2DDesc a_desc);

    // ComPtr より後に破棄し、Native Resource 解放後に Heap 領域を返す
    std::shared_ptr<void> m_placementLifetime;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_resource;
    ID3D12Heap* m_placementHeap = nullptr;
    std::uint64_t m_placementOffset = 0;
    GpuResourceKind m_kind = GpuResourceKind::Buffer;
    GpuMemoryUsage m_memory = GpuMemoryUsage::Default;
    std::uint64_t m_bufferSize = 0;
};
} // namespace cue::dx12
