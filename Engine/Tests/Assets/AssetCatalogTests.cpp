#include <Assets/AssetCatalog.h>

#include <cstdio>
#include <span>
#include <string>
#include <string_view>

#include "../Support/TemporaryFiles.h"

namespace
{
#define CHECK(...)                                                                                                     \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(__VA_ARGS__))                                                                                            \
        {                                                                                                              \
            std::fprintf(stderr, "AssetCatalog test failed at %d: %s\n", __LINE__, #__VA_ARGS__);                         \
            return 1;                                                                                                  \
        }                                                                                                              \
    } while (false)

constexpr std::string_view k_textureId = "67dc366c-de02-4f19-820e-e929fba54a0f";
constexpr std::string_view k_materialId = "d35cc395-7476-480e-9f8b-914fecebf70f";
constexpr std::string_view k_otherId = "7248a69e-ced0-4d74-9195-ee0ded44ec95";

/// @brief 索引検証用のファイルを一時 Content だけに作成する
bool write(cue::IFileSystem &a_files, const cue::Path &a_root, std::string_view a_name, std::string_view a_content)
{
    auto path = a_root.join(a_name);
    return path.has_value() && a_files.write_all(*path.try_value(), std::as_bytes(std::span(a_content)), true).has_value();
}

/// @brief 検証対象以外を固定し、Material の参照先だけを切り換える
std::string material(std::string_view a_textureId)
{
    return "version=1\nassetId=" + std::string(k_materialId) +
           "\ntype=Material\nbaseColor=0.5,0.25,1,1\nbaseColorTexture=" + std::string(a_textureId) + '\n';
}
} // namespace

/// @brief 再 Import、移動・改名、重複・欠落・型不一致を実 FileSystem と同じ手順で確認する
int main()
{
    cue::tests::TemporaryFiles fixture;
    CHECK(fixture.files && fixture.isCreated);
    auto &files = *fixture.files;
    auto content = fixture.root.join("Content");
    CHECK(content.has_value());
    const auto &root = *content.try_value();
    auto empty = cue::AssetCatalog::load(files, root);
    CHECK(empty.has_value() && empty.try_value()->textures().empty() && empty.try_value()->materials().empty());

    // Catalog は PNG の配置と型を検証する。デコードは Cook 側の責務
    CHECK(write(files, root, "Textures/UV.png", "source fixture"));
    auto source = root.join("Textures/UV.png");
    auto sidecar = root.join("Textures/UV.png.cuemeta");
    CHECK(source.has_value() && sidecar.has_value());
    auto imported = cue::import_texture(files, root, *source.try_value(), k_textureId);
    CHECK(imported.has_value() && imported.try_value()->assetId == k_textureId);
    auto repeated = cue::import_texture(files, root, *source.try_value(), k_otherId, cue::TextureColorSpace::Linear);
    CHECK(repeated.has_value() && repeated.try_value()->assetId == k_textureId &&
          repeated.try_value()->colorSpace == cue::TextureColorSpace::Srgb);
    CHECK(write(files, root, "Materials/Test.cuemat", material(k_textureId)));

    auto loaded = cue::AssetCatalog::load(files, root);
    CHECK(loaded.has_value() && loaded.try_value()->textures().size() == 1 && loaded.try_value()->materials().size() == 1);
    auto selected = loaded.try_value()->material(k_materialId);
    CHECK(selected.has_value() && (*selected.try_value())->baseColor[0] == 0.5f &&
          (*selected.try_value())->baseColorTexture == k_textureId);
    auto wrongType = loaded.try_value()->texture(k_materialId);
    CHECK(!wrongType.has_value() && wrongType.try_error()->operation == "AssetCatalog.type_mismatch");

    auto relocated = root.join("Renamed");
    auto newSource = root.join("Renamed/チェッカー.PNG");
    auto newSidecar = root.join("Renamed/チェッカー.PNG.cuemeta");
    CHECK(relocated.has_value() && newSource.has_value() && newSidecar.has_value());
    CHECK(files.create_directories(*relocated.try_value()).has_value());
    CHECK(files.rename(*source.try_value(), *newSource.try_value()).has_value());
    CHECK(files.rename(*sidecar.try_value(), *newSidecar.try_value()).has_value());
    auto moved = cue::AssetCatalog::load(files, root);
    CHECK(moved.has_value());
    auto texture = moved.try_value()->texture(k_textureId);
    CHECK(texture.has_value() && (*texture.try_value())->source.utf8() == newSource.try_value()->utf8());

    CHECK(write(files, root, "Materials/Test.cuemat", material(k_otherId)));
    auto missingReference = cue::AssetCatalog::load(files, root);
    CHECK(!missingReference.has_value() && missingReference.try_error()->operation == "AssetCatalog.missing_texture");
    CHECK(write(files, root, "Materials/Test.cuemat", material(k_materialId)));
    auto invalidReference = cue::AssetCatalog::load(files, root);
    CHECK(!invalidReference.has_value() && invalidReference.try_error()->operation == "AssetCatalog.type_mismatch");
    CHECK(write(files, root, "Materials/Test.cuemat", material(k_textureId)));

    CHECK(write(files, root, "Other.png", "other source fixture"));
    auto other = root.join("Other.png");
    CHECK(other.has_value());
    auto duplicateImport = cue::import_texture(files, root, *other.try_value(), k_textureId);
    CHECK(!duplicateImport.has_value() && duplicateImport.try_error()->operation == "AssetCatalog.duplicate_id");
    CHECK(cue::import_texture(files, root, *other.try_value(), k_otherId).has_value());
    CHECK(write(files, root, "Other.png.cuemeta", "version=1\nassetId=" + std::string(k_textureId) +
                                                   "\ntype=Texture\ncolorSpace=Linear\n"));
    auto duplicate = cue::AssetCatalog::load(files, root);
    CHECK(!duplicate.has_value() && duplicate.try_error()->operation == "AssetCatalog.duplicate_id");
    CHECK(files.remove(*newSource.try_value()).has_value());
    auto orphan = cue::AssetCatalog::load(files, root);
    CHECK(!orphan.has_value() && orphan.try_error()->operation == "AssetCatalog.texture_source_missing");
    return 0;
}
