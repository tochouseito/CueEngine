#include <Project/Project.h>

#include <array>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>

#include "../Support/FileSystemProbe.h"
#include "../Support/TemporaryFiles.h"

namespace
{
constexpr std::string_view k_descriptor =
    "formatVersion=1\nprojectId=7f533c49-d416-47ea-98c1-026822b1e6ed\n"
    "name=Game Project\napplicationName=Game\nsourceRoot=GameSource\ncontentRoot=Content\n";

#define CHECK(...)                                                                                                     \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(__VA_ARGS__))                                                                                            \
        {                                                                                                              \
            std::fprintf(stderr, "Project Build contract check failed at %d: %s\n", __LINE__, #__VA_ARGS__);           \
            return __LINE__;                                                                                           \
        }                                                                                                              \
    } while (false)

/// @brief 固定 Fixture 名から得た Path の所有値を取り出す
cue::Path path(cue::Result<cue::Path> a_value)
{
    return a_value.take_value();
}
/// @brief 一時 Fixture へ UTF-8 を保存する。Project の読込自体は書き込まない
bool write(cue::IFileSystem &a_files, const cue::Path &a_file, std::string_view a_text)
{
    return a_files.write_all(a_file, std::as_bytes(std::span(a_text.data(), a_text.size())), true).has_value();
}

/// @brief CMake Entry の問い合わせ失敗を、形式不正と区別して返すか観測する
class CmakeStatProbe final : public cue::tests::FileSystemProbe
{
  public:
    /// @brief Test の FileSystem を呼出し中だけ借用する
    explicit CmakeStatProbe(cue::IFileSystem &a_files) : FileSystemProbe(a_files)
    {
    }
    /// @brief CMake Entry の問い合わせだけを失敗させる
    cue::Result<std::optional<cue::FileInfo>> stat(const cue::Path &a_path) override
    {
        if (a_path.filename() == "CMakeLists.txt")
            return cue::Result<std::optional<cue::FileInfo>>::failure(
                {cue::ErrorCategory::PlatformFailure, "Probe.cmake_stat", 5});
        return FileSystemProbe::stat(a_path);
    }
};

/// @brief Target 名が Command / Path / Device として解釈されないことを検証する
int test_names()
{
    for (const auto *name : {"Game", "MyGame_1", "_Game", "Game-Client", "com0", "lpt10"})
        CHECK(cue::is_valid_game_target(name));
    const std::array bad{"",
                         "1Game",
                         "../Game",
                         "Game/Target",
                         "Game\\Target",
                         "C:/Game",
                         "Game.dll",
                         "Game::Target",
                         "Game Target",
                         "Game;Other",
                         "Game$(cmd)",
                         "Game=Value",
                         "Game\nName",
                         "ゲーム",
                         "all",
                         "ALL_BUILD",
                         "Zero_Check",
                         "Install",
                         "CLEAN",
                         "test",
                         "help",
                         "Package",
                         "package_source",
                         "rebuild",
                         "edit_cache",
                         "REBUILD_CACHE",
                         "run_tests",
                         "CON",
                         "PrN",
                         "aux",
                         "Nul",
                         "COM1",
                         "com9",
                         "lpt1",
                         "LPT9"};
    for (const auto *name : bad)
        CHECK(!cue::is_valid_game_target(name));
    CHECK(cue::is_valid_game_target(std::string(128, 'A')));
    CHECK(!cue::is_valid_game_target(std::string(129, 'A')));
    CHECK(!cue::is_valid_game_target(std::string_view("Game\0Other", 10)));
    return 0;
}

/// @brief 旧形式、CMake Entry の検証、移動後の相対解決と所有値の寿命を確認する
int test_project(cue::tests::TemporaryFiles &a_fixture)
{
    auto &files = *a_fixture.files;
    const auto root = path(a_fixture.root.join("日本語 Game Project"));
    const auto source = path(root.join("GameSource"));
    const auto content = path(root.join("Content"));
    const auto projectFile = path(root.join("Game.cueproj"));
    const auto entry = path(source.join("CMakeLists.txt"));
    CHECK(files.create_directories(source).has_value() && files.create_directories(content).has_value());
    CHECK(write(files, projectFile, k_descriptor));
    auto legacy = cue::Project::load(files, projectFile);
    CHECK(legacy.has_value() && !legacy.try_value()->descriptor().gameTarget && !legacy.try_value()->game_cmake_file());
    const auto configured = std::string(k_descriptor) + "gameTarget=Game_Client\n";
    CHECK(write(files, projectFile, configured));
    // 読込の検証では File / Directory を自動生成せず、欠損 Entry を黙って補完しない
    auto missing = cue::Project::load(files, projectFile);
    CHECK(!missing.has_value() && missing.try_error()->operation == "Project.gameTarget.cmake_file");
    auto missingInfo = files.stat(entry);
    CHECK(missingInfo.has_value() && !*missingInfo.try_value());
    CHECK(files.create_directories(entry).has_value());
    CHECK(!cue::Project::load(files, projectFile).has_value());
    CHECK(files.remove(entry).has_value());
    CHECK(write(files, entry, "# Project load must not execute this CMake file\nmessage(FATAL_ERROR unexpected)\n"));
    cue::tests::FileSystemProbe probe(files);
    probe.failCreate = true;
    auto loaded = cue::Project::load(probe, projectFile);
    CHECK(loaded.has_value() && probe.absoluteCount == 1);
    CHECK(loaded.try_value()->descriptor().gameTarget == "Game_Client" && loaded.try_value()->game_cmake_file() &&
          loaded.try_value()->game_cmake_file()->utf8() == entry.utf8());
    const auto owned = loaded.take_value();
    CHECK(write(files, projectFile, std::string(k_descriptor) + "gameTarget=AnotherGame\n"));
    CHECK(owned.descriptor().gameTarget == "Game_Client");
    CmakeStatProbe failed(files);
    auto failedLoad = cue::Project::load(failed, projectFile);
    CHECK(!failedLoad.has_value() && failedLoad.try_error()->operation == "Probe.cmake_stat");
    const auto movedRoot = path(a_fixture.root.join("Moved Project"));
    CHECK(files.rename(root, movedRoot).has_value());
    auto moved = cue::Project::load(files, path(movedRoot.join("Game.cueproj")));
    CHECK(moved.has_value() && moved.try_value()->descriptor().gameTarget == "AnotherGame" &&
          moved.try_value()->game_cmake_file()->utf8() == movedRoot.utf8() + "/GameSource/CMakeLists.txt");
    const auto movedFile = path(movedRoot.join("Game.cueproj"));
    for (const auto *extra : {"gameTarget=\n", "gameTarget=Game\ngameTarget=Game\n", "gameTarget=../Game\n",
                              "gameTarget=ALL_BUILD\n", "gameTarget=CON\n"})
    {
        CHECK(write(files, movedFile, std::string(k_descriptor) + extra));
        CHECK(!cue::Project::load(files, movedFile).has_value());
    }
    return 0;
}

/// @brief Build の生成先を Source / Content と分離し、CMake 固有の非対応 Path を明示的に拒否する
int test_build_paths(cue::tests::TemporaryFiles &a_fixture)
{
    auto &files = *a_fixture.files;
    const auto root = path(a_fixture.root.join("Build Path Project"));
    const auto projectFile = path(root.join("Game.cueproj"));
    CHECK(files.create_directories(path(root.join("GameSource"))).has_value());
    CHECK(files.create_directories(path(root.join("Content"))).has_value());
    CHECK(write(files, path(root.join("GameSource/CMakeLists.txt")), "# Entry\n"));
    const auto configured = std::string(k_descriptor) + "gameTarget=Game\n";
    const auto replace = [&](std::string_view a_before, std::string_view a_after)
    {
        auto text = configured;
        text.replace(text.find(a_before), a_before.size(), a_after);
        return text;
    };
    for (const auto *source : {".", "Build", "build/Source", "Intermediate", "INTERMEDIATE/Build/Source",
                               "Build./Source", "Build /Source", "Foo/../Build/Source"})
    {
        CHECK(write(files, projectFile, replace("sourceRoot=GameSource", std::string("sourceRoot=") + source)));
        auto rejected = cue::Project::load(files, projectFile);
        CHECK(!rejected.has_value() && rejected.try_error()->category == cue::ErrorCategory::InvalidArgument &&
              rejected.try_error()->operation == "Project.gameTarget.generated_overlap");
    }
    for (const auto *content : {".", "Build/Content", "intermediate/Content", "Intermediate./Content"})
    {
        CHECK(write(files, projectFile, replace("contentRoot=Content", std::string("contentRoot=") + content)));
        auto rejected = cue::Project::load(files, projectFile);
        CHECK(!rejected.has_value() && rejected.try_error()->operation == "Project.gameTarget.generated_overlap");
    }
    for (const auto *key : {"sourceRoot=GameSource", "contentRoot=Content"})
        for (const auto *suffix : {"Bad;Path", "Bad$<CONFIG>"})
        {
            auto prefix = std::string_view(key).substr(0, std::string_view(key).find('=') + 1);
            CHECK(write(files, projectFile, replace(key, std::string(prefix) + suffix)));
            auto rejected = cue::Project::load(files, projectFile);
            CHECK(!rejected.has_value() && rejected.try_error()->operation == "Project.gameTarget.cmake_path");
        }
    // CMake の記号制限は表示名へ適用せず、従来の UTF-8 表示名をそのまま保持する
    CHECK(write(files, projectFile, replace("name=Game Project", "name=Game; $<Display>")));
    CHECK(cue::Project::load(files, projectFile).has_value());
    // Game 指定のない従来 Project では生成先の予約名や ';' を追加制限しない
    auto legacy = std::string(k_descriptor);
    legacy.replace(legacy.find("sourceRoot=GameSource"), std::string_view("sourceRoot=GameSource").size(),
                   "sourceRoot=.");
    CHECK(write(files, projectFile, legacy));
    CHECK(cue::Project::load(files, projectFile).has_value());
    const auto semicolonRoot = path(a_fixture.root.join("Root; Project"));
    const auto semicolonFile = path(semicolonRoot.join("Game.cueproj"));
    CHECK(files.create_directories(path(semicolonRoot.join("GameSource"))).has_value());
    CHECK(files.create_directories(path(semicolonRoot.join("Content"))).has_value());
    CHECK(write(files, path(semicolonRoot.join("GameSource/CMakeLists.txt")), "# Entry\n"));
    CHECK(write(files, semicolonFile, k_descriptor));
    CHECK(cue::Project::load(files, semicolonFile).has_value());
    CHECK(write(files, semicolonFile, configured));
    auto unsupportedRoot = cue::Project::load(files, semicolonFile);
    CHECK(!unsupportedRoot.has_value() && unsupportedRoot.try_error()->operation == "Project.gameTarget.cmake_path");
    return 0;
}
} // namespace

/// @brief Project の Build 指定を Source の定義と接続し、永続設定に PC 固有 Path を持たせない契約を検証する
int main()
{
    auto result = test_names();
    if (result)
        return result;
    cue::tests::TemporaryFiles fixture;
    CHECK(fixture.isCreated);
    result = test_project(fixture);
    return result ? result : test_build_paths(fixture);
}
