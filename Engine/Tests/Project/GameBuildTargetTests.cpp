#include <ProjectBuild/BuildTarget.h>

#include <array>
#include <cstdio>
#include <string>

namespace
{
#define CHECK(...)                                                                                                     \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(__VA_ARGS__))                                                                                            \
        {                                                                                                              \
            std::fprintf(stderr, "Game Build target test line %d: %s\n", __LINE__, #__VA_ARGS__);                      \
            return __LINE__;                                                                                           \
        }                                                                                                              \
    } while (false)
} // namespace

/// @brief Build 要求の明示選択・所有・未対応拒否と Native Play の独立した対象を確認する
int main()
{
    using cue::GameBuildConfiguration;
    const auto platforms = cue::supported_game_build_platforms();
    CHECK(platforms.size() == 1);
    CHECK(platforms[0].id == "Windows");
    CHECK(platforms[0].architectures.size() == 1 && platforms[0].architectures[0] == "x64");
    CHECK(platforms[0].moduleSuffix == ".dll" && platforms[0].staticLibrarySuffix == ".lib" &&
          platforms[0].applicationSuffix == ".exe");
    CHECK(cue::find_game_build_platform("Windows") == &platforms[0]);
    CHECK(!cue::find_game_build_platform("") && !cue::find_game_build_platform("Linux"));

    std::string platform = "Windows";
    std::string architecture = "x64";
    auto target = cue::create_game_build_target(platform, architecture, GameBuildConfiguration::Release);
    CHECK(target.has_value());
    platform = "Linux";
    architecture = "arm64";
    CHECK(target.try_value()->platform == "Windows" && target.try_value()->architecture == "x64" &&
          target.try_value()->configuration == GameBuildConfiguration::Release);
    CHECK(cue::validate_game_build_target(*target.try_value()).has_value());

    // 同じ Project でも Build ごとに構成を選べる。Native の Play 設定を製品の選択から書き換えない
    const auto native = cue::current_native_game_build_target();
    CHECK(native.has_value());
    CHECK(native.try_value()->configuration == cue::current_game_build_configuration());
    for (const auto configuration :
         {GameBuildConfiguration::Debug, GameBuildConfiguration::Development, GameBuildConfiguration::Release})
    {
        auto selected = cue::create_game_build_target("Windows", "x64", configuration);
        CHECK(selected.has_value() && selected.try_value()->configuration == configuration);
        const auto unchanged = cue::current_native_game_build_target();
        CHECK(unchanged.has_value() && *unchanged.try_value() == *native.try_value());
    }
    const std::array invalid{cue::GameBuildTarget{"", "x64", GameBuildConfiguration::Debug},
                             cue::GameBuildTarget{"Windows", "", GameBuildConfiguration::Debug},
                             cue::GameBuildTarget{"Linux", "x64", GameBuildConfiguration::Debug},
                             cue::GameBuildTarget{"Android", "arm64", GameBuildConfiguration::Debug},
                             cue::GameBuildTarget{"windows", "x64", GameBuildConfiguration::Debug},
                             cue::GameBuildTarget{"Windows", "arm64", GameBuildConfiguration::Debug},
                             cue::GameBuildTarget{"Windows", "../x64", GameBuildConfiguration::Debug},
                             cue::GameBuildTarget{"Windows", "x64", static_cast<GameBuildConfiguration>(99)}};
    for (const auto &value : invalid)
    {
        const auto validated = cue::validate_game_build_target(value);
        CHECK(!validated.has_value());
        const auto created = cue::create_game_build_target(value.platform, value.architecture, value.configuration);
        CHECK(!created.has_value());
    }
    CHECK(cue::game_build_configuration_name(static_cast<GameBuildConfiguration>(99)).empty());
    return 0;
}
