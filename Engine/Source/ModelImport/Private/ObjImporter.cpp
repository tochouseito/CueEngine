#include <ModelImport/ObjImporter.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <assimp/Importer.hpp>
#include <assimp/MemoryIOWrapper.h>
#include <assimp/material.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <Assets/AssetCatalog.h>
#include <Assets/ModelAsset.h>
#include <Platform/FileSystem.h>

namespace cue
{
namespace
{
constexpr std::size_t k_maxObjBytes = 64 * 1024 * 1024;
constexpr std::size_t k_maxMtlBytes = 1024 * 1024;
constexpr std::size_t k_maxMaterialBytes = 4096;
constexpr std::size_t k_maxVertices = 1'000'000;
constexpr std::size_t k_maxIndices = 3'000'000;

/// @brief Content の拡張子は Windows 上でも ASCII の大文字小文字を区別しない
bool has_extension(const Path &a_path, std::string_view a_extension)
{
    return std::ranges::equal(a_path.extension(), a_extension, [](char a_actual, char a_expected)
    {
        return a_actual == a_expected ||
               (a_actual >= 'A' && a_actual <= 'Z' && a_actual - 'A' + 'a' == a_expected);
    });
}

/// @brief 正規化済み Path を Content 内の実ファイルに限定し、親 Link も拒否する
Result<FileInfo> inspect_source(IFileSystem &a_files, const Path &a_root, const Path &a_path)
{
    const auto &root = a_root.utf8();
    const auto prefix = root.ends_with('/') ? root : root + '/';
    const auto &path = a_path.utf8();
    if (!a_root.is_absolute() || !a_path.is_absolute() || !path.starts_with(prefix) || path.size() == prefix.size() ||
        path.find(':', prefix.size()) != std::string::npos)
    {
        return Result<FileInfo>::failure({ErrorCategory::InvalidArgument, "ObjImporter.outside_content:" + path});
    }
    for (auto parent = a_path.parent();; parent = parent.parent())
    {
        auto info = a_files.stat(parent);
        if (!info.has_value())
            return Result<FileInfo>::failure(*info.try_error());
        const auto &value = *info.try_value();
        if (!value || value->type != FileType::Directory)
            return Result<FileInfo>::failure(
                {ErrorCategory::InvalidArgument, "ObjImporter.directory:" + parent.utf8()});
        if (parent.utf8() == root)
            break;
    }
    auto found = a_files.stat(a_path);
    if (!found.has_value())
        return Result<FileInfo>::failure(*found.try_error());
    const auto &info = *found.try_value();
    if (!info || info->type != FileType::Regular)
        return Result<FileInfo>::failure({ErrorCategory::InvalidArgument, "ObjImporter.missing_source:" + path});
    return Result<FileInfo>::success(*info);
}

/// @brief Assimp が Stream を閉じるまで、上限付きで読んだファイルの Bytes を保持する
class ObjInput final : public Assimp::MemoryIOStream
{
  public:
    /// @brief Buffer の所有権を移し、基底 Stream には移動後も同じ位置を借りさせる
    explicit ObjInput(std::vector<std::byte> a_bytes)
        : Assimp::MemoryIOStream(reinterpret_cast<const std::uint8_t *>(a_bytes.data()), a_bytes.size()),
          m_bytes(std::move(a_bytes))
    {
    }

  private:
    std::vector<std::byte> m_bytes;
};

/// @brief Assimp に入力 OBJ と Content 内の MTL だけを公開する読み取り専用 FileSystem
class ObjIo final : public Assimp::IOSystem
{
  public:
    /// @brief Importer の寿命中だけ Project FileSystem と Path を借りる
    ObjIo(IFileSystem &a_files, const Path &a_root, const Path &a_source)
        : m_files(a_files), m_root(a_root), m_source(a_source)
    {
    }

