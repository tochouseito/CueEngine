#include <Assets/ModelAsset.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <limits>
#include <new>
#include <type_traits>
#include <utility>

#include <Foundation/Uuid.h>
#include <Platform/FileSystem.h>

#include "ModelAssetFormat.h"

namespace cue
{
namespace
{
constexpr std::string_view k_magic = "CUEMODL1";
constexpr std::size_t k_idSize = 36;
constexpr std::size_t k_modelIdOffset = 8;
constexpr std::size_t k_materialIdOffset = 44;
constexpr std::size_t k_vertexCountOffset = 80;
constexpr std::size_t k_indexCountOffset = 84;
constexpr std::size_t k_attributesOffset = 88;
constexpr std::size_t k_indexWidthOffset = 92;
constexpr std::uint32_t k_normal = 1;
constexpr std::uint32_t k_uv0 = 2;
static_assert(sizeof(float) == sizeof(std::uint32_t) && std::numeric_limits<float>::is_iec559);

/// @brief Native の Alignment と Byte Order に依存せず整数を取得する
std::uint32_t read_number(std::span<const std::byte> a_bytes, std::size_t a_offset, std::size_t a_width = 4) noexcept
{
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < a_width; ++index)
        value |= std::uint32_t{std::to_integer<unsigned char>(a_bytes[a_offset + index])} << (index * 8);
    return value;
}

/// @brief 整数を指定の要素幅で Little Endian に追加する
void append_number(std::vector<std::byte> &a_bytes, std::uint32_t a_number, std::size_t a_width = 4)
{
    for (std::size_t index = 0; index < a_width; ++index)
        a_bytes.push_back(static_cast<std::byte>((a_number >> (index * 8)) & 0xffu));
}

/// @brief 文字列を終端文字を加えず Header へ追加する
void append_text(std::vector<std::byte> &a_bytes, std::string_view a_text)
{
    const auto bytes = std::as_bytes(std::span(a_text));
    a_bytes.insert(a_bytes.end(), bytes.begin(), bytes.end());
}

/// @brief 32 bit 項目の積を 64 bit で計算し、全 Layout の必要量を返す
std::uint64_t model_size(std::uint32_t a_vertices, std::uint32_t a_indices, std::uint32_t a_attributes,
                         std::uint32_t a_indexWidth) noexcept
{
    const auto vertexWidth = 12u + ((a_attributes & k_normal) ? 12u : 0u) + ((a_attributes & k_uv0) ? 8u : 0u);
    return detail::k_modelHeaderSize + std::uint64_t{a_vertices} * vertexWidth +
           std::uint64_t{a_indices} * a_indexWidth;
}
} // namespace

namespace detail
{
Result<ModelAsset> decode_model_header(std::span<const std::byte> a_header, std::uint64_t a_fileSize, Path a_source)
{
    using R = Result<ModelAsset>;
    if (a_header.size() < k_modelHeaderSize || a_fileSize > k_maxModelSize ||
        std::memcmp(a_header.data(), k_magic.data(), k_magic.size()) != 0)
        return R::failure({ErrorCategory::InvalidArgument, "ModelAsset.header"});

    const std::string_view assetId(reinterpret_cast<const char *>(a_header.data() + k_modelIdOffset), k_idSize);
    const std::string_view materialId(reinterpret_cast<const char *>(a_header.data() + k_materialIdOffset), k_idSize);
    const bool hasMaterial = !std::ranges::all_of(materialId, [](char a_char) { return a_char == '\0'; });
    const auto vertices = read_number(a_header, k_vertexCountOffset);
    const auto indices = read_number(a_header, k_indexCountOffset);
    const auto attributes = read_number(a_header, k_attributesOffset);
    const auto indexWidth = read_number(a_header, k_indexWidthOffset);
    if (!is_valid_uuid_text(assetId) || (hasMaterial && (!is_valid_uuid_text(materialId) || materialId == assetId)) ||
        vertices == 0 || indices == 0 || indices % 3 != 0 || (attributes & ~(k_normal | k_uv0)) != 0 ||
        (indexWidth != 2 && indexWidth != 4) || a_fileSize != model_size(vertices, indices, attributes, indexWidth))
        return R::failure({ErrorCategory::InvalidArgument, "ModelAsset.layout"});

    try
    {
        ModelAsset asset{std::string(assetId), std::move(a_source), std::nullopt};
        if (hasMaterial)
            asset.materialAssetId.emplace(materialId);
        return R::success(std::move(asset));
    }
    catch (const std::bad_alloc &)
    {
        return R::failure({ErrorCategory::PlatformFailure, "ModelAsset.allocation"});
    }
}

Result<ModelAsset> read_model_header(IFileSystem &a_files, const Path &a_source)
{
    using R = Result<ModelAsset>;
    auto opened = a_files.open(a_source);
    if (!opened.has_value())
        return R::failure(*opened.try_error());
    auto file = opened.take_value();
    auto size = file->size();
    if (!size.has_value())
        return R::failure(*size.try_error());
    std::array<std::byte, k_modelHeaderSize> header{};
    std::span<std::byte> pending(header);
    while (!pending.empty())
    {
        auto read = file->read(pending);
        if (!read.has_value())
            return R::failure(*read.try_error());
        if (*read.try_value() == 0 || *read.try_value() > pending.size())
            return R::failure({ErrorCategory::InvalidArgument, "ModelAsset.truncated_header"});
        pending = pending.subspan(*read.try_value());
    }
    return decode_model_header(header, *size.try_value(), a_source);
}
} // namespace detail

