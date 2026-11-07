#include <ProjectBuild/BuildTarget.h>

#include <algorithm>
#include <array>
#include <utility>

#include "BuildPlatforms.h"

namespace cue
{
std::span<const GameBuildPlatform> supported_game_build_platforms() noexcept
{
    // 新 Platform の実装を登録する場所を一箇所にし、呼出側へ Platform 固有条件を広げない
    static const std::array<GameBuildPlatform, 1> platforms{detail::windows_game_build_platform()};
    return platforms;
}

const GameBuildPlatform *find_game_build_platform(std::string_view a_platform) noexcept
{
    const auto platforms = supported_game_build_platforms();
    const auto found =
        std::find_if(platforms.begin(), platforms.end(), [&](const auto &a_entry) { return a_entry.id == a_platform; });
    return found == platforms.end() ? nullptr : &*found;
}

Result<void> validate_game_build_target(const GameBuildTarget &a_target)
{
    if (game_build_configuration_name(a_target.configuration).empty())
        return Result<void>::failure({ErrorCategory::InvalidArgument, "GameBuild.configuration_unsupported"});
    const auto *platform = find_game_build_platform(a_target.platform);
    if (!platform)
        return Result<void>::failure(
            {ErrorCategory::InvalidArgument, "GameBuild.platform_unsupported:" + a_target.platform});
    if (std::find(platform->architectures.begin(), platform->architectures.end(), a_target.architecture) ==
        platform->architectures.end())
        return Result<void>::failure(
            {ErrorCategory::InvalidArgument, "GameBuild.architecture_unsupported:" + a_target.architecture});
    return Result<void>::success();
}

Result<GameBuildTarget> create_game_build_target(std::string_view a_platform, std::string_view a_architecture,
                                                 GameBuildConfiguration a_configuration)
{
    try
    {
        GameBuildTarget target{std::string(a_platform), std::string(a_architecture), a_configuration};
        auto validated = validate_game_build_target(target);
        if (!validated.has_value())
            return Result<GameBuildTarget>::failure(*validated.try_error());
        return Result<GameBuildTarget>::success(std::move(target));
    }
    catch (...)
    {
        return Result<GameBuildTarget>::failure({ErrorCategory::PlatformFailure, "GameBuild.target_allocation"});
    }
}

Result<GameBuildTarget> current_native_game_build_target()
{
    return detail::native_game_build_target(current_game_build_configuration());
}

std::string_view game_build_configuration_name(GameBuildConfiguration a_configuration) noexcept
{
    switch (a_configuration)
    {
    case GameBuildConfiguration::Debug:
        return "Debug";
    case GameBuildConfiguration::Development:
        return "Development";
    case GameBuildConfiguration::Release:
        return "Release";
    }
    return {};
}

GameBuildConfiguration current_game_build_configuration() noexcept
{
#if defined(CUE_GAME_BUILD_DEBUG)
    return GameBuildConfiguration::Debug;
#elif defined(CUE_GAME_BUILD_DEVELOPMENT)
    return GameBuildConfiguration::Development;
#else
    return GameBuildConfiguration::Release;
#endif
}
} // namespace cue
