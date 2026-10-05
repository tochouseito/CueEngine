#pragma once

#include <cstdint>
#include <memory>

#include <DX12/DX12DescriptorAllocator.h>
#include <Foundation/Result.h>

namespace cue::dx12
{
class DX12RenderDevice;

/// @brief Texture View の Descriptor Heap 用途を識別する
enum class DX12ViewType
{
    RenderTarget,
    ShaderResource,
};

/// @brief View の用途と世代付き Slot を識別する非所有 Handle
struct DX12ViewHandle final
{
    DX12DescriptorHandle descriptor;
    DX12ViewType type = DX12ViewType::RenderTarget;

    /// @brief Slot が割り当てられた Handle か判定する
    [[nodiscard]] bool is_valid() const noexcept
    {
        return descriptor.is_valid();
    }
};

/// @brief 二次元 Texture の View 範囲を指定する
struct DX12TextureViewDesc final
{
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN; // UNKNOWN は Resource の Format を使う
    std::uint32_t firstMip = 0;
    std::uint32_t mipCount = 0; // 0 は RTV では 1、SRV では残りの Mip 全体
};

/// @brief Texture View の検証、生成と Descriptor Slot 操作を集約する
///
/// 呼出側が所有し、借用する Device と Allocator より先に破棄する
/// Resource は所有せず、利用者が GPU 完了まで維持する
/// 操作は一つの制御 Thread で直列化し、write と release は対象 Slot の GPU 完了後に行う
class DX12ViewManager final
{
    struct CreateToken final
    {
    };

  public:
    /// @brief 検証済みの生成基盤を非所有で保持する
    DX12ViewManager(CreateToken, DX12RenderDevice &a_device, DX12DescriptorAllocator &a_rtv,
                    DX12DescriptorAllocator &a_srv) noexcept;

    /// @brief 同一 Device の RTV Heap と Shader 可視 SRV Heap を借用する
    ///
    /// 失敗時は Slot を割り当てず、部分生成物を公開しない
    [[nodiscard]] static Result<std::unique_ptr<DX12ViewManager>> create(DX12RenderDevice &a_device,
                                                                         DX12DescriptorAllocator &a_rtv,
                                                                         DX12DescriptorAllocator &a_srv);

    /// @brief Slot の所有者が全 View を返却してから Manager を破棄する
    ~DX12ViewManager() = default;
    /// @brief 借用依存と Slot 管理の複製を禁止する
    DX12ViewManager(const DX12ViewManager &) = delete;
    /// @brief 借用依存と Slot 管理の複製を禁止する
    DX12ViewManager &operator=(const DX12ViewManager &) = delete;

    /// @brief 外部 Resource の Binding が確定する前に Slot だけを予約する
    ///
    /// 成功後は呼出側が release する。write 前の Slot を Command から使用しない
    [[nodiscard]] Result<DX12ViewHandle> reserve(DX12ViewType a_type);

    /// @brief Resource と View 範囲を検証して RTV を生成する
    [[nodiscard]] Result<DX12ViewHandle> create_rtv(ID3D12Resource &a_resource, DX12TextureViewDesc a_desc = {});

    /// @brief Resource と View 範囲を検証して SRV を生成する
    [[nodiscard]] Result<DX12ViewHandle> create_srv(ID3D12Resource &a_resource, DX12TextureViewDesc a_desc = {});

    /// @brief Slot を変更せず Resource、Device、Format と Mip 範囲を検証する
    [[nodiscard]] Result<void> validate_texture2d(ID3D12Resource &a_resource, DX12ViewType a_type,
                                                  DX12TextureViewDesc a_desc = {}) const;

    /// @brief 有効な予約 Slot へ View を記録し、検証失敗時は既存 Descriptor を保持する
    ///
    /// D3D12 の View 生成 API は HRESULT を返さず、Native 診断は Debug Layer が担う
    [[nodiscard]] Result<void> write_texture2d(DX12ViewHandle a_handle, ID3D12Resource &a_resource,
                                               DX12TextureViewDesc a_desc = {});

    /// @brief 利用者が GPU 完了を確認した Slot を返却する
    [[nodiscard]] Result<void> release(DX12ViewHandle a_handle);

    /// @brief 所属と世代を検証して Native CPU Handle を借用する
    [[nodiscard]] Result<D3D12_CPU_DESCRIPTOR_HANDLE> cpu_handle(DX12ViewHandle a_handle) const;

    /// @brief SRV の所属と世代を検証して Native GPU Handle を借用する
    [[nodiscard]] Result<D3D12_GPU_DESCRIPTOR_HANDLE> gpu_handle(DX12ViewHandle a_handle) const;

    /// @brief SRV を Bind する Heap を Manager の利用期間中だけ借用する
    [[nodiscard]] ID3D12DescriptorHeap *srv_heap() const noexcept;

    /// @brief Context の Device と同じ生成基盤か確認する
    [[nodiscard]] ID3D12Device *device() const noexcept;

  private:
    /// @brief 用途に対応する借用 Allocator を選び、未知の用途を拒否する
    [[nodiscard]] DX12DescriptorAllocator *allocator(DX12ViewType a_type) const noexcept;

    /// @brief 検証済み範囲から Slot を割り当て、失敗時は返却する
    [[nodiscard]] Result<DX12ViewHandle> create_view(ID3D12Resource &a_resource, DX12ViewType a_type,
                                                     DX12TextureViewDesc a_desc);

    /// @brief 省略値を補い、Native View が受け付ける Texture と範囲へ限定する
    [[nodiscard]] Result<DX12TextureViewDesc> resolve_desc(ID3D12Resource &a_resource, DX12ViewType a_type,
                                                           DX12TextureViewDesc a_desc) const;

    DX12RenderDevice &m_device;
    DX12DescriptorAllocator &m_rtv;
    DX12DescriptorAllocator &m_srv;
};
} // namespace cue::dx12
