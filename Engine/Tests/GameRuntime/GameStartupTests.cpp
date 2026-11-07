#include <GameRuntime/GameStartup.h>

#include <cstdio>
#include <span>
#include <string>
#include <string_view>

#include "../Support/FileSystemProbe.h"
#include "../Support/TemporaryFiles.h"

namespace
{
#define CHECK(...)                                                                                                     \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(__VA_ARGS__))                                                                                            \
        {                                                                                                              \
            std::fprintf(stderr, "Game startup test line %d: %s\n", __LINE__, #__VA_ARGS__);                           \
            return __LINE__;                                                                                           \
        }                                                                                                              \
    } while (false)

constexpr std::string_view k_config =
    "formatVersion=1\nprojectId=7f533c49-d416-47ea-98c1-026822b1e6ed\ngameTarget=ExampleGame\n"
    "applicationName=Example\nplatform=Windows\narchitecture=x64\nconfiguration=Debug\n"
    "startupScene=Content/Scenes/Main.cuescene\nwindowTitle=回転 Cube\nwindowWidth=1280\nwindowHeight=720\n"
    "targetFramesPerSecond=60\n";
constexpr std::string_view k_marker =
    "formatVersion=1\nprojectId=7f533c49-d416-47ea-98c1-026822b1e6ed\ngameTarget=ExampleGame\n"
    "applicationName=Example\nplatform=Windows\narchitecture=x64\nconfiguration=Debug\n";

/// @brief 検証する Field 一つだけを置換する
std::string replace(std::string_view a_before, std::string_view a_after)
{
    std::string text(k_config);
    text.replace(text.find(a_before), a_before.size(), a_after);
    return text;
}
/// @brief 検証 Fixture の小さい File を抽象 FileSystem で書く
bool write(cue::IFileSystem &a_files, const cue::Path &a_file, std::string_view a_text)
{
    return a_files.write_all(a_file, std::as_bytes(std::span(a_text.data(), a_text.size())), true).has_value();
}
/// @brief 成功 Fixture の Path を所有値として受け取る
cue::Path path(cue::Result<cue::Path> a_path)
{
    return a_path.take_value();
}
/// @brief 対応 Platform を推測しない設定解釈と、未知 / 重複 / 範囲外を検証する
int test_parse()
{
    auto config = cue::parse_game_startup(k_config);
    CHECK(config.has_value());
    CHECK(config.try_value()->windowTitle == "回転 Cube" &&
          config.try_value()->startupScene.utf8() == "Content/Scenes/Main.cuescene");
    auto other = cue::parse_game_startup(replace("platform=Windows", "platform=OtherPlatform"));
    CHECK(other.has_value() && other.try_value()->platform == "OtherPlatform");
    CHECK(cue::parse_game_startup(replace("targetFramesPerSecond=60", "targetFramesPerSecond=0")).has_value());
    for (const auto &text :
         {std::string{},
          std::string(k_config) + "unexpected=true\n",
          std::string(k_config) + "windowWidth=12\n",
          replace("formatVersion=1", "formatVersion=2"),
          replace("projectId=7f533c49-d416-47ea-98c1-026822b1e6ed", "projectId=bad"),
          replace("windowWidth=1280", "windowWidth=0"),
          replace("windowHeight=720", "windowHeight=16385"),
          replace("windowWidth=1280", "windowWidth=-1"),
          replace("targetFramesPerSecond=60", "targetFramesPerSecond=NaN"),
          replace("targetFramesPerSecond=60", "targetFramesPerSecond=10001"),
          replace("applicationName=Example", "applicationName=../Other"),
          replace("startupScene=Content/Scenes/Main.cuescene", "startupScene=../Main.cuescene"),
          replace("startupScene=Content/Scenes/Main.cuescene", "startupScene=C:/Main.cuescene"),
          replace("startupScene=Content/Scenes/Main.cuescene", "startupScene=Content/../../Main.cuescene"),
          replace("startupScene=Content/Scenes/Main.cuescene", "startupScene=Content/Scene:stream.cuescene"),
          replace("startupScene=Content/Scenes/Main.cuescene", "startupScene=Content/Main.txt"),
          replace("windowTitle=回転 Cube", "windowTitle="),
          replace("configuration=Debug", "configuration=Unknown"),
          std::string(k_config) + std::string("#\0comment", 9),
          std::string(65537, 'x')})
        CHECK(!cue::parse_game_startup(text).has_value());
    return 0;
}

/// @brief Link / 配布途中 / I/O / Marker の競合を注入して起動前に拒否する
class StartupProbe final : public cue::tests::FileSystemProbe
{
  public:
    /// @brief Fixture より短い期間 FileSystem を借用する
    explicit StartupProbe(cue::IFileSystem &a_files) : FileSystemProbe(a_files)
    {
    }
    /// @brief Content の途中に Link がある場合を再現する
    cue::Result<std::optional<cue::FileInfo>> stat(const cue::Path &a_path) override
    {
        if (isContentLink && a_path.filename() == "Content")
            return cue::Result<std::optional<cue::FileInfo>>::success(cue::FileInfo{cue::FileType::Link});
        return FileSystemProbe::stat(a_path);
    }
    /// @brief 2 回目の完成 Marker 読込前に再公開を再現する
    cue::Result<std::unique_ptr<cue::IFile>> open(const cue::Path &a_path,
                                                  const cue::FileOpenDesc &a_desc = {}) override
    {
        if (a_path.filename() == "GamePackage.cuepackage" && ++markerOpens == 2 && shouldRepublish)
        {
            const auto text = std::string(k_marker) + "# republished\n";
            auto saved = files.write_all(a_path, std::as_bytes(std::span(text.data(), text.size())), false);
            if (!saved.has_value())
                return cue::Result<std::unique_ptr<cue::IFile>>::failure(*saved.try_error());
        }
        return FileSystemProbe::open(a_path, a_desc);
    }
    bool isContentLink = false;
    bool shouldRepublish = false;
    unsigned markerOpens = 0;
};

/// @brief CWD と Repository に依存せず、完成した配布 Directory だけを起動設定として受け取る
int test_files()
{
    cue::tests::TemporaryFiles fixture;
    CHECK(fixture.isCreated && fixture.files);
    auto &files = *fixture.files;
    const auto configFile = path(fixture.root.join("GameStartup.cueapp"));
    const auto marker = path(fixture.root.join("GamePackage.cuepackage"));
    const auto scene = path(fixture.root.join("Content/Scenes/Main.cuescene"));
    CHECK(write(files, configFile, k_config) && write(files, scene, "Scene parser validates this separately"));
    CHECK(!cue::load_game_startup(files, fixture.root).has_value());
    CHECK(write(files, marker, k_marker));
    StartupProbe probe(files);
    auto loaded = cue::load_game_startup(probe, fixture.root);
    CHECK(loaded.has_value() && probe.absoluteCount == 0 && probe.saveCount == 0);
    probe.isContentLink = true;
    CHECK(!cue::load_game_startup(probe, fixture.root).has_value());
    probe.isContentLink = false;
    probe.failRead = true;
    CHECK(!cue::load_game_startup(probe, fixture.root).has_value());
    probe.failRead = false;
    auto changed = std::string(k_marker);
    changed.replace(changed.find("platform=Windows"), 16, "platform=Other");
    CHECK(write(files, marker, changed) && !cue::load_game_startup(files, fixture.root).has_value());
    CHECK(write(files, marker, k_marker));
    probe.markerOpens = 0;
    probe.shouldRepublish = true;
    CHECK(!cue::load_game_startup(probe, fixture.root).has_value());
    CHECK(write(files, marker, k_marker));
    auto relative = cue::Path::create(".");
    CHECK(relative.has_value() && !cue::load_game_startup(files, *relative.try_value()).has_value());
    const auto moved = path(fixture.root.join("Moved Package"));
    CHECK(files.create_directories(moved).has_value());
    CHECK(files.rename(configFile, path(moved.join("GameStartup.cueapp"))).has_value());
    CHECK(files.rename(marker, path(moved.join("GamePackage.cuepackage"))).has_value());
    CHECK(files.rename(path(fixture.root.join("Content")), path(moved.join("Content"))).has_value());
    CHECK(cue::load_game_startup(files, moved).has_value());
    return 0;
}
} // namespace

/// @brief 起動設定の Schema と Package の完成契約を検証する
int main()
{
    const auto parsed = test_parse();
    return parsed ? parsed : test_files();
}
