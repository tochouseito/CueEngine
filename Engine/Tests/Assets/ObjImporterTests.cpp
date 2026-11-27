#include <ModelImport/ObjImporter.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>

#include <Assets/AssetCatalog.h>
#include <Assets/ModelAsset.h>

#include "../Support/TemporaryFiles.h"

namespace
{
#define CHECK(...)                                                                                                     \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(__VA_ARGS__))                                                                                            \
        {                                                                                                              \
            std::fprintf(stderr, "OBJ importer test failed at %d: %s\n", __LINE__, #__VA_ARGS__);                       \
            return 1;                                                                                                  \
        }                                                                                                              \
    } while (false)

/// @brief 検証用の OBJ と依存を一時 Project だけに作成する
bool write_text(cue::IFileSystem &a_files, const cue::Path &a_root, std::string_view a_name, std::string_view a_text)
{
    auto path = a_root.join(a_name);
    return path.has_value() && a_files.write_all(*path.try_value(), std::as_bytes(std::span(a_text)), true).has_value();
}
} // namespace

/// @brief OBJ の UV 境界と依存参照を検証し、再 Import と不正な依存の拒否を確認する
int main()
{
    cue::tests::TemporaryFiles fixture;
    CHECK(fixture.isCreated && fixture.files);
    auto &files = *fixture.files;
    auto root = fixture.root.join("Content");
    CHECK(root.has_value());
    const auto &content = *root.try_value();
    constexpr auto k_vertices = "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\n";
    const std::string obj = std::string("mtllib Mats/Model.mtl\n") + k_vertices +
                            "vt 0.25 0.25\nvt 0.75 0.25\nvt 0.75 0.75\nvt 0 0\nvt 0.25 0.75\n"
                            "usemtl Surface\nf 1/1 2/2 3/3\nf 1/4 3/3 4/5\n";
    constexpr auto k_mtl = "newmtl Surface\nKd 0.5 0.25 1\nmap_Kd ../../Textures/UV.png\n";
    CHECK(write_text(files, content, "Models/Model.obj", obj));
    CHECK(write_text(files, content, "Models/Mats/Model.mtl", k_mtl));
    // Importer は元画像の場所と ID を解決する。画素の展開は描画側が担当する
    CHECK(write_text(files, content, "Textures/UV.png", "Texture bytes fixture"));
    auto objPath = content.join("Models/Model.obj");
    CHECK(objPath.has_value());

    constexpr std::array<std::string_view, 3> ids = {"67dc366c-de02-4f19-820e-e929fba54a0f",
                                                     "d35cc395-7476-480e-9f8b-914fecebf70f",
                                                     "7248a69e-ced0-4d74-9195-ee0ded44ec95"};
    std::size_t nextId = 0;
    const auto generate = [&]() -> cue::Result<std::string>
    {
        if (nextId >= ids.size())
            return cue::Result<std::string>::failure({cue::ErrorCategory::InvalidState, "test.id_exhausted"});
        return cue::Result<std::string>::success(std::string(ids[nextId++]));
    };
    auto imported = cue::import_obj_model(files, content, *objPath.try_value(), generate);
    if (!imported.has_value())
        std::fprintf(stderr, "%s\n", imported.try_error()->operation.c_str());
    CHECK(imported.has_value() && imported.try_value()->meshId == ids[0] && imported.try_value()->materialId == ids[1]);
    auto catalog = cue::AssetCatalog::load(files, content);
    CHECK(catalog.has_value() && catalog.try_value()->models().size() == 1 &&
          catalog.try_value()->textures().size() == 1);
    const auto &registered = catalog.try_value()->models().front();
    auto loaded = cue::read_model_asset(files, registered.source);
    CHECK(loaded.has_value() && registered.materialAssetId == ids[1]);
    const auto &mesh = loaded.try_value()->mesh;
    CHECK(mesh.index_count() == 6 && mesh.positions().size() == 5 && mesh.normals().size() == mesh.uv0().size());
    CHECK(std::ranges::all_of(mesh.normals(), [](const auto &a_normal) { return a_normal.z == 1.0f; }));
    CHECK(std::ranges::any_of(mesh.uv0(), [](const auto &a_uv) { return a_uv.x == 0.25f && a_uv.y == 0.75f; }));
    CHECK(std::ranges::any_of(mesh.uv0(), [](const auto &a_uv) { return a_uv.x == 0.0f && a_uv.y == 1.0f; }));
    const auto &material = catalog.try_value()->materials().front();
    CHECK(material.baseColor[0] == 0.5f && material.baseColorTexture == ids[2]);
    CHECK(write_text(files, content, "Models/Mats/Model.mtl",
                     "newmtl Surface\nKd 1 0.25 0.5\nmap_Kd ../../Textures/UV.png\n"));
    auto repeated = cue::import_obj_model(files, content, *objPath.try_value(), generate);
    CHECK(repeated.has_value() && repeated.try_value()->meshId == ids[0] && repeated.try_value()->materialId == ids[1]);
    auto updated = cue::AssetCatalog::load(files, content);
    CHECK(updated.has_value() && updated.try_value()->materials().front().baseColor[0] == 1.0f &&
          updated.try_value()->materials().front().baseColorTexture == ids[2]);

    // Content の外への参照や拡張子違いでモデルを生成しない
    CHECK(write_text(files, fixture.root, "Outside.mtl", k_mtl));
    CHECK(write_text(files, content, "Models/Outside.obj",
                     std::string("mtllib ../../Outside.mtl\n") + k_vertices + "f 1 2 3\n"));
    CHECK(write_text(files, content, "Models/Model.fbx", obj));
    auto outsidePath = content.join("Models/Outside.obj");
    auto unsupportedPath = content.join("Models/Model.fbx");
    CHECK(outsidePath.has_value() && unsupportedPath.has_value());
    auto outside = cue::import_obj_model(files, content, *outsidePath.try_value(), generate);
    CHECK(!outside.has_value() && outside.try_error()->operation.starts_with("ObjImporter.outside_content"));
    auto unsupported = cue::import_obj_model(files, content, *unsupportedPath.try_value(), generate);
    CHECK(!unsupported.has_value() && unsupported.try_error()->operation == "ObjImporter.source_arguments");
    auto after = cue::AssetCatalog::load(files, content);
    CHECK(after.has_value() && after.try_value()->models().size() == 1);
    return 0;
}
