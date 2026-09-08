#include <Cue/Build/Service.h>

#include <type_traits>

static_assert(!std::is_copy_constructible_v<cue::GameBuildService>);
static_assert(!std::is_move_constructible_v<cue::GameBuildService>);

/// @brief Service公開Headerを単独Includeして宣言の自己完結性を検証する
void cue_build_service_header_compile()
{
}
