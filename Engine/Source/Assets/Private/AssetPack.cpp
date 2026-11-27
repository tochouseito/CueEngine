#include <Assets/AssetPack.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <new>
#include <optional>
#include <utility>

#include "ModelAssetFormat.h"

namespace cue
{
namespace
{
constexpr std::string_view k_packMagic = "CUEPACK1";
constexpr std::size_t k_headerSize = 16;
constexpr std::size_t k_assetIdSize = 36;
constexpr std::size_t k_entrySize = 56;
constexpr std::size_t k_ddsHeaderSize = 148;
constexpr std::uint32_t k_maxDimension = 16384;
constexpr std::uint64_t k_maxDdsSize = k_ddsHeaderSize + std::uint64_t{k_maxDimension} * k_maxDimension * 4;
constexpr std::uint32_t k_maxEntries = 65536;
constexpr std::uint64_t k_maxMaterialSize = 4096;

/// @brief Pack Writer の型番号を読み、型ごとの読み込み容量を検証する
std::optional<AssetKind> read_asset_kind(std::uint32_t a_kind, std::uint64_t a_size) noexcept
{
    switch (a_kind)
    {
    case 1:
        if (a_size > k_ddsHeaderSize && a_size <= k_maxDdsSize)
            return AssetKind::Texture;
        break;
    case 2:
        if (a_size != 0 && a_size <= k_maxMaterialSize)
            return AssetKind::Material;
        break;
    case 3:
        if (a_size > detail::k_modelHeaderSize && a_size <= detail::k_maxModelSize)
            return AssetKind::Model;
        break;
    }
    return std::nullopt;
}

/// @brief ファイルの Little Endian 整数を Native のアラインメントに依存せず復元する
template <class T>
T read_number(std::span<const std::byte> a_bytes, std::size_t a_offset) noexcept
{
    T number = 0;
    for (std::size_t index = 0; index < sizeof(T); ++index)
        number |= static_cast<T>(std::to_integer<unsigned char>(a_bytes[a_offset + index])) << (index * 8);
    return number;
}

/// @brief 部分読込を繰り返し、索引が指す全範囲の取得を確認する
Result<void> read_extent(IFile &a_file, std::uint64_t a_base, std::span<std::byte> a_target)
{
    const auto seek = a_file.seek(static_cast<std::int64_t>(a_base), SeekOrigin::Begin);
    if (!seek.has_value())
        return Result<void>::failure(*seek.try_error());
    if (*seek.try_value() != a_base)
        return Result<void>::failure({ErrorCategory::InvalidArgument, "AssetPack.seek"});
    while (!a_target.empty())
    {
        const auto progress = a_file.read(a_target);
        if (!progress.has_value())
            return Result<void>::failure(*progress.try_error());
        const auto count = *progress.try_value();
        if (count == 0 || count > a_target.size())
            return Result<void>::failure({ErrorCategory::InvalidArgument, "AssetPack.truncated"});
        a_target = a_target.subspan(count);
    }
    return Result<void>::success();
}
} // namespace

Result<TexturePixels> decode_texture_dds(std::span<const std::byte> a_bytes)
{
    using PixelsResult = Result<TexturePixels>;
    if (a_bytes.size() < k_ddsHeaderSize || std::memcmp(a_bytes.data(), "DDS ", 4) != 0 ||
        read_number<std::uint32_t>(a_bytes, 4) != 124 || read_number<std::uint32_t>(a_bytes, 76) != 32 ||
        (read_number<std::uint32_t>(a_bytes, 80) & 4u) == 0 || std::memcmp(a_bytes.data() + 84, "DX10", 4) != 0)
        return PixelsResult::failure({ErrorCategory::InvalidArgument, "TextureDds.header"});

    const auto width = read_number<std::uint32_t>(a_bytes, 16);
    const auto height = read_number<std::uint32_t>(a_bytes, 12);
    const auto format = read_number<std::uint32_t>(a_bytes, 128);
    // 28 / 29 は Cook で使う DXGI の RGBA8 UNORM / sRGB。3 は Texture2D を表す
    if (!width || !height || width > k_maxDimension || height > k_maxDimension || (format != 28 && format != 29) ||
        read_number<std::uint32_t>(a_bytes, 132) != 3 || read_number<std::uint32_t>(a_bytes, 136) != 0 ||
        read_number<std::uint32_t>(a_bytes, 140) != 1 || read_number<std::uint32_t>(a_bytes, 28) > 1 ||
        read_number<std::uint32_t>(a_bytes, 24) > 1 || read_number<std::uint32_t>(a_bytes, 112) != 0 ||
        read_number<std::uint32_t>(a_bytes, 20) != width * 4 ||
        a_bytes.size() != k_ddsHeaderSize + std::uint64_t{width} * height * 4)
        return PixelsResult::failure({ErrorCategory::InvalidArgument, "TextureDds.layout"});
    try
    {
        const auto pixels = a_bytes.subspan(k_ddsHeaderSize);
        return PixelsResult::success({width, height, format == 29 ? TextureColorSpace::Srgb : TextureColorSpace::Linear,
                                      {pixels.begin(), pixels.end()}});
    }
    catch (const std::bad_alloc &)
    {
        return PixelsResult::failure({ErrorCategory::PlatformFailure, "TextureDds.allocation"});
    }
}

Result<AssetPack> AssetPack::open(IFileSystem &a_files, const Path &a_file)
{
    using PackResult = Result<AssetPack>;
    if (!a_file.is_absolute())
        return PackResult::failure({ErrorCategory::InvalidArgument, "AssetPack.absolute_path"});
    try
    {
        auto opened = a_files.open(a_file);
        if (!opened.has_value())
            return PackResult::failure(*opened.try_error());
        AssetPack pack;
        pack.m_file = opened.take_value();
        const auto length = pack.m_file->size();
        if (!length.has_value())
            return PackResult::failure(*length.try_error());
        const auto fileSize = *length.try_value();
        if (fileSize < k_headerSize || fileSize > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()))
            return PackResult::failure({ErrorCategory::InvalidArgument, "AssetPack.size"});

        std::array<std::byte, k_headerSize> header{};
        const auto headerRead = read_extent(*pack.m_file, 0, header);
        if (!headerRead.has_value())
            return PackResult::failure(*headerRead.try_error());
        const auto count = read_number<std::uint32_t>(header, 8);
        if (std::memcmp(header.data(), k_packMagic.data(), k_packMagic.size()) != 0 ||
            read_number<std::uint32_t>(header, 12) != 0 || count > k_maxEntries)
            return PackResult::failure({ErrorCategory::InvalidArgument, "AssetPack.header"});
        const auto indexSize = static_cast<std::size_t>(count) * k_entrySize;
        std::uint64_t nextOffset = k_headerSize + indexSize;
        if (nextOffset > fileSize)
            return PackResult::failure({ErrorCategory::InvalidArgument, "AssetPack.index_range"});
        std::vector<std::byte> index(indexSize);
        const auto indexRead = read_extent(*pack.m_file, k_headerSize, index);
        if (!indexRead.has_value())
            return PackResult::failure(*indexRead.try_error());

        pack.m_index.reserve(count);
        for (std::uint32_t row = 0; row < count; ++row)
        {
            const auto record = std::span<const std::byte>{index}.subspan(row * k_entrySize, k_entrySize);
            const std::string_view assetId(reinterpret_cast<const char *>(record.data()), k_assetIdSize);
            const auto offset = read_number<std::uint64_t>(record, 40);
            const auto size = read_number<std::uint64_t>(record, 48);
            const auto kind = read_asset_kind(read_number<std::uint32_t>(record, 36), size);
            // Pack Writer は ID 順に格納する。重複・範囲の重なり・未知の型・不正な容量を拒否する
            if (!is_valid_asset_id(assetId) || (!pack.m_index.empty() && pack.m_index.back().assetId >= assetId) ||
                !kind || offset != nextOffset || size > fileSize - nextOffset)
                return PackResult::failure({ErrorCategory::InvalidArgument, "AssetPack.entry"});
            pack.m_index.push_back({std::string(assetId), *kind, offset, size});
            nextOffset += size;
        }
        if (nextOffset != fileSize)
            return PackResult::failure({ErrorCategory::InvalidArgument, "AssetPack.trailing_data"});

        for (const auto &entry : pack.m_index)
        {
            if (entry.kind == AssetKind::Model)
            {
                std::array<std::byte, detail::k_modelHeaderSize> modelHeader{};
                auto read = read_extent(*pack.m_file, entry.offset, modelHeader);
                if (!read.has_value())
                    return PackResult::failure(*read.try_error());
                auto model = detail::decode_model_header(modelHeader, entry.size);
                if (!model.has_value())
                    return PackResult::failure(*model.try_error());
                if (model.try_value()->assetId != entry.assetId)
                    return PackResult::failure({ErrorCategory::InvalidArgument, "AssetPack.model_id"});
                if (const auto &materialId = model.try_value()->materialAssetId)
                {
                    const auto *material = pack.find(*materialId);
                    if (!material || material->kind != AssetKind::Material)
                        return PackResult::failure({ErrorCategory::InvalidArgument, "AssetPack.model_material"});
                }
                pack.m_models.push_back(model.take_value());
                continue;
            }
            if (entry.kind != AssetKind::Material)
                continue;
            std::string text(static_cast<std::size_t>(entry.size), '\0');
            auto read = read_extent(*pack.m_file, entry.offset, std::as_writable_bytes(std::span{text}));
            if (!read.has_value())
                return PackResult::failure(*read.try_error());
            auto material = parse_material_asset(text);
            if (!material.has_value())
                return PackResult::failure(*material.try_error());
            if (material.try_value()->assetId != entry.assetId)
                return PackResult::failure({ErrorCategory::InvalidArgument, "AssetPack.material_id"});
            if (const auto &textureId = material.try_value()->baseColorTexture)
            {
                const auto *texture = pack.find(*textureId);
                if (!texture || texture->kind != AssetKind::Texture)
                    return PackResult::failure({ErrorCategory::InvalidArgument, "AssetPack.material_texture"});
            }
            pack.m_materials.push_back(material.take_value());
        }
        return PackResult::success(std::move(pack));
    }
    catch (const std::bad_alloc &)
    {
        return PackResult::failure({ErrorCategory::PlatformFailure, "AssetPack.allocation"});
    }
}

