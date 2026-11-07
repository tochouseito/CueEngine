#include <Assets/BuiltinMesh.h>
#include <Assets/MeshData.h>

/// @brief Native API なしで CPU Geometry の公開契約を利用できることを確認する
int main()
{
    return cue::create_cube_mesh().has_value() ? 0 : 1;
}
