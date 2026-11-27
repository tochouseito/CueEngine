#pragma once

#include <string_view>

#include <Assets/MeshData.h>
#include <Foundation/Uuid.h>

namespace cue
{
inline constexpr std::string_view k_cubeMeshId = "cue://engine/mesh/cube";

/// @brief 組み込み Mesh と Project で付与する永続 UUID の表記を検証する
[[nodiscard]] inline bool is_valid_mesh_asset_id(std::string_view a_assetId) noexcept
{
    return a_assetId == k_cubeMeshId || is_valid_uuid_text(a_assetId);
}

/// @brief 原点中心・一辺 1 の Cube を生成する。面ごとの Normal / UV 用に 24 頂点、外向き CCW を使う
/// 各呼出しは独立した CPU Geometry を所有し、GPU や可変 Global Cache を利用しない
[[nodiscard]] Result<MeshData> create_cube_mesh();
} // namespace cue
