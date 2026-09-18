#include <Cue/EngineAssets/BuiltInMesh.h>

#include <Cue/Math/Vector.h>

#include <algorithm>
#include <cstdlib>
#include <limits>

namespace
{
/// @brief 条件が偽ならTest Processを失敗終了する
void require(bool a_condition) noexcept
{
    if (!a_condition)
    {
        std::abort();
    }
}

/// @brief CubeのIdentity、Bounds、Normal、Triangle Contractを検証する
void test_cube_mesh() noexcept
{
    const auto cube = cue::engine_assets::built_in_cube_mesh();
    const auto secondView = cue::engine_assets::built_in_cube_mesh();

    require(cube.assetId == cue::engine_assets::k_cubeMeshAssetId);
    require(cube.revision == cue::engine_assets::k_cubeMeshRevision);
    require(cube.vertices.size() == 24U);
    require(cube.indices.size() == 36U);
    require(cube.vertices.data() == secondView.vertices.data());
    require(cube.indices.data() == secondView.indices.data());

    cue::math::Vector3 minimum{
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max(),
    };
    cue::math::Vector3 maximum{
        std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest(),
    };

    for (const auto &vertex : cube.vertices)
    {
        minimum.x = (std::min)(minimum.x, vertex.position.x);
        minimum.y = (std::min)(minimum.y, vertex.position.y);
        minimum.z = (std::min)(minimum.z, vertex.position.z);
        maximum.x = (std::max)(maximum.x, vertex.position.x);
        maximum.y = (std::max)(maximum.y, vertex.position.y);
        maximum.z = (std::max)(maximum.z, vertex.position.z);
        require(cue::math::dot(vertex.normal, vertex.normal) == 1.0F);
    }

    require(minimum == cue::math::Vector3{-0.5F, -0.5F, -0.5F});
    require(maximum == cue::math::Vector3{0.5F, 0.5F, 0.5F});

    for (std::size_t index = 0U; index < cube.indices.size(); index += 3U)
    {
        const auto firstIndex = cube.indices[index];
        const auto secondIndex = cube.indices[index + 1U];
        const auto thirdIndex = cube.indices[index + 2U];
        require(firstIndex < cube.vertices.size());
        require(secondIndex < cube.vertices.size());
        require(thirdIndex < cube.vertices.size());

        const auto &first = cube.vertices[firstIndex];
        const auto &second = cube.vertices[secondIndex];
        const auto &third = cube.vertices[thirdIndex];
        const auto edgeA = second.position - first.position;
        const auto edgeB = third.position - first.position;
        const auto triangleNormal = cue::math::cross(edgeA, edgeB);

        require(cue::math::dot(triangleNormal, triangleNormal) > 0.0F);
        require(cue::math::dot(triangleNormal, first.normal) > 0.0F);
        require(first.normal == second.normal);
        require(first.normal == third.normal);
    }
}
} // namespace

/// @brief Built-in Mesh Contract Testを実行する
int main()
{
    test_cube_mesh();
    return 0;
}
