#pragma once

#include <cstdint>
#include <span>
#include <variant>
#include <vector>

#include <Foundation/Result.h>
#include <Math/Vector2.h>
#include <Math/Vector3.h>

namespace cue
{
/// @brief Index の要素幅。値は Mesh 内の頂点番号であり Byte Offset ではない
enum class MeshIndexFormat
{
    Uint16,
    Uint32,
};

/// @brief TriangleList の CPU 正本。属性は分離し、存在する属性は全頂点分を持つ
struct MeshDescriptor final
{
    std::vector<math::float3> positions;
    std::vector<math::float3> normals;
    std::vector<math::float2> uv0;
    std::variant<std::vector<std::uint16_t>, std::vector<std::uint32_t>> indices;
};

/// @brief 検証済み CPU Geometry を一意所有する。Bounds / Material / GPU Handle は持たない
/// 所有者の変更・Move・破棄と競合しなければ複数 Thread で読み取れる
class MeshData final
{
  public:
    /// @brief 属性数・有限値・TriangleList・Index 範囲を検証し、Normal を正規化して所有する
    [[nodiscard]] static Result<MeshData> create(MeshDescriptor a_descriptor);
    /// @brief 大容量 Geometry の暗黙コピーを禁止する
    MeshData(const MeshData &) = delete;
    /// @brief Geometry の暗黙コピーによる所有権の複製を禁止する
    MeshData &operator=(const MeshData &) = delete;
    /// @brief Geometry の所有権を移す。移動元は再代入または破棄だけに使う
    MeshData(MeshData &&) noexcept = default;
    /// @brief 既存 Geometry を解放し、移動元から所有権を受け取る
    MeshData &operator=(MeshData &&) noexcept = default;
    /// @brief 所有する CPU Geometry を解放する
    ~MeshData() = default;

    /// @brief 所有者の次の Move / 破棄まで有効な読み取り View を返す
    [[nodiscard]] std::span<const math::float3> positions() const noexcept;
    /// @brief Normal がなければ空の読み取り View を返す
    [[nodiscard]] std::span<const math::float3> normals() const noexcept;
    /// @brief UV0 がなければ空の読み取り View を返す
    [[nodiscard]] std::span<const math::float2> uv0() const noexcept;
    /// @brief Mesh 内で共通の Index 幅を返す
    [[nodiscard]] MeshIndexFormat index_format() const noexcept;
    /// @brief TriangleList の Index 数を返す
    [[nodiscard]] std::uint32_t index_count() const noexcept;
    /// @brief Uint16 の場合だけ Index View を返す
    [[nodiscard]] std::span<const std::uint16_t> indices16() const noexcept;
    /// @brief Uint32 の場合だけ Index View を返す
    [[nodiscard]] std::span<const std::uint32_t> indices32() const noexcept;

  private:
    /// @brief 検証済み Descriptor だけを保持する
    explicit MeshData(MeshDescriptor a_descriptor) noexcept;
    MeshDescriptor m_descriptor;
};
} // namespace cue
