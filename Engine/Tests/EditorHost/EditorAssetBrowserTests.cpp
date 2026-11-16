#include "EditorAssetBrowser.h"

#include <algorithm>
#include <cstdio>
#include <span>
#include <string_view>

#include <Editor/EditorApplication.h>
#include <EditorIde/ScriptSource.h>
#include <Project/Project.h>

#include "../Support/FileSystemProbe.h"
#include "../Support/TemporaryFiles.h"

namespace
{
using namespace cue;

#define CHECK(...)                                                                                                     \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(__VA_ARGS__))                                                                                            \
        {                                                                                                              \
            std::fprintf(stderr, "Editor Asset Browser check failed at %d: %s\n", __LINE__, #__VA_ARGS__);             \
            return __LINE__;                                                                                           \
        }                                                                                                              \
    } while (false)

/// @brief 既存 Scene を持つテスト用 Project の通常 File だけを作成する
bool write(IFileSystem &a_files, const Path &a_root, std::string_view a_file, std::string_view a_text)
{
    auto path = a_root.join(a_file);
    return path.has_value() && a_files.write_all(*path.try_value(), std::as_bytes(std::span(a_text)), true).has_value();
}

/// @brief Scene の書き換え後に Source Map の公開を失敗させ、巻き戻しを通す
struct FailingMapWrite final : tests::FileSystemProbe
{
    using FileSystemProbe::FileSystemProbe;

    /// @brief Source Map だけに公開失敗を注入する
    Result<FileSaveOutcome> replace_file(const Path &a_path, std::span<const std::byte> a_data) override
    {
        if (a_path.filename() == "ScriptSources.cueide")
            return Result<FileSaveOutcome>::failure({ErrorCategory::PlatformFailure, "Test.asset_map_failure"});
        return FileSystemProbe::replace_file(a_path, a_data);
    }
};
} // namespace

