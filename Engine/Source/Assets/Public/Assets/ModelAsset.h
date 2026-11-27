#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <Assets/MeshData.h>
#include <Foundation/Path.h>
#include <Foundation/Result.h>

namespace cue
{
class IFileSystem;

/// @brief 静的 Model Asset の永続 ID と既定 Material を保持する
struct ModelAsset final
{
    std::string assetId;
    Path source;
    std::optional<std::string> materialAssetId;
};

/// @brief 単一 Mesh の Model Asset と CPU Geometry の所有値
struct ModelData final
{
    ModelAsset asset;
    MeshData mesh;
};

/// @brief 検証済み Mesh と永続 ID を Model File のバイト列に変換する
[[nodiscard]] Result<std::vector<std::byte>> encode_model_asset(std::string_view a_assetId,
                                                               std::optional<std::string_view> a_materialAssetId,
                                                               const MeshData &a_mesh);

/// @brief Model File の Layout と全 Geometry を検証し、入力から独立した所有値を返す
[[nodiscard]] Result<ModelData> decode_model_asset(std::span<const std::byte> a_bytes, Path a_source = {});

/// @brief Content 内の変換済み Model File から CPU Geometry を取得する
[[nodiscard]] Result<ModelData> read_model_asset(IFileSystem &a_files, const Path &a_source);
} // namespace cue
