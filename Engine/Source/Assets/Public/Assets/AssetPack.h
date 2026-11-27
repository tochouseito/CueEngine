#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <Assets/AssetCatalog.h>
#include <Foundation/Result.h>
#include <Platform/FileSystem.h>

namespace cue
{
inline constexpr std::string_view k_gameAssetPack = "Content/Assets.cuepak";

/// @brief 1 Mip の RGBA8 Texture と、その画素を解釈する色空間を所有する
struct TexturePixels final
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    TextureColorSpace colorSpace = TextureColorSpace::Srgb;
    std::vector<std::byte> rgba;
};

/// @brief Cook で生成した DX10 Header 付き単一 Mip の RGBA8 DDS を検証して画素を所有する
[[nodiscard]] Result<TexturePixels> decode_texture_dds(std::span<const std::byte> a_bytes);

/// @brief 製品パックの索引と開いた File を所有し、必要な Texture・Model の範囲だけ読み込む
/// FileSystem は Pack より長く生存させ、同じ Pack の操作は呼出側で直列化する
class AssetPack final
{
public:
    /// @brief 索引の範囲・ID と Material・Model の参照を検証してから公開する
    [[nodiscard]] static Result<AssetPack> open(IFileSystem &a_files, const Path &a_file);

    /// @brief Pack が生存し移動するまで Material 定義を借用する
    [[nodiscard]] std::span<const MaterialAsset> materials() const noexcept;
    /// @brief Pack が生存し移動するまで Model の永続 ID と既定 Material を借用する
    [[nodiscard]] std::span<const ModelAsset> models() const noexcept;

    /// @brief 同じ File Handle の読み込み範囲を Asset ID で決め、DDS から画素を取り出す
    [[nodiscard]] Result<TexturePixels> read_texture(std::string_view a_assetId) const;
    /// @brief 同じ File Handle から指定 Model の Geometry を読み、所有値を返す
    [[nodiscard]] Result<ModelData> read_model(std::string_view a_assetId) const;

private:
    struct Entry final
    {
        std::string assetId;
        AssetKind kind;
        std::uint64_t offset;
        std::uint64_t size;
    };

    /// @brief ソート済みの索引を二分探索し、Pack の生存中だけ Entry を借用する
    [[nodiscard]] const Entry *find(std::string_view a_assetId) const noexcept;

    std::unique_ptr<IFile> m_file;
    std::vector<Entry> m_index;
    std::vector<MaterialAsset> m_materials;
    std::vector<ModelAsset> m_models;
};
} // namespace cue
