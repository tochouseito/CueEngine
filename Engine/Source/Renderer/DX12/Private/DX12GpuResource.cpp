#include <DX12/DX12GpuResource.h>

#include <bit>
#include <cstring>
#include <memory>
#include <string>

#include <Platform/Diagnostics.h>

namespace cue::dx12
{
namespace
{
/// @brief HRESULT を Native Code を持つ Error に変換する
Error resource_error(const char* a_operation, HRESULT a_result)
{
    return {ErrorCategory::PlatformFailure, a_operation, static_cast<std::int64_t>(a_result)};
}

/// @brief RHI の画素形式を Native 形式へ変換する
DXGI_FORMAT native_format(GpuTextureFormat a_format) noexcept
{
    switch (a_format)
    {
    case GpuTextureFormat::Rgba8Unorm:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case GpuTextureFormat::Bgra8Unorm:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case GpuTextureFormat::Rgba16Float:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case GpuTextureFormat::R32Float:
        return DXGI_FORMAT_R32_FLOAT;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}

/// @brief 任意の Debug 名を設定し、失敗を診断へ残す
void set_resource_name(ID3D12Resource& a_resource, std::wstring_view a_name, const wchar_t* a_fallback)
{
    const std::wstring name = a_name.empty() ? std::wstring(a_fallback) : std::wstring(a_name);
    const HRESULT result = a_resource.SetName(name.c_str());
    if (FAILED(result))
    {
        report_error("DX12GpuResource", resource_error("ID3D12Resource.SetName", result),
                     DiagnosticSeverity::Warning);
    }
}
} // namespace

/// @brief create の内部でのみ未生成状態を構築する
DX12GpuResource::DX12GpuResource(CreateToken) noexcept
{
}

/// @brief Buffer の容量と Heap 用途を検証して Native 定義を返す
Result<D3D12_RESOURCE_DESC> DX12GpuResource::buffer_desc(GpuBufferDesc a_desc)
{
    if (a_desc.byteSize == 0 || (a_desc.memory != GpuMemoryUsage::Default &&
                                 a_desc.memory != GpuMemoryUsage::Upload &&
                                 a_desc.memory != GpuMemoryUsage::Readback))
    {
        return Result<D3D12_RESOURCE_DESC>::failure({ErrorCategory::InvalidArgument,
                                                     "DX12GpuResource.buffer_desc"});
    }
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = a_desc.byteSize;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return Result<D3D12_RESOURCE_DESC>::success(desc);
}

/// @brief 二次元 Texture の形状と形式を検証して Native 定義を返す
Result<D3D12_RESOURCE_DESC> DX12GpuResource::texture2d_desc(GpuTexture2DDesc a_desc)
{
    const DXGI_FORMAT format = native_format(a_desc.format);
    const std::uint32_t maxDimension = a_desc.width > a_desc.height ? a_desc.width : a_desc.height;
    if (a_desc.width == 0 || a_desc.height == 0 ||
        a_desc.width > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        a_desc.height > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        a_desc.mipLevels == 0 || a_desc.mipLevels > std::bit_width(maxDimension) ||
        format == DXGI_FORMAT_UNKNOWN)
    {
        return Result<D3D12_RESOURCE_DESC>::failure({ErrorCategory::InvalidArgument,
                                                     "DX12GpuResource.texture2d_desc"});
    }
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = a_desc.width;
    desc.Height = a_desc.height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = a_desc.mipLevels;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    return Result<D3D12_RESOURCE_DESC>::success(desc);
}

/// @brief Buffer の Heap 用途に固定された State で Committed Resource を作る
Result<std::unique_ptr<DX12GpuResource>> DX12GpuResource::create_buffer(
    ID3D12Device& a_device, GpuBufferDesc a_desc, std::wstring_view a_name)
{
    using ResourceResult = Result<std::unique_ptr<DX12GpuResource>>;
    auto descResult = buffer_desc(a_desc);
    if (!descResult.has_value())
    {
        return ResourceResult::failure(*descResult.try_error());
    }

    D3D12_HEAP_PROPERTIES heap{};
    D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_COMMON;
    switch (a_desc.memory)
    {
    case GpuMemoryUsage::Default:
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        break;
    case GpuMemoryUsage::Upload:
        heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        initialState = D3D12_RESOURCE_STATE_GENERIC_READ;
        break;
    case GpuMemoryUsage::Readback:
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        initialState = D3D12_RESOURCE_STATE_COPY_DEST;
        break;
    default:
        return ResourceResult::failure({ErrorCategory::InvalidArgument, "DX12GpuResource.create_buffer"});
    }

    auto resource = std::make_unique<DX12GpuResource>(CreateToken{});
    const HRESULT result = a_device.CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, descResult.try_value(),
                                                             initialState, nullptr,
                                                             IID_PPV_ARGS(&resource->m_resource));
    if (FAILED(result))
    {
        return ResourceResult::failure(resource_error("ID3D12Device.CreateCommittedResource.Buffer", result));
    }
    resource->m_kind = GpuResourceKind::Buffer;
    resource->m_memory = a_desc.memory;
    resource->m_bufferSize = a_desc.byteSize;
    set_resource_name(*resource->m_resource.Get(), a_name, L"CueEngine DX12 Buffer");
    return ResourceResult::success(std::move(resource));
}

/// @brief Default Heap に Texture を作り転送前の COMMON State で公開する
Result<std::unique_ptr<DX12GpuResource>> DX12GpuResource::create_texture2d(
    ID3D12Device& a_device, GpuTexture2DDesc a_desc, std::wstring_view a_name)
{
    using ResourceResult = Result<std::unique_ptr<DX12GpuResource>>;
    auto descResult = texture2d_desc(a_desc);
    if (!descResult.has_value())
    {
        return ResourceResult::failure(*descResult.try_error());
    }

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    auto resource = std::make_unique<DX12GpuResource>(CreateToken{});
    const HRESULT result = a_device.CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, descResult.try_value(),
                                                             D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                             IID_PPV_ARGS(&resource->m_resource));
    if (FAILED(result))
    {
        return ResourceResult::failure(resource_error("ID3D12Device.CreateCommittedResource.Texture2D", result));
    }
    resource->m_kind = GpuResourceKind::Texture2D;
    resource->m_memory = GpuMemoryUsage::Default;
    set_resource_name(*resource->m_resource.Get(), a_name, L"CueEngine DX12 Texture2D");
    return ResourceResult::success(std::move(resource));
}

