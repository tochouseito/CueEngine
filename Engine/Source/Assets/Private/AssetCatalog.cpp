#include <Assets/AssetCatalog.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <functional>
#include <new>
#include <utility>

#include <Foundation/Utf8.h>
#include <Platform/FileSystem.h>

namespace cue
{
namespace
{
constexpr std::size_t k_maxDefinitionBytes = 4096;
constexpr std::string_view k_metaExtension = ".cuemeta";
constexpr std::array<std::string_view, 4> k_textureKeys{"version", "assetId", "type", "colorSpace"};
constexpr std::array<std::string_view, 5> k_materialKeys{"version", "assetId", "type", "baseColor",
                                                       "baseColorTexture"};

/// @brief UTF-8 の内容を保ったまま設定値の周囲にある ASCII 空白を除く
std::string_view trim(std::string_view a_value) noexcept
{
    constexpr std::string_view spacing = " \t\r";
    const auto first = a_value.find_first_not_of(spacing);
    return first == std::string_view::npos ? std::string_view{}
                                          : a_value.substr(first, a_value.find_last_not_of(spacing) - first + 1);
}

/// @brief v1 設定の各項目を一度だけ読み、項目不足・未知項目・重複を上書きで隠さない
template <std::size_t Size>
Result<std::array<std::string_view, Size>> read_fields(std::string_view a_text,
                                                       const std::array<std::string_view, Size> &a_keys)
{
    using FieldsResult = Result<std::array<std::string_view, Size>>;
    if (a_text.size() > k_maxDefinitionBytes || !is_valid_utf8_text(a_text))
    {
        return FieldsResult::failure({ErrorCategory::InvalidArgument, "AssetCatalog.invalid_text"});
    }
    if (a_text.starts_with("\xef\xbb\xbf"))
    {
        a_text.remove_prefix(3);
    }
    std::array<std::string_view, Size> fields{};
    while (!a_text.empty())
    {
        const auto end = a_text.find('\n');
        const auto line = trim(a_text.substr(0, end));
        a_text = end == std::string_view::npos ? std::string_view{} : a_text.substr(end + 1);
        if (line.empty())
        {
            continue;
        }
        const auto separator = line.find('=');
        if (separator == std::string_view::npos)
        {
            return FieldsResult::failure({ErrorCategory::InvalidArgument, "AssetCatalog.invalid_line"});
        }
        const auto key = trim(line.substr(0, separator));
        const auto value = trim(line.substr(separator + 1));
        const auto found = std::find(a_keys.begin(), a_keys.end(), key);
        if (found == a_keys.end() || value.empty())
        {
            return FieldsResult::failure({ErrorCategory::InvalidArgument, "AssetCatalog.invalid_field"});
        }
        auto &field = fields[static_cast<std::size_t>(found - a_keys.begin())];
        if (!field.empty())
        {
            return FieldsResult::failure({ErrorCategory::InvalidArgument, "AssetCatalog.duplicate_field"});
        }
        field = value;
    }
    if (fields[0] != "1" || !is_valid_asset_id(fields[1]))
    {
        return FieldsResult::failure({ErrorCategory::InvalidArgument, "AssetCatalog.version_or_id"});
    }
    return FieldsResult::success(fields);
}

/// @brief UI と Cook で共通に扱う線形 RGBA を有限の 0–1 値として読む
bool read_base_color(std::string_view a_list, std::array<float, 4> &a_color) noexcept
{
    for (std::size_t index = 0; index < a_color.size(); ++index)
    {
        const auto comma = a_list.find(',');
        if ((comma == std::string_view::npos) != (index + 1 == a_color.size()))
        {
            return false;
        }
        const auto value = trim(a_list.substr(0, comma));
        if (value.empty())
        {
            return false;
        }
        const auto *end = value.data() + value.size();
        auto &component = a_color[index];
        const auto parsed = std::from_chars(value.data(), end, component, std::chars_format::general);
        if (parsed.ec != std::errc{} || parsed.ptr != end || !std::isfinite(component) || component < 0.0f ||
            component > 1.0f)
        {
            return false;
        }
        if (comma != std::string_view::npos)
        {
            a_list.remove_prefix(comma + 1);
        }
    }
    return true;
}

/// @brief ファイル拡張子だけは大文字小文字に依存せず分類する
bool has_extension(const Path &a_path, std::string_view a_extension)
{
    const auto extension = a_path.extension();
    return std::ranges::equal(extension, a_extension, [](char a_left, char a_right)
    {
        return (a_left >= 'A' && a_left <= 'Z' ? a_left - 'A' + 'a' : a_left) == a_right;
    });
}

/// @brief FileSystem API が返す生バイトを設定 Parser が借用できる文字列にする
std::string_view bytes_as_text(std::span<const std::byte> a_bytes) noexcept
{
    return a_bytes.empty() ? std::string_view{}
                           : std::string_view(reinterpret_cast<const char *>(a_bytes.data()), a_bytes.size());
}

/// @brief サイドカー名から隣接 PNG を求め、型と色空間を照合する
Result<TextureAsset> read_texture(IFileSystem &a_files, const Path &a_meta, std::string_view a_text)
{
    auto fields = read_fields(a_text, k_textureKeys);
    if (!fields.has_value())
    {
        return Result<TextureAsset>::failure(*fields.try_error());
    }
    const auto &[version, id, type, colorSpace] = *fields.try_value();
    if (type != "Texture" || (colorSpace != "sRGB" && colorSpace != "Linear"))
    {
        return Result<TextureAsset>::failure({ErrorCategory::InvalidArgument, "AssetCatalog.texture_fields"});
    }
    auto source = a_meta.parent().join(a_meta.stem());
    if (!source.has_value() || !has_extension(*source.try_value(), ".png"))
    {
        return Result<TextureAsset>::failure({ErrorCategory::InvalidArgument, "AssetCatalog.texture_source_type"});
    }
    auto info = a_files.stat(*source.try_value());
    if (!info.has_value())
    {
        return Result<TextureAsset>::failure(*info.try_error());
    }
    if (!*info.try_value() || (*info.try_value())->type != FileType::Regular)
    {
        return Result<TextureAsset>::failure({ErrorCategory::InvalidArgument, "AssetCatalog.texture_source_missing"});
    }
    return Result<TextureAsset>::success(
        {std::string(id), source.take_value(), colorSpace == "sRGB" ? TextureColorSpace::Srgb : TextureColorSpace::Linear});
}

struct CatalogEntries final
{
    std::vector<TextureAsset> textures;
    std::vector<MaterialAsset> materials;
};

/// @brief ID 順の索引を参照し、一時文字列を確保せず検索する
template <typename Asset>
const Asset *find_asset(const std::vector<Asset> &a_assets, std::string_view a_id) noexcept
{
    const auto found = std::ranges::lower_bound(a_assets, a_id, std::less<>{}, &Asset::assetId);
    return found != a_assets.end() && found->assetId == a_id ? &*found : nullptr;
}

/// @brief 同じ種別の ID を一意に保ち、読み取り専用検索向けに順序を確定する
template <typename Asset>
bool sort_unique_ids(std::vector<Asset> &a_assets)
{
    std::ranges::sort(a_assets, std::less<>{}, &Asset::assetId);
    return std::ranges::adjacent_find(a_assets, std::equal_to<>{}, &Asset::assetId) == a_assets.end();
}

/// @brief Content Tree の直下だけを FileSystem 越しに読む。Link はたどらない
Result<CatalogEntries> scan_catalog(IFileSystem &a_files, const Path &a_root)
{
    if (!a_root.is_absolute())
    {
        return Result<CatalogEntries>::failure({ErrorCategory::InvalidArgument, "AssetCatalog.relative_root"});
    }
    auto rootInfo = a_files.stat(a_root);
    if (!rootInfo.has_value())
    {
        return Result<CatalogEntries>::failure(*rootInfo.try_error());
    }
    CatalogEntries values;
    // Asset を置く前の Project からも空の索引を取得できる
    if (!*rootInfo.try_value())
    {
        return Result<CatalogEntries>::success(std::move(values));
    }
    if ((*rootInfo.try_value())->type != FileType::Directory)
    {
        return Result<CatalogEntries>::failure({ErrorCategory::InvalidArgument, "AssetCatalog.invalid_root"});
    }
    std::vector<Path> pending{a_root};
    while (!pending.empty())
    {
        auto directory = std::move(pending.back());
        pending.pop_back();
        auto entries = a_files.list_directory(directory);
        if (!entries.has_value())
        {
            return Result<CatalogEntries>::failure(*entries.try_error());
        }
        for (const auto &path : *entries.try_value())
        {
            if (path.utf8() == directory.utf8() || path.parent().utf8() != directory.utf8())
            {
                return Result<CatalogEntries>::failure({ErrorCategory::InvalidArgument, "AssetCatalog.outside_root"});
            }
            auto stat = a_files.stat(path);
            if (!stat.has_value())
            {
                return Result<CatalogEntries>::failure(*stat.try_error());
            }
            if (!*stat.try_value() || (*stat.try_value())->type == FileType::Link)
            {
                continue;
            }
            if ((*stat.try_value())->type == FileType::Directory)
            {
                pending.push_back(path);
                continue;
            }
            const bool isMeta = has_extension(path, k_metaExtension);
            if (!isMeta && !has_extension(path, ".cuemat"))
            {
                continue;
            }
            auto bytes = a_files.read_all(path, k_maxDefinitionBytes);
            if (!bytes.has_value())
            {
                return Result<CatalogEntries>::failure(*bytes.try_error());
            }
            const auto content = bytes_as_text(*bytes.try_value());
            if (isMeta)
            {
                auto texture = read_texture(a_files, path, content);
                if (!texture.has_value())
                {
                    return Result<CatalogEntries>::failure(*texture.try_error());
                }
                values.textures.push_back(texture.take_value());
            }
            else
            {
                auto material = parse_material_asset(content, path);
                if (!material.has_value())
                {
                    return Result<CatalogEntries>::failure(*material.try_error());
                }
                values.materials.push_back(material.take_value());
            }
        }
    }
    if (!sort_unique_ids(values.textures) || !sort_unique_ids(values.materials) ||
        std::ranges::any_of(values.materials, [&](const MaterialAsset &a_material)
        { return find_asset(values.textures, a_material.assetId) != nullptr; }))
    {
        return Result<CatalogEntries>::failure({ErrorCategory::InvalidArgument, "AssetCatalog.duplicate_id"});
    }
    return Result<CatalogEntries>::success(std::move(values));
}

/// @brief 新規サイドカーを同じ Directory に書き切り、上書きなしで公開する
Result<void> publish_metadata(IFileSystem &a_files, const Path &a_meta, std::string_view a_id,
                               std::string_view a_text)
{
    auto temp = a_meta.parent().join(a_meta.filename() + '.' + std::string(a_id) + ".tmp");
    if (!temp.has_value())
    {
        return Result<void>::failure(*temp.try_error());
    }
    const auto &path = *temp.try_value();
    auto opened = a_files.open(path, {FileAccess::Write, FileCreation::CreateNew, false, false, false});
    if (!opened.has_value())
    {
        return Result<void>::failure(*opened.try_error());
    }
    auto file = opened.take_value();
    const auto fail = [&](Error a_error)
    {
        file.reset();
        auto removed = a_files.remove(path);
        if (!removed.has_value())
        {
            a_error.operation += " / " + removed.try_error()->operation;
        }
        return Result<void>::failure(std::move(a_error));
    };
    auto bytes = std::as_bytes(std::span(a_text.data(), a_text.size()));
    while (!bytes.empty())
    {
        auto written = file->write(bytes);
        if (!written.has_value())
        {
            return fail(*written.try_error());
        }
        const auto size = *written.try_value();
        if (size == 0 || size > bytes.size())
        {
            return fail({ErrorCategory::PlatformFailure, "AssetCatalog.incomplete_write"});
        }
        bytes = bytes.subspan(size);
    }
    auto flushed = file->flush();
    if (!flushed.has_value())
    {
        return fail(*flushed.try_error());
    }
    auto closed = file->close();
    if (!closed.has_value())
    {
        return fail(*closed.try_error());
    }
    auto moved = a_files.rename(path, a_meta);
    return moved.has_value() ? Result<void>::success() : fail(*moved.try_error());
}
} // namespace

/// @brief UTF-8 定義から描画用の色と型付き参照を所有する
Result<MaterialAsset> parse_material_asset(std::string_view a_text, Path a_source)
{
    try
    {
        auto fields = read_fields(a_text, k_materialKeys);
        if (!fields.has_value())
        {
            return Result<MaterialAsset>::failure(*fields.try_error());
        }
        const auto &[version, id, type, baseColor, textureId] = *fields.try_value();
        MaterialAsset material;
        if (type != "Material" || !read_base_color(baseColor, material.baseColor) ||
            (!textureId.empty() && !is_valid_asset_id(textureId)))
        {
            return Result<MaterialAsset>::failure({ErrorCategory::InvalidArgument, "AssetCatalog.material_fields"});
        }
        material.assetId = id;
        material.source = std::move(a_source);
        if (!textureId.empty())
        {
            material.baseColorTexture.emplace(textureId);
        }
        return Result<MaterialAsset>::success(std::move(material));
    }
    catch (const std::bad_alloc &)
    {
        return Result<MaterialAsset>::failure({ErrorCategory::PlatformFailure, "AssetCatalog.allocation"});
    }
}

/// @brief 定義と依存を検証し、完成した索引だけを呼出元へ渡す
Result<AssetCatalog> AssetCatalog::load(IFileSystem &a_files, const Path &a_contentRoot)
{
    try
    {
        auto entries = scan_catalog(a_files, a_contentRoot);
        if (!entries.has_value())
        {
            return Result<AssetCatalog>::failure(*entries.try_error());
        }
        AssetCatalog catalog;
        catalog.m_textures = std::move(entries.try_value()->textures);
        catalog.m_materials = std::move(entries.try_value()->materials);
        for (const auto &material : catalog.m_materials)
        {
            if (material.baseColorTexture)
            {
                const auto texture = catalog.texture(*material.baseColorTexture);
                if (!texture.has_value())
                {
                    return Result<AssetCatalog>::failure(*texture.try_error());
                }
            }
        }
        return Result<AssetCatalog>::success(std::move(catalog));
    }
    catch (const std::bad_alloc &)
    {
        return Result<AssetCatalog>::failure({ErrorCategory::PlatformFailure, "AssetCatalog.allocation"});
    }
}

/// @brief Texture の ID を照会し、関連する別型の登録があれば型不一致とする
Result<const TextureAsset *> AssetCatalog::texture(std::string_view a_assetId) const
{
    if (!is_valid_asset_id(a_assetId))
    {
        return Result<const TextureAsset *>::failure({ErrorCategory::InvalidArgument, "AssetCatalog.invalid_id"});
    }
    if (const auto *asset = find_asset(m_textures, a_assetId))
    {
        return Result<const TextureAsset *>::success(asset);
    }
    return Result<const TextureAsset *>::failure(
        {ErrorCategory::InvalidArgument,
         find_asset(m_materials, a_assetId) ? "AssetCatalog.type_mismatch" : "AssetCatalog.missing_texture"});
}

/// @brief Material の ID を照会し、関連する別型の登録があれば型不一致とする
Result<const MaterialAsset *> AssetCatalog::material(std::string_view a_assetId) const
{
    if (!is_valid_asset_id(a_assetId))
    {
        return Result<const MaterialAsset *>::failure({ErrorCategory::InvalidArgument, "AssetCatalog.invalid_id"});
    }
    if (const auto *asset = find_asset(m_materials, a_assetId))
    {
        return Result<const MaterialAsset *>::success(asset);
    }
    return Result<const MaterialAsset *>::failure(
        {ErrorCategory::InvalidArgument,
         find_asset(m_textures, a_assetId) ? "AssetCatalog.type_mismatch" : "AssetCatalog.missing_material"});
}

/// @brief 索引で検証済みの Texture を ID 順に公開する
std::span<const TextureAsset> AssetCatalog::textures() const noexcept
{
    return m_textures;
}

/// @brief 索引で検証済みの Material を ID 順に公開する
std::span<const MaterialAsset> AssetCatalog::materials() const noexcept
{
    return m_materials;
}

/// @brief PNG の境界と重複 ID を検証してから新規サイドカーを公開する
Result<TextureAsset> import_texture(IFileSystem &a_files, const Path &a_contentRoot, const Path &a_source,
                                    std::string_view a_assetId, TextureColorSpace a_colorSpace)
{
    try
    {
        if (!a_contentRoot.is_absolute() || !a_source.is_absolute() || !has_extension(a_source, ".png") ||
            !is_valid_asset_id(a_assetId) ||
            (a_colorSpace != TextureColorSpace::Srgb && a_colorSpace != TextureColorSpace::Linear))
        {
            return Result<TextureAsset>::failure({ErrorCategory::InvalidArgument, "AssetCatalog.import_arguments"});
        }
        const auto &root = a_contentRoot.utf8();
        const auto prefix = root.ends_with('/') ? root : root + '/';
        if (!a_source.utf8().starts_with(prefix) || a_source.utf8().size() <= prefix.size())
        {
            return Result<TextureAsset>::failure({ErrorCategory::InvalidArgument, "AssetCatalog.outside_root"});
        }
        // ネストした Link も拒否し、明示 Import を既存の Project Content 内に収める
        for (auto path = a_source.parent();; path = path.parent())
        {
            auto info = a_files.stat(path);
            if (!info.has_value())
            {
                return Result<TextureAsset>::failure(*info.try_error());
            }
            if (!*info.try_value() || (*info.try_value())->type != FileType::Directory)
            {
                return Result<TextureAsset>::failure({ErrorCategory::InvalidArgument, "AssetCatalog.import_directory"});
            }
            if (path.utf8() == root)
            {
                break;
            }
        }
        auto sourceInfo = a_files.stat(a_source);
        if (!sourceInfo.has_value())
        {
            return Result<TextureAsset>::failure(*sourceInfo.try_error());
        }
        if (!*sourceInfo.try_value() || (*sourceInfo.try_value())->type != FileType::Regular)
        {
            return Result<TextureAsset>::failure({ErrorCategory::InvalidArgument, "AssetCatalog.texture_source_missing"});
        }

        auto entries = scan_catalog(a_files, a_contentRoot);
        if (!entries.has_value())
        {
            return Result<TextureAsset>::failure(*entries.try_error());
        }
        const auto &textures = entries.try_value()->textures;
        const auto existing = std::ranges::find_if(textures, [&](const TextureAsset &a_texture)
        { return a_texture.source.utf8() == a_source.utf8(); });
        if (existing != textures.end())
        {
            return Result<TextureAsset>::success(*existing);
        }
        if (find_asset(textures, a_assetId) || find_asset(entries.try_value()->materials, a_assetId))
        {
            return Result<TextureAsset>::failure({ErrorCategory::InvalidArgument, "AssetCatalog.duplicate_id"});
        }
        auto meta = a_source.parent().join(a_source.filename() + std::string(k_metaExtension));
        if (!meta.has_value())
        {
            return Result<TextureAsset>::failure(*meta.try_error());
        }
        const auto colorSpace = a_colorSpace == TextureColorSpace::Srgb ? "sRGB" : "Linear";
        TextureAsset asset{std::string(a_assetId), a_source, a_colorSpace};
        const auto contents = "version=1\nassetId=" + asset.assetId + "\ntype=Texture\ncolorSpace=" + colorSpace + '\n';
        auto published = publish_metadata(a_files, *meta.try_value(), a_assetId, contents);
        return published.has_value() ? Result<TextureAsset>::success(std::move(asset))
                                     : Result<TextureAsset>::failure(*published.try_error());
    }
    catch (const std::bad_alloc &)
    {
        return Result<TextureAsset>::failure({ErrorCategory::PlatformFailure, "AssetCatalog.allocation"});
    }
}
} // namespace cue
