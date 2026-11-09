#include "GameWorkspace.h"

#include <cstdio>
#include <span>
#include <string>
#include <string_view>

#include <Foundation/Windows/UtfConversion.h>
#include <Project/Project.h>

#include "../Support/FileSystemProbe.h"
#include "../Support/TemporaryFiles.h"

namespace
{
#define CHECK(...)                                                                                                     \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(__VA_ARGS__))                                                                                            \
        {                                                                                                              \
            std::fprintf(stderr, "Game workspace failed at %d: %s\n", __LINE__, #__VA_ARGS__);                         \
            return __LINE__;                                                                                           \
        }                                                                                                              \
    } while (false)

/// @brief Test の固定 Path を所有値にする
cue::Path path(cue::Result<cue::Path> a_result)
{
    return a_result.take_value();
}

/// @brief Test の小さい UTF-8 File を保存する
bool write(cue::IFileSystem &a_files, const cue::Path &a_path, std::string_view a_text)
{
    return a_files.write_all(a_path, std::as_bytes(std::span(a_text.data(), a_text.size())), true).has_value();
}

/// @brief 呼出側の CHECK のため、読込成功時だけ所有文字列を返す
std::string read(cue::IFileSystem &a_files, const cue::Path &a_path)
{
    auto bytes = a_files.read_all(a_path, 65536);
    if (!bytes.has_value())
        return {};
    const auto &value = *bytes.try_value();
    return {value.empty() ? "" : reinterpret_cast<const char *>(value.data()), value.size()};
}

/// @brief 作成済み Project を使い、入口の保持、SDK 切替、失敗時の新規要素回収を確認する
int test_workspace(cue::tests::TemporaryFiles &a_fixture, const cue::Path &a_engineRoot)
{
    auto &files = *a_fixture.files;
    const auto root = path(a_fixture.root.join("ゲーム Project"));
    CHECK(files.create_directories(path(root.join("Source"))).has_value());
    CHECK(files.create_directories(path(root.join("Content"))).has_value());
    CHECK(write(files, path(root.join("Source/CMakeLists.txt")), "# Workspace fixture game module\n"));
    const auto projectFile = path(root.join("Game.cueproj"));
    CHECK(write(files, projectFile,
                "formatVersion=1\nprojectId=11111111-1111-4111-8111-111111111111\nname=Game\n"
                "applicationName=Game\nsourceRoot=Source\ncontentRoot=Content\ngameTarget=GameModule\n"));
    auto loaded = cue::Project::load(files, projectFile);
    CHECK(loaded.has_value());
    const auto &project = *loaded.try_value();
    const auto ide = path(root.join("Intermediate/IDE/sdk/Windows/x64"));
    const auto player = path(root.join("Intermediate/EditorBuild/sdk/Windows/x64"));
    const auto entry = path(root.join("CMakeLists.txt"));
    const auto shared = path(root.join("CMakePresets.json"));
    const auto user = path(root.join("CMakeUserPresets.json"));
    const auto prepare = [&](cue::IFileSystem &a_system, const cue::Path &a_sdk)
    { return cue::detail::prepare_game_workspace(a_system, project, a_sdk, ide, player, "Windows", "x64"); };

    // 三番目の新規 File に失敗しても、作成済みの入口を残さない
    cue::tests::FileSystemProbe probe(files);
    probe.failedFilename = "CMakeUserPresets.json";
    CHECK(!prepare(probe, a_engineRoot).has_value());
    for (const auto &file : {entry, shared, user})
    {
        auto exists = files.exists(file);
        CHECK(exists.has_value() && !*exists.try_value());
    }
    CHECK(prepare(files, a_engineRoot).has_value());
    CHECK(read(files, entry) == read(files, path(a_engineRoot.join("Templates/GameWorkspace/CMakeLists.txt"))));
    CHECK(read(files, shared) == read(files, path(a_engineRoot.join("Templates/GameWorkspace/CMakePresets.json"))));
    const auto originalUser = read(files, user);
    CHECK(originalUser.find("cueengine.dev/Workspace/1.0") != std::string::npos);
    CHECK(originalUser.find(a_engineRoot.utf8()) != std::string::npos);
    CHECK(originalUser.find(projectFile.utf8()) != std::string::npos);
    CHECK(originalUser.find(ide.utf8()) != std::string::npos && originalUser.find(player.utf8()) != std::string::npos);
    CHECK(originalUser.find("\"CUE_GAME_BUILD_PLAYER\": true") != std::string::npos);
    CHECK(originalUser.find("\"CUE_GAME_BUILD_PLAYER\": false") != std::string::npos);
    CHECK(prepare(files, a_engineRoot).has_value() && read(files, user) == originalUser);

    // 共有 CMake は利用者が編集でき、SDK 選択を更新しても上書きしない
    const std::string customEntry = "# custom entry\n";
    const std::string customShared = "{\"version\":9,\"configurePresets\":[]}\n";
    CHECK(write(files, entry, customEntry) && write(files, shared, customShared));
    const auto otherSdk = path(a_fixture.root.join("別 SDK"));
    CHECK(files.create_directories(otherSdk).has_value());
    CHECK(prepare(files, otherSdk).has_value());
    const auto changedUser = read(files, user);
    CHECK(changedUser.find(otherSdk.utf8()) != std::string::npos);
    CHECK(read(files, entry) == customEntry && read(files, shared) == customShared);
    probe.failedFilename.clear();
    probe.failSave = true;
    CHECK(!prepare(probe, a_engineRoot).has_value() && read(files, user) == changedUser);

    // 管理印のない設定と、他 Project の管理印を持つ設定は保持して失敗する
    const std::string foreign = "{\"version\":9,\"configurePresets\":[]}\n";
    CHECK(write(files, user, foreign));
    auto refused = prepare(files, a_engineRoot);
    CHECK(!refused.has_value() && refused.try_error()->operation == "GameWorkspace.foreign_user_presets");
    CHECK(read(files, user) == foreign && read(files, entry) == customEntry && read(files, shared) == customShared);
    auto otherProject = originalUser;
    const auto id = project.descriptor().projectId;
    otherProject.replace(otherProject.find(id), id.size(), "22222222-2222-4222-8222-222222222222");
    CHECK(write(files, user, otherProject));
    CHECK(!prepare(files, a_engineRoot).has_value() && read(files, user) == otherProject);

    // Directory を保存先 File とみなさず、既存の内容を保つ
    CHECK(files.remove(user).has_value() && files.create_directories(user).has_value());
    CHECK(!prepare(files, a_engineRoot).has_value());
    auto info = files.stat(user);
    CHECK(info.has_value() && *info.try_value() && (**info.try_value()).type == cue::FileType::Directory);
    return 0;
}
} // namespace

/// @brief 実 SDK Template を読むが、書込みは所有する一時 Project だけに限定する
int wmain(int a_count, wchar_t **a_arguments)
{
    CHECK(a_count == 2);
    auto text = cue::utf16_to_utf8(a_arguments[1]);
    CHECK(text.has_value());
    auto engine = cue::Path::create(*text.try_value());
    CHECK(engine.has_value());
    cue::tests::TemporaryFiles fixture;
    CHECK(fixture.isCreated);
    return test_workspace(fixture, *engine.try_value());
}
