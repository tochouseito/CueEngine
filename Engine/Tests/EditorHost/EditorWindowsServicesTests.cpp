#include <set>
#include <span>
#include <string>
#include <string_view>

#include "EditorWindowsServices.h"

#include <Foundation/Uuid.h>

#include "../Support/TemporaryFiles.h"

namespace
{
/// @brief Folder 内の候補数・通常 File 条件を実 FileSystem で検証する
int test_project_folder()
{
    cue::tests::TemporaryFiles fixture;
    if (!fixture.isCreated || !fixture.files)
        return __LINE__;
    auto &files = *fixture.files;
    auto empty = cue::find_project_file(files, fixture.root);
    if (empty.has_value() || empty.try_error()->operation != "Editor.project_folder.no_project")
        return __LINE__;
    if (cue::find_project_file(files, {}).has_value())
        return __LINE__;
    auto first = fixture.root.join("Game.cueproj");
    const std::string_view text = "Project metadata is validated separately";
    if (!first.has_value() || !files.write_all(*first.try_value(), std::as_bytes(std::span(text))).has_value())
        return __LINE__;
    auto single = cue::find_project_file(files, fixture.root);
    if (!single.has_value() || single.try_value()->utf8() != first.try_value()->utf8())
        return __LINE__;
    if (cue::find_project_file(files, *first.try_value()).has_value())
        return __LINE__;
    auto second = fixture.root.join("Other.CUEPROJ");
    if (!second.has_value() || !files.write_all(*second.try_value(), std::as_bytes(std::span(text))).has_value())
        return __LINE__;
    auto multiple = cue::find_project_file(files, fixture.root);
    if (multiple.has_value() || multiple.try_error()->operation != "Editor.project_folder.multiple_projects")
        return __LINE__;
    if (!files.remove(*second.try_value()).has_value())
        return __LINE__;
    auto invalid = fixture.root.join("Directory.cueproj");
    if (!invalid.has_value() || !files.create_directories(*invalid.try_value()).has_value())
        return __LINE__;
    auto directory = cue::find_project_file(files, fixture.root);
    if (directory.has_value() || directory.try_error()->operation != "Editor.project_folder.invalid_file")
        return __LINE__;
    return 0;
}
} // namespace

/// @brief COM 初期化なしで生成した Object ID の Scene 契約と重複の有無を検証する
int main()
{
    if (const auto result = test_project_folder(); result != 0)
        return result;
    std::set<std::string> identifiers;
    for (int index = 0; index < 64; ++index)
    {
        auto identifier = cue::create_editor_object_id();
        if (!identifier.has_value() || !cue::is_valid_uuid_text(*identifier.try_value()))
        {
            return 1;
        }
        if (!identifiers.insert(identifier.take_value()).second)
        {
            return 2;
        }
    }
    return 0;
}
