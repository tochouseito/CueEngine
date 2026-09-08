#include <Cue/Build/Plan.h>

#include <type_traits>

static_assert(!std::is_default_constructible_v<cue::BuildPlan>);
static_assert(!std::is_copy_constructible_v<cue::BuildPlan>);
static_assert(std::is_nothrow_move_constructible_v<cue::BuildPlan>);

void cue_build_plan_header_compile()
{
}
