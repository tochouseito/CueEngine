#include "BuildPlatforms.h"

#include <array>

namespace cue::detail
{
const GameBuildPlatform &windows_game_build_platform() noexcept
{
    static constexpr std::array<std::string_view, 1> architectures{"x64"};
    static constexpr GameBuildPlatform descriptor{"Windows", "Windows", architectures, ".dll", ".lib", ".exe"};
    return descriptor;
}

Result<GameBuildTarget> native_game_build_target(GameBuildConfiguration a_configuration)
{
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    const auto &platform = windows_game_build_platform();
    return create_game_build_target(platform.id, platform.architectures.front(), a_configuration);
#else
    (void)a_configuration;
    return Result<GameBuildTarget>::failure({ErrorCategory::InvalidState, "GameBuild.native_target_unsupported"});
#endif
}
} // namespace cue::detail
