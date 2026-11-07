#include <Assets/BuiltinMesh.h>

#include <array>
#include <new>
#include <utility>

namespace cue
{
Result<MeshData> create_cube_mesh()
{
    try
    {
        struct Face final
        {
            math::float3 normal;
            math::float3 u;
            math::float3 v;
        };
        // cross(u, v) が面の外側を向くように各面の軸を固定する
        constexpr std::array faces = {Face{{0, 0, 1}, {1, 0, 0}, {0, 1, 0}},  Face{{0, 0, -1}, {-1, 0, 0}, {0, 1, 0}},
                                      Face{{1, 0, 0}, {0, 0, -1}, {0, 1, 0}}, Face{{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}},
                                      Face{{0, 1, 0}, {1, 0, 0}, {0, 0, -1}}, Face{{0, -1, 0}, {1, 0, 0}, {0, 0, 1}}};
        constexpr std::array<math::float2, 4> uvs = {{{0, 0}, {1, 0}, {1, 1}, {0, 1}}};
        MeshDescriptor descriptor;
        descriptor.positions.reserve(24);
        descriptor.normals.reserve(24);
        descriptor.uv0.reserve(24);
        auto &indices = std::get<std::vector<std::uint16_t>>(descriptor.indices);
        indices.reserve(36);
        for (const auto &face : faces)
        {
            const auto base = static_cast<std::uint16_t>(descriptor.positions.size());
            for (const auto uv : uvs)
            {
                descriptor.positions.push_back(face.normal * 0.5f + face.u * (uv.x - 0.5f) + face.v * (uv.y - 0.5f));
                descriptor.normals.push_back(face.normal);
                descriptor.uv0.push_back(uv);
            }
            for (const std::uint16_t index : {0, 1, 2, 0, 2, 3})
                indices.push_back(static_cast<std::uint16_t>(base + index));
        }
        return MeshData::create(std::move(descriptor));
    }
    catch (const std::bad_alloc &)
    {
        return Result<MeshData>::failure({ErrorCategory::PlatformFailure, "BuiltinMesh.cube.allocation"});
    }
}
} // namespace cue