    /// @brief Assimp の拡張子判定で参照される Path が読み込み対象か調べる
    bool Exists(const char *a_path) const override
    {
        auto path = resolve(a_path);
        return path.has_value() && inspect_source(m_files, m_root, *path.try_value()).has_value();
    }

    /// @brief 正規化した Project Path と同じ区切りを Assimp に返す
    char getOsSeparator() const override
    {
        return '/';
    }

    /// @brief OBJ / MTL を読み取る。Assimp が Material の欠落を復帰扱いにしても Error を保持する
    Assimp::IOStream *Open(const char *a_path, const char *a_mode) override
    {
        if (!a_mode || (std::string_view(a_mode) != "rb" && std::string_view(a_mode) != "r" &&
                        std::string_view(a_mode) != "rt"))
            return nullptr;
        auto resolved = resolve(a_path);
        if (!resolved.has_value())
            return fail(*resolved.try_error());
        const auto &path = *resolved.try_value();
        auto info = inspect_source(m_files, m_root, path);
        if (!info.has_value())
            return fail(*info.try_error());
        const bool isMaterial = path.utf8() != m_source.utf8();
        if (isMaterial && m_materialPath && m_materialPath->utf8() != path.utf8())
            return fail({ErrorCategory::InvalidArgument, "ObjImporter.multiple_material_libraries"});
        auto bytes = m_files.read_all(path, isMaterial ? k_maxMtlBytes : k_maxObjBytes);
        if (!bytes.has_value())
            return fail(*bytes.try_error());
        auto stream = std::make_unique<ObjInput>(bytes.take_value());
        if (isMaterial)
            m_materialPath = path;
        return stream.release();
    }

    /// @brief Assimp API に渡した Stream の所有権を受け取り解放する
    void Close(Assimp::IOStream *a_stream) override
    {
        const std::unique_ptr<Assimp::IOStream> owned(a_stream);
    }

    /// @brief Assimp が開いた MTL と最初の FileSystem エラーを Importer の破棄まで借りる
    [[nodiscard]] const std::optional<Path> &material_path() const noexcept
    {
        return m_materialPath;
    }
    /// @brief Assimp の既定 Material へのフォールバックで失われる参照エラーを返す
    [[nodiscard]] const std::optional<Error> &error() const noexcept
    {
        return m_error;
    }

  private:
    /// @brief 相対 Path は OBJ の Directory を基準として解決する
    Result<Path> resolve(const char *a_value) const
    {
        if (!a_value)
            return Result<Path>::failure({ErrorCategory::InvalidArgument, "ObjImporter.input_path"});
        auto path = m_source.parent().join(a_value);
        if (path.has_value() && path.try_value()->utf8() != m_source.utf8() &&
            !has_extension(*path.try_value(), ".mtl"))
        {
            return Result<Path>::failure({ErrorCategory::InvalidArgument, "ObjImporter.input_type:" +
                                                                             path.try_value()->utf8()});
        }
        return path;
    }

    /// @brief 参照先の不足を診断に残し Stream の生成を拒否する
    Assimp::IOStream *fail(Error a_error)
    {
        if (!m_error)
            m_error = std::move(a_error);
        return nullptr;
    }

