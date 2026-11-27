#pragma once

#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <Assets/ModelAsset.h>
#include <Foundation/Path.h>
#include <Foundation/Result.h>
#include <Foundation/Uuid.h>

namespace cue
{
class IFileSystem;

enum class AssetKind
{
    Texture,
    Material,
    Model
};

enum class TextureColorSpace
{
    Srgb,
    Linear
};

/// @brief 同じ永続 ID をプロジェクト索引と製品パック索引に使う
[[nodiscard]] inline bool is_valid_asset_id(std::string_view a_text) noexcept
{
    return is_valid_uuid_text(a_text);
}

/// @brief 隣接する .cuemeta から解決した PNG の所有情報
struct TextureAsset final
{
    std::string assetId;
    Path source;
    TextureColorSpace colorSpace = TextureColorSpace::Srgb;
};

/// @brief .cuemat の Material 定義と参照テクスチャの永続 ID
struct MaterialAsset final
{
    std::string assetId;
    Path source;
    std::array<float, 4> baseColor{1.0f, 1.0f, 1.0f, 1.0f};
    std::optional<std::string> baseColorTexture;
};

/// @brief UTF-8 の Material v1 を検証して所有値に変換する。Texture の参照整合は索引で検証する
[[nodiscard]] Result<MaterialAsset> parse_material_asset(std::string_view a_text, Path a_source = {});

/// @brief Content 内の Texture／Material／Model を同じ永続 ID 空間で管理する読み取り専用索引
/// 参照・Span は索引の破棄、Move、代入まで借用する。公開後の読み取りは並行実行できる
class AssetCatalog final
{
  public:
    /// @brief 絶対 Content Root を走査し、ID の重複と Material・Model の参照を検証する
    /// 外部 Link をたどらず、FileSystem は呼出中だけ借用する。失敗時に部分索引を公開しない
    [[nodiscard]] static Result<AssetCatalog> load(IFileSystem &a_files, const Path &a_contentRoot);

    /// @brief 種類を照合して Texture を借用する。無効 ID・欠落・型違いを区別する
    [[nodiscard]] Result<const TextureAsset *> texture(std::string_view a_assetId) const;
    /// @brief 種類を照合して Material を借用する。無効 ID・欠落・型違いを区別する
    [[nodiscard]] Result<const MaterialAsset *> material(std::string_view a_assetId) const;
    /// @brief 種類を照合して Model を借用する。無効 ID・欠落・型違いを区別する
    [[nodiscard]] Result<const ModelAsset *> model(std::string_view a_assetId) const;
    /// @brief ID 順の Texture 一覧を借用する
    [[nodiscard]] std::span<const TextureAsset> textures() const noexcept;
    /// @brief ID 順の Material 一覧を借用する
    [[nodiscard]] std::span<const MaterialAsset> materials() const noexcept;
    /// @brief ID 順の Model 一覧を借用する
    [[nodiscard]] std::span<const ModelAsset> models() const noexcept;

  private:
    /// @brief 検証が完了するまで空の索引を外部へ公開しない
    AssetCatalog() = default;

    std::vector<TextureAsset> m_textures;
    std::vector<MaterialAsset> m_materials;
    std::vector<ModelAsset> m_models;
};

/// @brief Content 内の既存 PNG を登録する。再 Import では既存 ID と色空間を再利用する
/// Root と PNG は絶対 Path で指定し、FileSystem は呼出中だけ借用する
/// 新規 Metadata は書込み成功後に公開する。既存 Metadata を置換しない
/// 同じ Content の書込み操作は呼出側で直列化する
[[nodiscard]] Result<TextureAsset> import_texture(IFileSystem &a_files, const Path &a_contentRoot,
                                                   const Path &a_source, std::string_view a_assetId,
                                                   TextureColorSpace a_colorSpace = TextureColorSpace::Srgb);
} // namespace cue
