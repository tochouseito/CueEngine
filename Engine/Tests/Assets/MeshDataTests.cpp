#include <Assets/BuiltinMesh.h>

#include <cmath>
#include <cstdio>
#include <limits>
#include <utility>

#define REQUIRE(a_condition)                                                                                           \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(a_condition))                                                                                            \
        {                                                                                                              \
            std::fprintf(stderr, "MeshData test line %d\n", __LINE__);                                                 \
            return 1;                                                                                                  \
        }                                                                                                              \
    } while (false)

/// @brief 境界入力の検証用に最小 TriangleList を作る
cue::MeshDescriptor triangle()
{
    cue::MeshDescriptor descriptor;
    descriptor.positions = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    descriptor.indices = std::vector<std::uint16_t>{0, 1, 2};
    return descriptor;
}

/// @brief Cube の形状・UV Seam・面法線・Winding と入力不正を GPU なしで検証する
int main()
{
    auto cubeResult = cue::create_cube_mesh();
    REQUIRE(cubeResult.has_value());
    auto cube = cubeResult.take_value();
    REQUIRE(cube.positions().size() == 24 && cube.normals().size() == 24 && cube.uv0().size() == 24);
    REQUIRE(cube.index_count() == 36 && cube.index_format() == cue::MeshIndexFormat::Uint16);
    REQUIRE(cube.indices32().empty() && cue::k_cubeMeshId == "cue://engine/mesh/cube");
    for (std::size_t face = 0; face < 6; ++face)
    {
        const auto normal = cube.normals()[face * 4];
        for (std::size_t corner = 0; corner < 4; ++corner)
        {
            const auto vertex = face * 4 + corner;
            REQUIRE(cube.normals()[vertex] == normal);
            const auto position = cube.positions()[vertex];
            REQUIRE(std::abs(position.x) == 0.5f && std::abs(position.y) == 0.5f && std::abs(position.z) == 0.5f);
            REQUIRE(cue::math::dot(position, normal) == 0.5f);
            REQUIRE(cube.uv0()[vertex].x >= 0 && cube.uv0()[vertex].x <= 1 && cube.uv0()[vertex].y >= 0 &&
                    cube.uv0()[vertex].y <= 1);
        }
        for (std::size_t offset = 0; offset < 6; offset += 3)
        {
            const auto first = face * 6 + offset;
            const auto a = cube.positions()[cube.indices16()[first]];
            const auto b = cube.positions()[cube.indices16()[first + 1]];
            const auto c = cube.positions()[cube.indices16()[first + 2]];
            REQUIRE(cue::math::dot(cue::math::cross(b - a, c - a), normal) > 0.0f);
        }
    }
    auto plain = cue::MeshData::create(triangle());
    REQUIRE(plain.has_value() && plain.try_value()->normals().empty() && plain.try_value()->uv0().empty());
    auto descriptor = triangle();
    descriptor.normals = {{0, 0, 2}, {0, 0, 2}, {0, 0, 2}};
    auto normalized = cue::MeshData::create(std::move(descriptor));
    REQUIRE(normalized.has_value() && normalized.try_value()->normals()[0].z == 1);
    descriptor = triangle();
    descriptor.normals = {{0, 0, 1}};
    REQUIRE(!cue::MeshData::create(std::move(descriptor)).has_value());
    descriptor = triangle();
    descriptor.normals.resize(3);
    REQUIRE(!cue::MeshData::create(std::move(descriptor)).has_value());
    descriptor = triangle();
    descriptor.positions[0].x = std::numeric_limits<float>::infinity();
    REQUIRE(!cue::MeshData::create(std::move(descriptor)).has_value());
    descriptor = triangle();
    descriptor.uv0 = {{0, 0}, {0, 0}, {std::numeric_limits<float>::quiet_NaN(), 0}};
    REQUIRE(!cue::MeshData::create(std::move(descriptor)).has_value());
    descriptor = triangle();
    descriptor.indices = std::vector<std::uint16_t>{0, 1, 3};
    REQUIRE(!cue::MeshData::create(std::move(descriptor)).has_value());
    descriptor = triangle();
    descriptor.indices = std::vector<std::uint16_t>{0, 1};
    REQUIRE(!cue::MeshData::create(std::move(descriptor)).has_value());
    REQUIRE(!cue::MeshData::create({}).has_value());
    descriptor = triangle();
    descriptor.positions.resize(70000);
    descriptor.indices = std::vector<std::uint32_t>{0, 1, 69999};
    auto wide = cue::MeshData::create(std::move(descriptor));
    REQUIRE(wide.has_value() && wide.try_value()->index_format() == cue::MeshIndexFormat::Uint32);
    REQUIRE(wide.try_value()->indices16().empty() && wide.try_value()->indices32()[2] == 69999);
    return 0;
}