    IFileSystem &m_files;
    const Path &m_root;
    const Path &m_source;
    std::optional<Path> m_materialPath;
    std::optional<Error> m_error;
};

/// @brief Engine が所有する Geometry と Model の単一 Material を Importer の寿命から切り離す
struct ObjGeometry final
{
    MeshData mesh;
    std::array<float, 4> baseColor;
    std::optional<Path> png;
};

/// @brief Assimp が MTL の相対 Directory を付けた画像参照を OBJ 側で解決する
Result<std::optional<Path>> resolve_texture(IFileSystem &a_files, const Path &a_root, const Path &a_obj,
                                           const ObjIo &a_io, const aiMaterial &a_material)
{
    using TextureResult = Result<std::optional<Path>>;
    aiString name;
    if (a_material.GetTexture(aiTextureType_DIFFUSE, 0, &name) != AI_SUCCESS)
        return TextureResult::success(std::nullopt);
    if (!a_io.material_path())
        return TextureResult::failure({ErrorCategory::InvalidArgument, "ObjImporter.texture_without_mtl"});
    auto relative = Path::create(name.C_Str());
    if (!relative.has_value() || relative.try_value()->is_absolute() || !has_extension(*relative.try_value(), ".png"))
        return TextureResult::failure({ErrorCategory::InvalidArgument, "ObjImporter.relative_png_required"});
    auto joined = a_obj.parent().join(*relative.try_value());
    if (!joined.has_value())
        return TextureResult::failure(*joined.try_error());
    auto info = inspect_source(a_files, a_root, *joined.try_value());
    if (!info.has_value())
        return TextureResult::failure(*info.try_error());
    if (info.try_value()->size == 0 || info.try_value()->size > k_maxObjBytes)
        return TextureResult::failure({ErrorCategory::InvalidArgument, "ObjImporter.texture_size"});
    return TextureResult::success(joined.take_value());
}

/// @brief 1 Material の OBJ Group を同じ Mesh に連結し、UV の継ぎ目を保持する
Result<MeshData> read_geometry(const aiScene &a_scene, unsigned int a_materialIndex, bool a_hasTexture)
{
    using MeshResult = Result<MeshData>;
    MeshDescriptor mesh;
    std::vector<std::uint32_t> indices;
    for (unsigned int meshIndex = 0; meshIndex < a_scene.mNumMeshes; ++meshIndex)
    {
        const aiMesh &source = *a_scene.mMeshes[meshIndex];
        if (source.mMaterialIndex != a_materialIndex)
            return MeshResult::failure({ErrorCategory::InvalidArgument, "ObjImporter.multiple_materials"});
        if (!source.HasPositions() || !source.HasNormals() || !source.HasFaces() ||
            (a_hasTexture && !source.HasTextureCoords(0)))
        {
            return MeshResult::failure({ErrorCategory::InvalidArgument, "ObjImporter.vertex_streams"});
        }
        const auto base = static_cast<std::uint32_t>(mesh.positions.size());
        if (source.mNumVertices > k_maxVertices - base || source.mNumFaces > (k_maxIndices - indices.size()) / 3)
            return MeshResult::failure({ErrorCategory::InvalidArgument, "ObjImporter.geometry_limit"});
        for (unsigned int vertex = 0; vertex < source.mNumVertices; ++vertex)
        {
            const auto &position = source.mVertices[vertex];
            const auto &normal = source.mNormals[vertex];
            const auto uv = source.HasTextureCoords(0) ? source.mTextureCoords[0][vertex] : aiVector3D{};
            mesh.positions.push_back({position.x, position.y, position.z});
            mesh.normals.push_back({normal.x, normal.y, normal.z});
            mesh.uv0.push_back({uv.x, uv.y});
        }
        for (unsigned int face = 0; face < source.mNumFaces; ++face)
        {
            const auto &triangle = source.mFaces[face];
            if (triangle.mNumIndices != 3 || !triangle.mIndices)
                return MeshResult::failure({ErrorCategory::InvalidArgument, "ObjImporter.triangles_required"});
            for (unsigned int corner = 0; corner < 3; ++corner)
            {
                if (triangle.mIndices[corner] >= source.mNumVertices)
                    return MeshResult::failure({ErrorCategory::InvalidArgument, "ObjImporter.index_range"});
                indices.push_back(base + triangle.mIndices[corner]);
            }
        }
    }
    if (mesh.positions.size() <= std::size_t{std::numeric_limits<std::uint16_t>::max()} + 1)
    {
        // 全 Index は上限内の頂点を参照するため、16 bit へ縮めても値を維持できる
        std::vector<std::uint16_t> compact(indices.size());
        std::ranges::transform(indices, compact.begin(),
                               [](std::uint32_t a_index) { return static_cast<std::uint16_t>(a_index); });
        mesh.indices = std::move(compact);
    }
    else
        mesh.indices = std::move(indices);
    return MeshData::create(std::move(mesh));
}

/// @brief OBJ の右手系と面の順序を保ち、下原点の UV を PNG の上原点へ合わせる
Result<ObjGeometry> parse_obj(IFileSystem &a_files, const Path &a_root, const Path &a_source)
{
    using ObjResult = Result<ObjGeometry>;
    Assimp::Importer importer;
    auto io = std::make_unique<ObjIo>(a_files, a_root, a_source);
    const auto *sourceIo = io.get();
    importer.SetIOHandler(io.release());
    constexpr unsigned int flags = aiProcess_Triangulate | aiProcess_JoinIdenticalVertices | aiProcess_GenNormals |
                                   aiProcess_FlipUVs | aiProcess_ValidateDataStructure;
    const auto *scene = importer.ReadFile(a_source.utf8(), flags);
    if (sourceIo->error())
        return ObjResult::failure(*sourceIo->error());
    if (!scene || !scene->HasMeshes() || !scene->HasMaterials() || !scene->mMeshes[0])
        return ObjResult::failure({ErrorCategory::InvalidArgument, std::string("ObjImporter.read:") +
                                                                        importer.GetErrorString()});
    const auto materialIndex = scene->mMeshes[0]->mMaterialIndex;
    if (materialIndex >= scene->mNumMaterials || !scene->mMaterials[materialIndex])
        return ObjResult::failure({ErrorCategory::InvalidArgument, "ObjImporter.material_index"});
    const auto &material = *scene->mMaterials[materialIndex];
    auto texture = resolve_texture(a_files, a_root, a_source, *sourceIo, material);
    if (!texture.has_value())
        return ObjResult::failure(*texture.try_error());
    auto geometry = read_geometry(*scene, materialIndex, texture.try_value()->has_value());
    if (!geometry.has_value())
        return ObjResult::failure(*geometry.try_error());
    aiColor3D diffuse(1.0f, 1.0f, 1.0f);
    ai_real alpha = 1.0f;
    material.Get(AI_MATKEY_COLOR_DIFFUSE, diffuse);
    material.Get(AI_MATKEY_OPACITY, alpha);
    std::array<float, 4> color = {diffuse.r, diffuse.g, diffuse.b, alpha};
    for (auto &channel : color)
    {
        if (!std::isfinite(channel))
            return ObjResult::failure({ErrorCategory::InvalidArgument, "ObjImporter.material_color"});
        channel = std::clamp(channel, 0.0f, 1.0f);
    }
    return ObjResult::success({geometry.take_value(), color, texture.take_value()});
}

/// @brief AssetCatalog の書式で Material を生成し、ロケールに依存しない数字を保存する
std::string material_text(const std::string &a_id, const std::array<float, 4> &a_color,
                          const std::optional<std::string> &a_texture)
{
    std::string text = "version=1\nassetId=" + a_id + "\ntype=Material\nbaseColor=";
    for (std::size_t index = 0; index < a_color.size(); ++index)
    {
        if (index != 0)
            text += ',';
        std::array<char, 32> number{};
        const auto converted = std::to_chars(number.data(), number.data() + number.size(), a_color[index]);
        text.append(number.data(), converted.ptr);
    }
    text += '\n';
    if (a_texture)
        text += "baseColorTexture=" + *a_texture + '\n';
    return text;
}

/// @brief 新規 ID が既存 Asset と同じ Import で予約済みの Asset に重複しないことを確認する
Result<std::string> reserve_id(const AssetCatalog &a_catalog, const std::function<Result<std::string>()> &a_generator,
                               std::span<const std::string_view> a_reserved = {})
{
    auto id = a_generator();
    if (!id.has_value())
        return id;
    const auto &value = *id.try_value();
    const auto matches = [&](const auto &a_asset) { return a_asset.assetId == value; };
    if (!is_valid_asset_id(value) || std::ranges::find(a_reserved, value) != a_reserved.end() ||
        std::ranges::any_of(a_catalog.models(), matches) || std::ranges::any_of(a_catalog.materials(), matches) ||
        std::ranges::any_of(a_catalog.textures(), matches))
    {
        return Result<std::string>::failure({ErrorCategory::InvalidArgument, "ObjImporter.asset_id"});
    }
    return id;
}

/// @brief モデル公開前に書いた依存を復元し、旧モデルの参照を保つ
void rollback_import(IFileSystem &a_files, const Path &a_material,
                     const std::optional<std::vector<std::byte>> &a_previous,
                     const std::optional<Path> &a_createdMeta, bool a_hasMaterialWrite, Error &a_error)
{
    if (a_hasMaterialWrite)
    {
        if (a_previous)
        {
            auto restored = a_files.replace_file(a_material, *a_previous);
            if (!restored.has_value())
            {
                // 復元できなかった Material が参照する、新規 Texture Metadata は有効なまま保つ
                a_error.operation += " / " + restored.try_error()->operation;
                return;
            }
        }
        else
        {
            auto removed = a_files.remove(a_material);
            if (!removed.has_value())
            {
                a_error.operation += " / " + removed.try_error()->operation;
                return;
            }
        }
    }
    if (a_createdMeta)
    {
        auto removed = a_files.remove(*a_createdMeta);
        if (!removed.has_value())
            a_error.operation += " / " + removed.try_error()->operation;
    }
}
} // namespace

Result<ImportedModel> import_obj_model(IFileSystem &a_files, const Path &a_contentRoot, const Path &a_source,
                                      const std::function<Result<std::string>()> &a_generateId)
{
    using ImportResult = Result<ImportedModel>;
    try
    {
        if (!a_generateId || !a_contentRoot.is_absolute() || !a_source.is_absolute() ||
            !has_extension(a_source, ".obj"))
            return ImportResult::failure({ErrorCategory::InvalidArgument, "ObjImporter.source_arguments"});
        auto parsed = parse_obj(a_files, a_contentRoot, a_source);
        if (!parsed.has_value())
            return ImportResult::failure(*parsed.try_error());
        const auto &source = *parsed.try_value();
        auto loaded = AssetCatalog::load(a_files, a_contentRoot);
        if (!loaded.has_value())
            return ImportResult::failure(*loaded.try_error());
        const auto &catalog = *loaded.try_value();
        auto modelPath = a_source.parent().join(a_source.stem() + ".cuemodel");
        auto materialPath = a_source.parent().join(a_source.stem() + ".cuemat");
        if (!modelPath.has_value() || !materialPath.has_value())
            return ImportResult::failure({ErrorCategory::InvalidArgument, "ObjImporter.output_path"});

        const auto models = catalog.models();
        const auto previous = std::ranges::find_if(models, [&](const ModelAsset &a_existing)
        { return a_existing.source.utf8() == modelPath.try_value()->utf8(); });
        auto meshId = previous != models.end() ? Result<std::string>::success(previous->assetId)
                                               : reserve_id(catalog, a_generateId);
        if (!meshId.has_value())
            return ImportResult::failure(*meshId.try_error());
        const auto existingMaterial = previous != models.end() ? previous->materialAssetId : std::nullopt;
        const std::array<std::string_view, 1> meshIds = {*meshId.try_value()};
        auto materialId = existingMaterial ? Result<std::string>::success(*existingMaterial)
                                            : reserve_id(catalog, a_generateId, meshIds);
        if (!materialId.has_value())
            return ImportResult::failure(*materialId.try_error());

        std::optional<std::vector<std::byte>> originalMaterial;
        if (existingMaterial)
        {
            auto registered = catalog.material(*existingMaterial);
            if (!registered.has_value())
                return ImportResult::failure(*registered.try_error());
            materialPath = Result<Path>::success((*registered.try_value())->source);
            auto bytes = a_files.read_all(*materialPath.try_value(), k_maxMaterialBytes);
            if (!bytes.has_value())
                return ImportResult::failure(*bytes.try_error());
            originalMaterial = bytes.take_value();
        }
        else
        {
            auto exists = a_files.exists(*materialPath.try_value());
            if (!exists.has_value())
                return ImportResult::failure(*exists.try_error());
            if (*exists.try_value())
                return ImportResult::failure(
                    {ErrorCategory::InvalidArgument, "ObjImporter.material_destination_exists"});
        }

        // 手動配置済み Texture の ID と色空間は維持し、初回だけ新規サイドカーを作る
        std::optional<std::string> textureId;
        std::optional<Path> newMeta;
        if (source.png)
        {
            const auto textures = catalog.textures();
            const auto same = std::ranges::find_if(textures, [&](const TextureAsset &a_existing)
            { return a_existing.source.utf8() == source.png->utf8(); });
            if (same != textures.end())
                textureId = same->assetId;
            else
            {
                const std::array<std::string_view, 2> reserved = {*meshId.try_value(), *materialId.try_value()};
                auto id = reserve_id(catalog, a_generateId, reserved);
                if (!id.has_value())
                    return ImportResult::failure(*id.try_error());
                textureId = id.take_value();
                auto sidecar = source.png->parent().join(source.png->filename() + ".cuemeta");
                if (!sidecar.has_value())
                    return ImportResult::failure(*sidecar.try_error());
                newMeta = sidecar.take_value();
            }
        }

        // Mesh と Material の全 Data を生成し終えてから依存順に公開する
        auto modelData = encode_model_asset(
            *meshId.try_value(), std::string_view(*materialId.try_value()), source.mesh);
        if (!modelData.has_value())
            return ImportResult::failure(*modelData.try_error());
        const auto materialData = material_text(*materialId.try_value(), source.baseColor, textureId);
        const auto materialBytes = std::as_bytes(std::span(materialData.data(), materialData.size()));
        ImportedModel result{meshId.take_value(), materialId.take_value()};

        if (previous == models.end())
        {
            auto exists = a_files.exists(*modelPath.try_value());
            if (!exists.has_value())
                return ImportResult::failure(*exists.try_error());
            if (*exists.try_value())
                return ImportResult::failure({ErrorCategory::InvalidArgument, "ObjImporter.model_destination_exists"});
        }
        if (newMeta)
        {
            auto imported = import_texture(a_files, a_contentRoot, *source.png, *textureId);
            if (!imported.has_value())
                return ImportResult::failure(*imported.try_error());
            if (imported.try_value()->assetId != *textureId)
                return ImportResult::failure({ErrorCategory::InvalidState, "ObjImporter.texture_changed"});
        }
        bool isMaterialSaved = false;
        const auto publish = [&]() -> Result<void>
        {
            try
            {
                auto materialSaved = a_files.replace_file(*materialPath.try_value(), materialBytes);
                if (!materialSaved.has_value())
                    return Result<void>::failure(std::move(*materialSaved.try_error()));
                isMaterialSaved = true;
                auto modelSaved = a_files.replace_file(*modelPath.try_value(), *modelData.try_value());
                return modelSaved.has_value() ? Result<void>::success()
                                              : Result<void>::failure(std::move(*modelSaved.try_error()));
            }
            catch (const std::bad_alloc &)
            {
                return Result<void>::failure({ErrorCategory::PlatformFailure, "ObjImporter.publish_allocation"});
            }
        };
        auto published = publish();
        if (!published.has_value())
        {
            auto error = std::move(*published.try_error());
            rollback_import(a_files, *materialPath.try_value(), originalMaterial, newMeta, isMaterialSaved, error);
            return ImportResult::failure(std::move(error));
        }
        return ImportResult::success(std::move(result));
    }
    catch (const std::bad_alloc &)
    {
        return ImportResult::failure({ErrorCategory::PlatformFailure, "ObjImporter.allocation"});
    }
}
} // namespace cue
