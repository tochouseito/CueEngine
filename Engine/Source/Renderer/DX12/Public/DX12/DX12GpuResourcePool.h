#pragma once

#include <memory>

#include <Foundation/Result.h>
#include <RHI/GpuResourcePool.h>

namespace cue::dx12
{
class DX12RenderDevice;
class DX12GpuResource;
class DX12PlacedResourceAllocator;

/// @brief DX12 Resource の Slot と GPU 完了点を共有状態で所有する
///
/// 生成、借用、回収、停止は複数 Thread から呼べる。Lease と Native Resource の利用は呼出側で同期する
class DX12GpuResourcePool final : public IGpuResourcePool
{
    struct CreateToken final
    {
    };

    struct State;
    class Lease;

public:
    /// @brief create の内部でのみ未生成状態を構築する
    explicit DX12GpuResourcePool(CreateToken) noexcept;

    /// @brief Device を保持する空の Pool を作る
    [[nodiscard]] static Result<std::unique_ptr<DX12GpuResourcePool>> create(DX12RenderDevice& a_device);

    /// @brief 未停止の Pool を停止し、残る Lease に Resource の寿命を引き継ぐ
    ~DX12GpuResourcePool() override;

    DX12GpuResourcePool(const DX12GpuResourcePool&) = delete;
    DX12GpuResourcePool& operator=(const DX12GpuResourcePool&) = delete;

    /// @brief Buffer を生成し、失敗時は Slot 状態を変更しない
    [[nodiscard]] Result<GpuResourceHandle> create_buffer(GpuBufferDesc a_desc) override;

    /// @brief Texture を生成し、失敗時は Slot 状態を変更しない
    [[nodiscard]] Result<GpuResourceHandle> create_texture2d(GpuTexture2DDesc a_desc) override;

    /// @brief Default Buffer を Placed Heap の占有領域に生成する
    [[nodiscard]] Result<GpuResourceHandle> create_transient_buffer(GpuBufferDesc a_desc) override;

    /// @brief Texture を Placed Heap の占有領域に生成する
    [[nodiscard]] Result<GpuResourceHandle> create_transient_texture2d(GpuTexture2DDesc a_desc) override;

    /// @brief 現世代の Resource を競合しない Access で借りる
    [[nodiscard]] Result<gpuResourceLease> acquire(GpuResourceHandle a_handle, GpuResourceAccess a_access) override;

    /// @brief 古い Handle を拒否し、有効な Resource の破棄を予約する
    [[nodiscard]] Result<void> retire(GpuResourceHandle a_handle) override;

    /// @brief GPU 完了済みの破棄予約を解放する
    [[nodiscard]] Result<std::size_t> collect() override;

    /// @brief 未返却 Lease がなければ全 Completion を待って Resource を解放する
    [[nodiscard]] Result<void> shutdown() override;

private:
    /// @brief 生成済み Resource を空き Slot に収めて Handle を作る
    [[nodiscard]] Result<GpuResourceHandle> insert_resource(std::unique_ptr<DX12GpuResource> a_resource);

    std::shared_ptr<State> m_state;
    std::unique_ptr<DX12PlacedResourceAllocator> m_placedAllocator;
};
} // namespace cue::dx12