/// @brief 登録済み Script の表示、欠落、影響確認、競合・失敗時の保全と最後の型の削除を検証する
int main()
{
    tests::TemporaryFiles fixture;
    CHECK(fixture.files && fixture.isCreated);
    auto &files = *fixture.files;
    const auto &root = fixture.root;
    constexpr std::string_view projectText =
        "formatVersion=2\nprojectId=05241921-394d-4bd5-bf44-65e53292094b\nname=Asset Test\n"
        "applicationName=AssetTest\nsourceRoot=Source\ncontentRoot=Content\nstartupScene=Scenes/Main.cuescene\n"
        "gameTarget=CueGame_AssetTest\nengineVersion=0.1.0\n";
    constexpr std::string_view sceneText =
        "formatVersion=2\r\nsceneId=5b655272-4ad6-43f4-a90b-1cbb3392eb2d\r\nname=Main\r\n"
        "# Keep this comment\r\n\r\n[object 19ba7840-e9ee-4941-81f9-ca4cfdf8166a]\r\nname=Cube\r\nactive=true\r\n"
        "script.0b0d4772-370c-4631-b355-0a20712e4739.typeId=game.Move\r\n"
        "script.0b0d4772-370c-4631-b355-0a20712e4739.schemaVersion=1\r\n"
        "script.0b0d4772-370c-4631-b355-0a20712e4739.enabled=true\r\n";
    constexpr std::string_view catalogText =
        "formatVersion=2\r\n# Keep this catalog comment\r\nscript.game.Move=Move.cpp\r\n"
        "registration.game.Move=Move\r\n";
    CHECK(write(files, root, "Game.cueproj", projectText));
    CHECK(write(files, root, "Content/Scenes/Main.cuescene", sceneText));
    CHECK(write(files, root, "Content/Scenes/Nested/Other.cuescene", sceneText));
    CHECK(write(files, root, "Source/CMakeLists.txt", "cue_add_game_scripts(CueGame_AssetTest)\n"));
    CHECK(write(files, root, "Source/ScriptSources.cueide", catalogText));
    CHECK(write(files, root, "Source/Move.cpp", "#include \"Move.h\"\n"));
    CHECK(write(files, root, "Source/Move.h", "#pragma once\n"));
    CHECK(write(files, root, "Build/Windows/x64/Development/GameModule.cuebuild", "previous build"));
    auto projectFile = root.join("Game.cueproj");
    CHECK(projectFile.has_value());
    auto project = Project::load(files, *projectFile.try_value());
    CHECK(project.has_value() && project.try_value()->startup_scene_file());
    const auto &game = *project.try_value();
    const auto &sceneFile = *game.startup_scene_file();
    auto loaded = Scene::load(files, sceneFile);
    CHECK(loaded.has_value());
    auto created = EditorApplication::create(loaded.take_value(), {sceneFile, game.content_directory()});
    CHECK(created.has_value());
    auto editor = created.take_value();

    auto assets = scan_editor_assets(files, game);
    CHECK(assets.has_value() && assets.try_value()->size() == 2);
    const auto &source = assets.try_value()->back();
    CHECK(source.assets.size() == 1 && source.assets.front().kind == EditorAssetKind::Script &&
          !source.assets.front().isMissing);
    auto cpp = game.source_directory().join("Move.cpp");
    CHECK(cpp.has_value() && files.remove(*cpp.try_value()).has_value());
    auto missing = scan_editor_assets(files, game);
    CHECK(missing.has_value() && missing.try_value()->back().assets.front().isMissing);
    const auto &object = editor->document().scene().descriptor().objects.front();
    auto editedScripts = object.scripts;
    editedScripts.front().isEnabled = false;
    CHECK(editor->set_scripts(object.objectId, std::move(editedScripts)).has_value());
    auto unsaved = inspect_script_removal(files, game, editor->document(), "game.Move");
    CHECK(unsaved.has_value() && unsaved.try_value()->requiresSceneSave);
    CHECK(editor->undo().has_value());
    auto plan = inspect_script_removal(files, game, editor->document(), "game.Move");
    CHECK(plan.has_value() && plan.try_value()->files.size() == 1 && plan.try_value()->uses.size() == 2 &&
          plan.try_value()->activeScene && !plan.try_value()->requiresSceneSave);
    const auto reload = [&]() { return editor->open_scene(files, sceneFile); };

    // 確認後の外部修正がある場合、別の File も一切変更しない
    CHECK(write(files, root, "Source/Move.h", "#pragma once\n// External edit\n"));
    auto changed = execute_script_removal(files, game, editor->document(), *plan.try_value(), reload);
    CHECK(!changed.has_value() && changed.try_error()->operation == "EditorAssets.removal_changed");
    CHECK(write(files, root, "Source/Move.h", "#pragma once\n"));
    FailingMapWrite failing(files);
    auto failed = execute_script_removal(failing, game, editor->document(), *plan.try_value(), reload);
    CHECK(!failed.has_value() && failed.try_error()->operation == "Test.asset_map_failure");
    auto original = files.read_all(sceneFile);
    CHECK(original.has_value() && std::ranges::equal(*original.try_value(), std::as_bytes(std::span(sceneText))));
    CHECK(editor->document().scene().descriptor().objects.front().scripts.size() == 1);

    auto deleted = execute_script_removal(files, game, editor->document(), *plan.try_value(), reload);
    CHECK(deleted.has_value());
    CHECK(editor->document().scene().descriptor().objects.front().scripts.empty());
    auto map = load_script_source_map(files, game);
    CHECK(map.has_value() && map.try_value()->entries.empty() &&
          map.try_value()->text.find("# Keep") != std::string::npos);
    auto modified = files.read_all(sceneFile);
    const auto expected = sceneText.substr(0, sceneText.find("script."));
    CHECK(modified.has_value() && std::ranges::equal(*modified.try_value(), std::as_bytes(std::span(expected))));
    for (const auto *name : {"Source/Move.h", "Build/Windows/x64/Development/GameModule.cuebuild"})
    {
        auto path = root.join(name);
        CHECK(path.has_value());
        auto info = files.stat(*path.try_value());
        CHECK(info.has_value() && !*info.try_value());
    }
    return 0;
}
