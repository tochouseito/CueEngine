#include <EditorIde/ScriptSource.h>

#include <cstdio>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

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
            std::fprintf(stderr, "Script Source failed %d: %s\n", __LINE__, #__VA_ARGS__);                             \
            return __LINE__;                                                                                           \
        }                                                                                                              \
    } while (false)
/// @brief 検証用の結合だけを所有値にする
cue::Path path(cue::Result<cue::Path> a_value)
{
    if (!a_value.has_value())
        throw std::runtime_error("fixture path");
    return a_value.take_value();
}
/// @brief Fixture の入力だけを書き換える
bool write(cue::IFileSystem &a_files, const cue::Path &a_path, std::string_view a_text)
{
    return a_files.write_all(a_path, std::as_bytes(std::span(a_text.data(), a_text.size())), true).has_value();
}
/// @brief Link の種類と I/O 失敗を Native の権限に依存せず注入する
class Probe final : public cue::tests::FileSystemProbe
{
  public:
    /// @brief Fixture の FileSystem を呼出期間だけ借用する
    explicit Probe(cue::IFileSystem &a_files) : FileSystemProbe(a_files)
    {
    }
    /// @brief Map / Source / Directory のすべての Link を観測する
    cue::Result<std::optional<cue::FileInfo>> stat(const cue::Path &a_path) override
    {
        if (!linkName.empty() && a_path.filename() == linkName)
            return cue::Result<std::optional<cue::FileInfo>>::success(cue::FileInfo{cue::FileType::Link});
        return FileSystemProbe::stat(a_path);
    }
    std::string linkName;
};
} // namespace

/// @brief Editor の型 ID と Source の対応が Runtime / 保存 Scene を変更しないことを確認する
int main()
{
    cue::tests::TemporaryFiles fixture;
    CHECK(fixture.isCreated && fixture.files);
    auto &files = *fixture.files;
    const auto projectFile = path(fixture.root.join("Game.cueproj"));
    const auto map = path(fixture.root.join("Source/ScriptSources.cueide"));
    const auto source = path(fixture.root.join("Source/日本語 Folder/Rotate.cpp"));
    CHECK(files.create_directories(path(fixture.root.join("Content"))).has_value());
    CHECK(write(files, source, "// Fixture source\n"));
    CHECK(write(files, projectFile,
                "formatVersion=1\nprojectId=7f533c49-d416-47ea-98c1-026822b1e6ed\nname=Source "
                "Test\napplicationName=SourceTest\nsourceRoot=Source\ncontentRoot=Content\n"));
    auto project = cue::Project::load(files, projectFile);
    CHECK(project.has_value());
    const std::string valid = "formatVersion=1\nscript.sample.Rotate=日本語 Folder/Rotate.cpp\n";
    CHECK(write(files, map, valid));
    Probe probe(files);
    auto resolved = cue::resolve_script_source(probe, *project.try_value(), "sample.Rotate");
    CHECK(resolved.has_value() && resolved.try_value()->utf8() == source.utf8());
    CHECK(probe.saveCount == 0);
    CHECK(!cue::resolve_script_source(probe, *project.try_value(), "missing").has_value());
    for (const auto &bad :
         {std::string("formatVersion=2\nscript.sample.Rotate=日本語 Folder/Rotate.cpp\n"),
          valid + "script.sample.Rotate=Other.cpp\n", valid + "unknown=x\n",
          std::string("script.sample.Rotate=日本語 Folder/Rotate.cpp\n"),
          std::string("formatVersion=1\nscript.sample.Rotate=../Outside.cpp\n"),
          std::string("formatVersion=1\nscript.sample.Rotate=C:/Outside.cpp\n"),
          std::string("formatVersion=1\nscript.sample.Rotate=/Outside.cpp\n"),
          std::string("formatVersion=1\nscript.sample.Rotate=日本語 Folder/Rotate.cpp:stream\n"),
          std::string("formatVersion=1\nscript.sample.Rotate=日本語 Folder /Rotate.cpp\n"), std::string(65537, 'x')})
    {
        CHECK(write(files, map, bad));
        CHECK(!cue::resolve_script_source(probe, *project.try_value(), "sample.Rotate").has_value());
    }
    CHECK(write(files, map, valid));
    for (const auto *name : {"ScriptSources.cueide", "Rotate.cpp", "日本語 Folder", "Source"})
    {
        probe.linkName = name;
        CHECK(!cue::resolve_script_source(probe, *project.try_value(), "sample.Rotate").has_value());
    }
    probe.linkName.clear();
    probe.failRead = true;
    CHECK(!cue::resolve_script_source(probe, *project.try_value(), "sample.Rotate").has_value());
    probe.failRead = false;
    CHECK(cue::resolve_script_source(probe, *project.try_value(), "sample.Rotate").has_value());
    CHECK(probe.saveCount == 0);
    return 0;
}