Result<std::vector<std::byte>> encode_model_asset(std::string_view a_assetId,
                                                  std::optional<std::string_view> a_materialAssetId,
                                                  const MeshData &a_mesh)
{
    using R = Result<std::vector<std::byte>>;
    if (!is_valid_uuid_text(a_assetId) ||
        (a_materialAssetId && (!is_valid_uuid_text(*a_materialAssetId) || *a_materialAssetId == a_assetId)))
        return R::failure({ErrorCategory::InvalidArgument, "ModelAsset.id"});

    const auto vertices = static_cast<std::uint32_t>(a_mesh.positions().size());
    const auto indices = a_mesh.index_count();
    const auto attributes = (a_mesh.normals().empty() ? 0u : k_normal) | (a_mesh.uv0().empty() ? 0u : k_uv0);
    const auto indexWidth = a_mesh.index_format() == MeshIndexFormat::Uint16 ? 2u : 4u;
    const auto size = model_size(vertices, indices, attributes, indexWidth);
    if (!vertices || !indices || size > detail::k_maxModelSize)
        return R::failure({ErrorCategory::InvalidArgument, "ModelAsset.size"});

    try
    {
        std::vector<std::byte> bytes;
        bytes.reserve(static_cast<std::size_t>(size));
        append_text(bytes, k_magic);
        append_text(bytes, a_assetId);
        if (a_materialAssetId)
            append_text(bytes, *a_materialAssetId);
        else
            bytes.resize(k_materialIdOffset + k_idSize);
        append_number(bytes, vertices);
        append_number(bytes, indices);
        append_number(bytes, attributes);
        append_number(bytes, indexWidth);
        const auto addFloat = [&](float a_value) { append_number(bytes, std::bit_cast<std::uint32_t>(a_value)); };
        const auto addVector3 = [&](const math::float3 &a_value)
        {
            addFloat(a_value.x);
            addFloat(a_value.y);
            addFloat(a_value.z);
        };
        std::ranges::for_each(a_mesh.positions(), addVector3);
        std::ranges::for_each(a_mesh.normals(), addVector3);
        for (const auto &uv : a_mesh.uv0())
        {
            addFloat(uv.x);
            addFloat(uv.y);
        }
        for (const auto index : a_mesh.indices16())
            append_number(bytes, index, 2);
        for (const auto index : a_mesh.indices32())
            append_number(bytes, index);
        return R::success(std::move(bytes));
    }
    catch (const std::bad_alloc &)
    {
        return R::failure({ErrorCategory::PlatformFailure, "ModelAsset.allocation"});
    }
}

Result<ModelData> decode_model_asset(std::span<const std::byte> a_bytes, Path a_source)
{
    using R = Result<ModelData>;
    auto asset = detail::decode_model_header(a_bytes, a_bytes.size(), std::move(a_source));
    if (!asset.has_value())
        return R::failure(*asset.try_error());
    try
    {
        const auto vertices = read_number(a_bytes, k_vertexCountOffset);
        const auto indices = read_number(a_bytes, k_indexCountOffset);
        const auto attributes = read_number(a_bytes, k_attributesOffset);
        const auto indexWidth = read_number(a_bytes, k_indexWidthOffset);
        std::size_t offset = detail::k_modelHeaderSize;
        const auto takeFloat = [&]
        {
            const auto value = read_number(a_bytes, offset);
            offset += 4;
            return std::bit_cast<float>(value);
        };
        const auto readVectors = [&](std::vector<math::float3> &a_values)
        {
            a_values.resize(vertices);
            for (auto &value : a_values)
                value = {takeFloat(), takeFloat(), takeFloat()};
        };
        MeshDescriptor descriptor;
        readVectors(descriptor.positions);
        if ((attributes & k_normal) != 0)
            readVectors(descriptor.normals);
        if ((attributes & k_uv0) != 0)
        {
            descriptor.uv0.resize(vertices);
            for (auto &value : descriptor.uv0)
                value = {takeFloat(), takeFloat()};
        }
        if (indexWidth == 4)
            descriptor.indices.emplace<std::vector<std::uint32_t>>();
        std::visit([&](auto &a_values)
        {
            a_values.resize(indices);
            for (auto &value : a_values)
            {
                value = static_cast<std::remove_reference_t<decltype(value)>>(read_number(a_bytes, offset, indexWidth));
                offset += indexWidth;
            }
        }, descriptor.indices);
        auto mesh = MeshData::create(std::move(descriptor));
        if (!mesh.has_value())
            return R::failure(*mesh.try_error());
        return R::success({asset.take_value(), mesh.take_value()});
    }
    catch (const std::bad_alloc &)
    {
        return R::failure({ErrorCategory::PlatformFailure, "ModelAsset.allocation"});
    }
}

Result<ModelData> read_model_asset(IFileSystem &a_files, const Path &a_source)
{
    try
    {
        auto bytes = a_files.read_all(a_source, detail::k_maxModelSize);
        return bytes.has_value() ? decode_model_asset(*bytes.try_value(), a_source)
                                 : Result<ModelData>::failure(*bytes.try_error());
    }
    catch (const std::bad_alloc &)
    {
        return Result<ModelData>::failure({ErrorCategory::PlatformFailure, "ModelAsset.allocation"});
    }
}
} // namespace cue
