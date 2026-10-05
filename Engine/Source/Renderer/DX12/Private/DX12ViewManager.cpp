#include <DX12/DX12ViewManager.h>

#include <memory>
#include <new>
#include <utility>

#include <wrl/client.h>

#include <DX12/DX12RenderDevice.h>

namespace cue::dx12
{
/// @brief Native Device と Heap は Owner の管理下で借用する
DX12ViewManager::DX12ViewManager(CreateToken, DX12RenderDevice &a_device, DX12DescriptorAllocator &a_rtv,
                                 DX12DescriptorAllocator &a_srv) noexcept
    : m_device(a_device), m_rtv(a_rtv), m_srv(a_srv)
{
}

/// @brief Heap の用途と所属 Device が揃った場合だけ Manager を公開する
Result<std::unique_ptr<DX12ViewManager>> DX12ViewManager::create(DX12RenderDevice &a_device,
                                                                 DX12DescriptorAllocator &a_rtv,
                                                                 DX12DescriptorAllocator &a_srv)
{
    using ManagerResult = Result<std::unique_ptr<DX12ViewManager>>;
    Microsoft::WRL::ComPtr<ID3D12Device> rtvDevice;
    Microsoft::WRL::ComPtr<ID3D12Device> srvDevice;
    if (!a_device.device() || !a_rtv.heap() || !a_srv.heap() || a_rtv.type() != D3D12_DESCRIPTOR_HEAP_TYPE_RTV ||
        a_srv.type() != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV || !a_srv.is_shader_visible() ||
        FAILED(a_rtv.heap()->GetDevice(IID_PPV_ARGS(&rtvDevice))) ||
        FAILED(a_srv.heap()->GetDevice(IID_PPV_ARGS(&srvDevice))) || rtvDevice.Get() != a_device.device() ||
        srvDevice.Get() != a_device.device())
    {
        return ManagerResult::failure({ErrorCategory::InvalidArgument, "DX12ViewManager.create"});
    }
    try
    {
        return ManagerResult::success(std::make_unique<DX12ViewManager>(CreateToken{}, a_device, a_rtv, a_srv));
    }
    catch (const std::bad_alloc &)
    {
        return ManagerResult::failure({ErrorCategory::PlatformFailure, "DX12ViewManager.create.allocation"});
    }
}

/// @brief 用途に対応する Heap だけを選ぶ
DX12DescriptorAllocator *DX12ViewManager::allocator(DX12ViewType a_type) const noexcept
{
    switch (a_type)
    {
    case DX12ViewType::RenderTarget:
        return &m_rtv;
    case DX12ViewType::ShaderResource:
        return &m_srv;
    }
    return nullptr;
}

/// @brief Native API 呼出前に Resource と View の全前提を検証する
Result<DX12TextureViewDesc> DX12ViewManager::resolve_desc(ID3D12Resource &a_resource, DX12ViewType a_type,
                                                          DX12TextureViewDesc a_desc) const
{
    using DescResult = Result<DX12TextureViewDesc>;
    const auto native = a_resource.GetDesc();
    Microsoft::WRL::ComPtr<ID3D12Device> resourceDevice;
    if (!allocator(a_type) || !device() || native.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        native.DepthOrArraySize != 1 || native.SampleDesc.Count != 1 || native.MipLevels == 0 ||
        a_desc.firstMip >= native.MipLevels || FAILED(a_resource.GetDevice(IID_PPV_ARGS(&resourceDevice))) ||
        resourceDevice.Get() != device() ||
        (a_type == DX12ViewType::RenderTarget && (native.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) == 0) ||
        (a_type == DX12ViewType::ShaderResource && (native.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) != 0))
    {
        return DescResult::failure({ErrorCategory::InvalidArgument, "DX12ViewManager.texture.resource"});
    }
    if (a_desc.format == DXGI_FORMAT_UNKNOWN)
    {
        a_desc.format = native.Format;
    }
    if (a_desc.mipCount == 0)
    {
        a_desc.mipCount = a_type == DX12ViewType::RenderTarget ? 1 : native.MipLevels - a_desc.firstMip;
    }
    if (a_desc.format != native.Format || a_desc.mipCount > native.MipLevels - a_desc.firstMip ||
        (a_type == DX12ViewType::RenderTarget && a_desc.mipCount != 1))
    {
        return DescResult::failure({ErrorCategory::InvalidArgument, "DX12ViewManager.texture.range"});
    }
    // 現行 RHI の Color Texture Format に限定し、Typeless／Depth の解釈は追加時に定義する
    switch (a_desc.format)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R32_FLOAT:
        return DescResult::success(a_desc);
    default:
        return DescResult::failure({ErrorCategory::InvalidArgument, "DX12ViewManager.texture.format"});
    }
}

/// @brief Resource の Binding が決まるまで Native Descriptor を書き込まない
Result<DX12ViewHandle> DX12ViewManager::reserve(DX12ViewType a_type)
{
    auto *target = allocator(a_type);
    if (!target)
    {
        return Result<DX12ViewHandle>::failure({ErrorCategory::InvalidArgument, "DX12ViewManager.reserve.type"});
    }
    auto result = target->allocate();
    if (!result.has_value())
    {
        return Result<DX12ViewHandle>::failure(*result.try_error());
    }
    return Result<DX12ViewHandle>::success({result.take_value(), a_type});
}

