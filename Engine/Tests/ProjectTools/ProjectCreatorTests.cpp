#include <ProjectTools/ProjectCreator.h>

#include <cstdio>
#include <span>
#include <string>
#include <string_view>

#include <Foundation/Windows/UtfConversion.h>
#include <Project/Project.h>
#include <Scene/Scene.h>

#include "../Support/FileSystemProbe.h"
#include "../Support/TemporaryFiles.h"

namespace
{
#define CHECK(...)                                                                                                     \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(__VA_ARGS__))                                                                                            \
        {                                                                                                              \
            std::fprintf(stderr, "Project creation failed at %d: %s\n", __LINE__, #__VA_ARGS__);                       \
            return __LINE__;                                                                                           \
        }                                                                                                              \
    } while (false)

/// @brief Fixture の Path だけを所有値へ変換する
cue::Path path(cue::Result<cue::Path> a_value)
{
    return a_value.take_value();
}

/// @brief 異なる作成呼出しでも重複しない固定 UUID を生成する
cue::projectIdGenerator generator(unsigned &a_counter)
{
    return [&]()
    {
        char id[37]{};
        std::snprintf(id, sizeof(id), "11111111-1111-4111-8111-%012u", ++a_counter);
        return cue::Result<std::string>::success(id);
    };
}

/// @brief 最終公開済み Project と正規 Scene、および ID の独立性を確認する
int test_templates(cue::tests::TemporaryFiles &a_fixture, const cue::Path &a_templates)
{
    auto &files = *a_fixture.files;
    unsigned ids = 0;
    auto generated = generator(ids);
    cue::ProjectCreateDesc descriptor{path(a_fixture.root.join("日本語 Project")), "回転 Game", "123Game",
                                      "0.1.0-preview.1", cue::ProjectTemplate::RotateCube};
    auto created = cue::create_project(files, a_templates, descriptor, generated);
    CHECK(created.has_value() && ids == 6);
    CHECK(created.try_value()->projectFile.utf8() == descriptor.directory.utf8() + "/123Game.cueproj");
    auto project = cue::Project::load(files, created.try_value()->projectFile);
    CHECK(project.has_value() && project.try_value()->descriptor().formatVersion == 2);
    CHECK(project.try_value()->descriptor().engineVersion == "0.1.0-preview.1");
    CHECK(project.try_value()->descriptor().gameTarget == "CueGame_123Game");
    auto scene = cue::Scene::load(files, *project.try_value()->startup_scene_file());
    CHECK(scene.has_value() && scene.try_value()->descriptor().objects.size() == 2);
    const auto &cube = scene.try_value()->descriptor().objects.front();
    CHECK(cube.mesh && cube.scripts.size() == 1 && cube.scripts.front().typeId == "sample.RotateCube");
    CHECK(cube.transform && cube.transform->rotation.x == 0 && cube.transform->rotation.y == 0 &&
          cube.transform->rotation.z == 0 && cube.transform->rotation.w == 1);
    auto before = files.read_all(created.try_value()->projectFile);
    CHECK(before.has_value());
    CHECK(!cue::create_project(files, a_templates, descriptor, generated).has_value());
    CHECK(ids == 6);
    auto after = files.read_all(created.try_value()->projectFile);
    CHECK(after.has_value() && *before.try_value() == *after.try_value());
    descriptor.directory = path(a_fixture.root.join("Empty"));
    descriptor.projectTemplate = cue::ProjectTemplate::Empty3D;
    auto empty = cue::create_project(files, a_templates, descriptor, generated);
    CHECK(empty.has_value() && empty.try_value()->projectId != created.try_value()->projectId);
    auto emptyProject = cue::Project::load(files, empty.try_value()->projectFile);
    CHECK(emptyProject.has_value());
    auto emptyScene = cue::Scene::load(files, *emptyProject.try_value()->startup_scene_file());
    CHECK(emptyScene.has_value() && emptyScene.try_value()->descriptor().objects.size() == 1);
    CHECK(emptyScene.try_value()->descriptor().objects.front().camera);
    CHECK(emptyScene.try_value()->descriptor().objects.front().scripts.empty());
    CHECK(emptyScene.try_value()->descriptor().sceneId != scene.try_value()->descriptor().sceneId);
    return 0;
}

/// @brief 無効な入力と Template では保存先や既存 File を変更しない
int test_invalid(cue::tests::TemporaryFiles &a_fixture, const cue::Path &a_templates)
{
    auto &files = *a_fixture.files;
    unsigned ids = 100;
    auto generated = generator(ids);
    cue::ProjectCreateDesc descriptor{path(a_fixture.root.join("Invalid")), "Game", "Game", "0.1.0"};
    for (const auto *version : {"", "1.2", "01.2.3", "1.2.3-preview.01", "1.2.3+", "1.2.3/evil", "../Game"})
    {
        descriptor.engineVersion = version;
        CHECK(!cue::create_project(files, a_templates, descriptor, generated).has_value());
    }
    CHECK(ids == 100);
    descriptor.engineVersion = "0.1.0";
    descriptor.applicationName = "NUL";
    CHECK(!cue::create_project(files, a_templates, descriptor, generated).has_value());
    descriptor.applicationName = "Game";
    descriptor.name = "Line\nInjected=Value";
    CHECK(!cue::create_project(files, a_templates, descriptor, generated).has_value());
    descriptor.name = "Game";
    descriptor.directory = path(a_fixture.root.join("Alias."));
    CHECK(!cue::create_project(files, a_templates, descriptor, generated).has_value());
    descriptor.directory = path(a_fixture.root.join("Invalid"));
    const auto missing = path(a_fixture.root.join("Missing Templates"));
    CHECK(!cue::create_project(files, missing, descriptor, generated).has_value());
    auto exists = files.exists(descriptor.directory);
    CHECK(exists.has_value() && !*exists.try_value());
    cue::projectIdGenerator repeated = []()
    { return cue::Result<std::string>::success("11111111-1111-4111-8111-111111111111"); };
    CHECK(!cue::create_project(files, a_templates, descriptor, repeated).has_value());
    return 0;
}

/// @brief File 生成途中の失敗では所有要素を回収し、外部 File が混入した場合は残して診断する
int test_rollback(cue::tests::TemporaryFiles &a_fixture, const cue::Path &a_templates)
{
    auto &files = *a_fixture.files;
    unsigned ids = 200;
    auto generated = generator(ids);
    cue::ProjectCreateDesc descriptor{path(a_fixture.root.join("Rollback")), "Game", "Game", "0.1.0"};
    cue::tests::FileSystemProbe probe(files);
    cue::Path staging;
    probe.onOpen = [&](const cue::Path &a_path)
    {
        if (a_path.filename() == ".cue-create-owner")
        {
            staging = a_path.parent();
            probe.failedFilename = "Game.cpp";
        }
    };
    auto failed = cue::create_project(probe, a_templates, descriptor, generated);
    CHECK(!failed.has_value() && !staging.is_empty());
    auto stageExists = files.exists(staging);
    auto destinationExists = files.exists(descriptor.directory);
    CHECK(stageExists.has_value() && !*stageExists.try_value());
    CHECK(destinationExists.has_value() && !*destinationExists.try_value());

    probe.failedFilename.clear();
    cue::Path foreign;
    probe.onOpen = [&](const cue::Path &a_path)
    {
        if (a_path.filename() == ".cue-create-owner")
        {
            staging = a_path.parent();
            foreign = path(staging.join("foreign.txt"));
            const std::string text = "outside owner";
            (void)files.write_all(foreign, std::as_bytes(std::span(text.data(), text.size())));
        }
    };
    auto mixed = cue::create_project(probe, a_templates, descriptor, generated);
    CHECK(!mixed.has_value() && mixed.try_error()->operation.find("cleanup:") != std::string::npos);
    auto retained = files.read_all(foreign);
    CHECK(retained.has_value() && retained.try_value()->size() == 13);
    auto entries = files.list_directory(staging);
    CHECK(entries.has_value() && entries.try_value()->size() == 1);
    return 0;
}

/// @brief 公開直前に保存先が作られても、その内容を置換せず Staging だけを回収する
int test_publish_collision(cue::tests::TemporaryFiles &a_fixture, const cue::Path &a_templates)
{
    auto &files = *a_fixture.files;
    struct Collision final : cue::tests::FileSystemProbe
    {
        /// @brief 最終 Rename の瞬間だけ別の作成者を模擬する
        explicit Collision(cue::IFileSystem &a_files) : FileSystemProbe(a_files)
        {
        }
        /// @brief 保存先に外部 File を作ってから、実 FileSystem の非置換 Rename を呼ぶ
        cue::Result<void> rename(const cue::Path &a_from, const cue::Path &a_to) override
        {
            staging = a_from;
            auto made = files.create_directories(a_to);
            if (!made.has_value())
                return made;
            const auto foreign = path(a_to.join("foreign.txt"));
            const std::string text = "outside owner";
            auto written = files.write_all(foreign, std::as_bytes(std::span(text.data(), text.size())));
            if (!written.has_value())
                return written;
            return files.rename(a_from, a_to);
        }
        cue::Path staging;
    } collision(files);
    unsigned ids = 300;
    cue::ProjectCreateDesc descriptor{path(a_fixture.root.join("Collision")), "Game", "Game", "0.1.0"};
    auto result = cue::create_project(collision, a_templates, descriptor, generator(ids));
    CHECK(!result.has_value() && !collision.staging.is_empty());
    auto retained = files.read_all(path(descriptor.directory.join("foreign.txt")));
    CHECK(retained.has_value() && retained.try_value()->size() == 13);
    auto stageExists = files.exists(collision.staging);
    auto publishedProject = files.exists(path(descriptor.directory.join("Game.cueproj")));
    CHECK(stageExists.has_value() && !*stageExists.try_value());
    CHECK(publishedProject.has_value() && !*publishedProject.try_value());
    return 0;
}

/// @brief v1 の互換性を維持し、v2 の指定欠損と Version 構文不正を拒否する
int test_project_versions(cue::tests::TemporaryFiles &a_fixture)
{
    auto &files = *a_fixture.files;
    const auto root = path(a_fixture.root.join("Version Test"));
    CHECK(files.create_directories(path(root.join("Source"))).has_value());
    CHECK(files.create_directories(path(root.join("Content"))).has_value());
    const auto file = path(root.join("Game.cueproj"));
    const std::string body = "\nprojectId=11111111-1111-4111-8111-111111111111\nname=Game\napplicationName="
                             "Game\nsourceRoot=Source\ncontentRoot=Content\n";
    const auto write = [&](const std::string &a_text)
    { return files.write_all(file, std::as_bytes(std::span(a_text.data(), a_text.size()))).has_value(); };
    CHECK(write("formatVersion=1" + body));
    CHECK(cue::Project::load(files, file).has_value());
    CHECK(write("formatVersion=2" + body));
    CHECK(!cue::Project::load(files, file).has_value());
    CHECK(write("formatVersion=2" + body + "engineVersion=0.1.0-preview.1\n"));
    CHECK(cue::Project::load(files, file).has_value());
    CHECK(write("formatVersion=1" + body + "engineVersion=0.1.0\n"));
    CHECK(!cue::Project::load(files, file).has_value());
    return 0;
}
} // namespace

/// @brief 共通の作成経路を Windows FileSystem と失敗注入で確認する
int wmain(int a_count, wchar_t **a_arguments)
{
    CHECK(a_count == 2);
    auto templateText = cue::utf16_to_utf8(a_arguments[1]);
    CHECK(templateText.has_value());
    auto templates = cue::Path::create(*templateText.try_value());
    CHECK(templates.has_value());
    cue::tests::TemporaryFiles fixture;
    CHECK(fixture.isCreated && fixture.files);
    CHECK(test_templates(fixture, *templates.try_value()) == 0);
    CHECK(test_invalid(fixture, *templates.try_value()) == 0);
    CHECK(test_rollback(fixture, *templates.try_value()) == 0);
    CHECK(test_publish_collision(fixture, *templates.try_value()) == 0);
    CHECK(test_project_versions(fixture) == 0);
    return 0;
}
