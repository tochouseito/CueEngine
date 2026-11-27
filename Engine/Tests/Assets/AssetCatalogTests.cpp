#include <Assets/AssetCatalog.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <Assets/AssetPack.h>
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
constexpr std::string_view k_modelId = "f149fddd-d27b-4fd4-9ebe-48a220f7ff40";

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

/// @brief 製品索引の Little Endian 整数を Fixture へ書き込む
void append(std::vector<std::byte> &a_bytes, std::uint64_t a_value, std::size_t a_width)
{
    for (std::size_t index = 0; index < a_width; ++index)
        a_bytes.push_back(static_cast<std::byte>((a_value >> (index * 8)) & 0xffu));
}

/// @brief Magic・永続 ID・Material 本文をパックに終端文字なしで連結する
void append(std::vector<std::byte> &a_bytes, std::string_view a_text)
{
    const auto bytes = std::as_bytes(std::span(a_text));
    a_bytes.insert(a_bytes.end(), bytes.begin(), bytes.end());
}

/// @brief Model の参照と全 Geometry を Content と製品パックの両方から読み直す
int test_model_asset()
{
    cue::tests::TemporaryFiles fixture;
    CHECK(fixture.files && fixture.isCreated);
    auto &files = *fixture.files;
    auto path = fixture.root.join("Fixture.cuemodel");
    auto packPath = fixture.root.join("Assets.cuepak");
    CHECK(path.has_value() && packPath.has_value());
    cue::MeshDescriptor triangle;
    triangle.positions = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    triangle.normals = {{0, 0, 1}, {0, 0, 1}, {0, 0, 1}};
    triangle.uv0 = {{0, 0}, {1, 0}, {0, 1}};
    triangle.indices = std::vector<std::uint16_t>{0, 1, 2};
    auto mesh = cue::MeshData::create(std::move(triangle));
    CHECK(mesh.has_value());
    auto encoded = cue::encode_model_asset(k_modelId, k_materialId, *mesh.try_value());
    CHECK(encoded.has_value() && encoded.try_value()->size() == 198);
    CHECK(!cue::encode_model_asset(k_modelId, k_modelId, *mesh.try_value()).has_value());
    auto malformed = *encoded.try_value();
    malformed.pop_back();
    CHECK(!cue::decode_model_asset(malformed).has_value());
    malformed = *encoded.try_value();
    malformed.back() = std::byte{0xff};
    CHECK(!cue::decode_model_asset(malformed).has_value());
    CHECK(files.write_all(*path.try_value(), *encoded.try_value()).has_value());
    const auto materialText = "version=1\nassetId=" + std::string(k_materialId) +
                              "\ntype=Material\nbaseColor=1,1,1,1\n";
    auto missing = cue::AssetCatalog::load(files, fixture.root);
    CHECK(!missing.has_value() && missing.try_error()->operation == "AssetCatalog.missing_material");
    CHECK(write(files, fixture.root, "Fixture.cuemat", materialText));
    auto catalog = cue::AssetCatalog::load(files, fixture.root);
    CHECK(catalog.has_value() && catalog.try_value()->models().size() == 1);
    auto found = catalog.try_value()->model(k_modelId);
    CHECK(found.has_value() && (*found.try_value())->source.utf8() == path.try_value()->utf8() &&
          (*found.try_value())->materialAssetId == k_materialId);
    auto wrongType = catalog.try_value()->material(k_modelId);
    CHECK(!wrongType.has_value() && wrongType.try_error()->operation == "AssetCatalog.type_mismatch");
    auto decoded = cue::read_model_asset(files, (*found.try_value())->source);
    CHECK(decoded.has_value() && decoded.try_value()->mesh.uv0().size() == 3 &&
          decoded.try_value()->mesh.uv0()[2].y == 1.0f);

    // Cook の順序と参照を再現し、異なる Payload を扱う同じ Pack に Model を置く
    std::vector<std::byte> pack;
    append(pack, "CUEPACK1");
    append(pack, 2, 4);
    append(pack, 0, 4);
    constexpr auto payloadOffset = 16 + 2 * 56;
    append(pack, k_materialId);
    append(pack, 2, 4);
    append(pack, payloadOffset, 8);
    append(pack, materialText.size(), 8);
    append(pack, k_modelId);
    append(pack, 3, 4);
    append(pack, payloadOffset + materialText.size(), 8);
    append(pack, encoded.try_value()->size(), 8);
    append(pack, materialText);
    pack.insert(pack.end(), encoded.try_value()->begin(), encoded.try_value()->end());
    CHECK(files.write_all(*packPath.try_value(), pack).has_value());
    auto opened = cue::AssetPack::open(files, *packPath.try_value());
    CHECK(opened.has_value() && opened.try_value()->models().size() == 1 &&
          opened.try_value()->models().front().assetId == k_modelId);
    CHECK(!opened.try_value()->read_model(k_materialId).has_value());
    auto loaded = opened.try_value()->read_model(k_modelId);
    CHECK(loaded.has_value() && loaded.try_value()->asset.materialAssetId == k_materialId &&
          loaded.try_value()->mesh.positions()[1].x == 1.0f &&
          std::ranges::equal(loaded.try_value()->mesh.indices16(), mesh.try_value()->indices16()));
    return 0;
}
} // namespace

/// @brief 再 Import、移動・改名、重複・欠落・型不一致を実 FileSystem と同じ手順で確認する
int main()
{
    if (const int result = test_model_asset(); result != 0)
        return result;

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