/// @brief RenderTarget 用 Slot を生成する
Result<DX12ViewHandle> DX12ViewManager::create_rtv(ID3D12Resource &a_resource, DX12TextureViewDesc a_desc)
{
    return create_view(a_resource, DX12ViewType::RenderTarget, a_desc);
}

/// @brief ShaderRead 用 Slot を生成する
Result<DX12ViewHandle> DX12ViewManager::create_srv(ID3D12Resource &a_resource, DX12TextureViewDesc a_desc)
{
    return create_view(a_resource, DX12ViewType::ShaderResource, a_desc);
}

/// @brief 検証と Slot 確保が完了した場合だけ View を公開する
Result<DX12ViewHandle> DX12ViewManager::create_view(ID3D12Resource &a_resource, DX12ViewType a_type,
                                                    DX12TextureViewDesc a_desc)
{
    auto validation = validate_texture2d(a_resource, a_type, a_desc);
    if (!validation.has_value())
    {
        return Result<DX12ViewHandle>::failure(*validation.try_error());
    }
    auto result = reserve(a_type);
    if (!result.has_value())
    {
        return result;
    }
    const auto handle = result.take_value();
    auto write = write_texture2d(handle, a_resource, a_desc);
    if (!write.has_value())
    {
        auto rollback = release(handle);
        return Result<DX12ViewHandle>::failure(rollback.has_value() ? *write.try_error() : *rollback.try_error());
    }
    return Result<DX12ViewHandle>::success(handle);
}

/// @brief Descriptor を書き込まず生成前の一括検証にも使う
Result<void> DX12ViewManager::validate_texture2d(ID3D12Resource &a_resource, DX12ViewType a_type,
                                                 DX12TextureViewDesc a_desc) const
{
    auto result = resolve_desc(a_resource, a_type, a_desc);
    return result.has_value() ? Result<void>::success() : Result<void>::failure(*result.try_error());
}

/// @brief 世代と Resource を検証してから対象 Slot だけを書き換える
Result<void> DX12ViewManager::write_texture2d(DX12ViewHandle a_handle, ID3D12Resource &a_resource,
                                              DX12TextureViewDesc a_desc)
{
    auto result = resolve_desc(a_resource, a_handle.type, a_desc);
    if (!result.has_value())
    {
        return Result<void>::failure(*result.try_error());
    }
    auto cpu = cpu_handle(a_handle);
    if (!cpu.has_value())
    {
        return Result<void>::failure(*cpu.try_error());
    }
    const auto desc = result.take_value();
    if (a_handle.type == DX12ViewType::RenderTarget)
    {
        D3D12_RENDER_TARGET_VIEW_DESC native{};
        native.Format = desc.format;
        native.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        native.Texture2D.MipSlice = desc.firstMip;
        device()->CreateRenderTargetView(&a_resource, &native, *cpu.try_value());
    }
    else
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC native{};
        native.Format = desc.format;
        native.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        native.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        native.Texture2D.MostDetailedMip = desc.firstMip;
        native.Texture2D.MipLevels = desc.mipCount;
        device()->CreateShaderResourceView(&a_resource, &native, *cpu.try_value());
    }
    return Result<void>::success();
}

/// @brief GPU 完了の確認は呼出側が行い、Slot の所属と世代を検証して返す
Result<void> DX12ViewManager::release(DX12ViewHandle a_handle)
{
    auto *target = allocator(a_handle.type);
    return target ? target->release(a_handle.descriptor)
                  : Result<void>::failure({ErrorCategory::InvalidArgument, "DX12ViewManager.release.type"});
}

/// @brief View の用途に対応する CPU Handle を検証して返す
Result<D3D12_CPU_DESCRIPTOR_HANDLE> DX12ViewManager::cpu_handle(DX12ViewHandle a_handle) const
{
    auto *target = allocator(a_handle.type);
    return target ? target->cpu_handle(a_handle.descriptor)
                  : Result<D3D12_CPU_DESCRIPTOR_HANDLE>::failure(
                        {ErrorCategory::InvalidArgument, "DX12ViewManager.cpu.type"});
}

/// @brief RTV には GPU Handle を公開しない
Result<D3D12_GPU_DESCRIPTOR_HANDLE> DX12ViewManager::gpu_handle(DX12ViewHandle a_handle) const
{
    return a_handle.type == DX12ViewType::ShaderResource
               ? m_srv.gpu_handle(a_handle.descriptor)
               : Result<D3D12_GPU_DESCRIPTOR_HANDLE>::failure(
                     {ErrorCategory::InvalidArgument, "DX12ViewManager.gpu.type"});
}

/// @brief Slot の生成と同じ Shader 可視 Heap を貸す
ID3D12DescriptorHeap *DX12ViewManager::srv_heap() const noexcept
{
    return m_srv.heap();
}

/// @brief Manager の生成基盤を Context の検証に使う
ID3D12Device *DX12ViewManager::device() const noexcept
{
    return m_device.device();
}
} // namespace cue::dx12
