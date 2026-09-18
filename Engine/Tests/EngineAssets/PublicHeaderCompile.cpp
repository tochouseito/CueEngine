#include <Cue/EngineAssets/BuiltInMesh.h>

#include <type_traits>

static_assert(std::is_standard_layout_v<cue::engine_assets::MeshVertex>);
static_assert(std::is_trivially_copyable_v<cue::engine_assets::MeshVertex>);

/// @brief EngineAssets Public Headerの独立Compile契約を実行する
int main()
{
    const auto cube = cue::engine_assets::built_in_cube_mesh();
    return cube.vertices.empty() ? 1 : 0;
}