/// @brief 作成時に固定した形状を返す
GpuResourceKind DX12GpuResource::kind() const noexcept
{
    return m_kind;
}

/// @brief 作成時に固定した Heap 用途を返す
GpuMemoryUsage DX12GpuResource::memory_usage() const noexcept
{
    return m_memory;
}

/// @brief Buffer の有効容量を返す
std::uint64_t DX12GpuResource::buffer_size() const noexcept
{
    return m_bufferSize;
}

/// @brief Native Resource を所有権を移さず返す
ID3D12Resource* DX12GpuResource::resource() const noexcept
{
    return m_resource.Get();
}

/// @brief Placed Heap から借りた領域を持つか返す
bool DX12GpuResource::is_placed() const noexcept
{
    return m_placementLifetime != nullptr;
}

/// @brief Placed Resource の Heap を非所有で返す
ID3D12Heap* DX12GpuResource::placement_heap() const noexcept
{
    return m_placementHeap;
}

/// @brief Placed Resource の配置 Offset を返す
std::uint64_t DX12GpuResource::placement_offset() const noexcept
{
    return m_placementOffset;
}

/// @brief 指定した Upload Buffer の範囲だけを Map して書き込む
Result<void> DX12GpuResource::write(std::uint64_t a_offset, std::span<const std::byte> a_data)
{
    if (m_kind != GpuResourceKind::Buffer || m_memory != GpuMemoryUsage::Upload ||
        a_offset > m_bufferSize || a_data.size() > m_bufferSize - a_offset)
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "DX12GpuResource.write"});
    }
    if (a_data.empty())
    {
        return Result<void>::success();
    }
    void* mapped = nullptr;
    const D3D12_RANGE noRead{0, 0};
    const HRESULT result = m_resource->Map(0, &noRead, &mapped);
    if (FAILED(result))
    {
        return Result<void>::failure(resource_error("ID3D12Resource.Map.Upload", result));
    }
    std::memcpy(static_cast<std::byte*>(mapped) + a_offset, a_data.data(), a_data.size());
    const D3D12_RANGE written{static_cast<SIZE_T>(a_offset), static_cast<SIZE_T>(a_offset + a_data.size())};
    m_resource->Unmap(0, &written);
    return Result<void>::success();
}

/// @brief 指定した Readback Buffer の範囲だけを Map して読み出す
Result<void> DX12GpuResource::read(std::uint64_t a_offset, std::span<std::byte> a_data)
{
    if (m_kind != GpuResourceKind::Buffer || m_memory != GpuMemoryUsage::Readback ||
        a_offset > m_bufferSize || a_data.size() > m_bufferSize - a_offset)
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "DX12GpuResource.read"});
    }
    if (a_data.empty())
    {
        return Result<void>::success();
    }
    void* mapped = nullptr;
    const D3D12_RANGE readRange{static_cast<SIZE_T>(a_offset), static_cast<SIZE_T>(a_offset + a_data.size())};
    const HRESULT result = m_resource->Map(0, &readRange, &mapped);
    if (FAILED(result))
    {
        return Result<void>::failure(resource_error("ID3D12Resource.Map.Readback", result));
    }
    std::memcpy(a_data.data(), static_cast<const std::byte*>(mapped) + a_offset, a_data.size());
    const D3D12_RANGE noWrite{0, 0};
    m_resource->Unmap(0, &noWrite);
    return Result<void>::success();
}
} // namespace cue::dx12