std::span<const MaterialAsset> AssetPack::materials() const noexcept
{
    return m_materials;
}

std::span<const ModelAsset> AssetPack::models() const noexcept
{
    return m_models;
}

const AssetPack::Entry *AssetPack::find(std::string_view a_assetId) const noexcept
{
    const auto found = std::ranges::lower_bound(m_index, a_assetId, {}, &Entry::assetId);
    return found == m_index.end() || found->assetId != a_assetId ? nullptr : &*found;
}

Result<TexturePixels> AssetPack::read_texture(std::string_view a_assetId) const
{
    using PixelsResult = Result<TexturePixels>;
    const auto *found = find(a_assetId);
    if (!found || found->kind != AssetKind::Texture || !m_file)
        return PixelsResult::failure({ErrorCategory::InvalidArgument, "AssetPack.texture_id"});
    try
    {
        std::vector<std::byte> encoded(static_cast<std::size_t>(found->size));
        const auto read = read_extent(*m_file, found->offset, encoded);
        return read.has_value() ? decode_texture_dds(encoded) : PixelsResult::failure(*read.try_error());
    }
    catch (const std::bad_alloc &)
    {
        return PixelsResult::failure({ErrorCategory::PlatformFailure, "AssetPack.texture_allocation"});
    }
}

Result<ModelData> AssetPack::read_model(std::string_view a_assetId) const
{
    using R = Result<ModelData>;
    const auto *found = find(a_assetId);
    if (!found || found->kind != AssetKind::Model || !m_file)
        return R::failure({ErrorCategory::InvalidArgument, "AssetPack.model_id"});
    try
    {
        std::vector<std::byte> encoded(static_cast<std::size_t>(found->size));
        const auto read = read_extent(*m_file, found->offset, encoded);
        if (!read.has_value())
            return R::failure(*read.try_error());
        auto model = decode_model_asset(encoded);
        if (model.has_value() && model.try_value()->asset.assetId != a_assetId)
            return R::failure({ErrorCategory::InvalidArgument, "AssetPack.model_id"});
        return model;
    }
    catch (const std::bad_alloc &)
    {
        return R::failure({ErrorCategory::PlatformFailure, "AssetPack.model_allocation"});
    }
}
} // namespace cue
