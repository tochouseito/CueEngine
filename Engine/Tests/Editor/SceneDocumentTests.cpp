#include <Editor/SceneDocument.h>

#include <cstdio>

#include "EditorTestSupport.h"

#define CUE_DOCUMENT_CHECK(...)                                                                                        \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(__VA_ARGS__))                                                                                            \
        {                                                                                                              \
            std::fprintf(stderr, "Document test line %d: %s\n", __LINE__, #__VA_ARGS__);                               \
            return __LINE__;                                                                                           \
        }                                                                                                              \
    } while (false)

/// @brief Undo 境界・Draft・保存基準・FileSystem 失敗の原子性を検証する
int main()
{
    using namespace cue;
    using namespace cue::tests;
    unsigned ids = 0;
    bool failWorld = false;
    auto created = EditorApplication::create(std::nullopt, editor_config(ids, &failWorld));
    CUE_DOCUMENT_CHECK(created.has_value());
    auto app = created.take_value();
    const auto cubeId = editor_value(app->add_cube());
    EditorMemoryFiles files;
    auto saveFile = editor_value(Path::create("/Content/Scenes/Cube.cuescene"));
    auto otherFile = editor_value(Path::create("/Content/Scenes/Other.cuescene"));
    CUE_DOCUMENT_CHECK(app->save_scene(files, saveFile).has_value() && !app->document().is_dirty());
    CUE_DOCUMENT_CHECK(app->document().file()->utf8() == saveFile.utf8());
    const auto savedText = files.entries[saveFile.utf8()].text;

    auto *world = app->preview_world();
    math::floatTransform value;
    CUE_DOCUMENT_CHECK(app->begin_transform_edit(cubeId).has_value());
    CUE_DOCUMENT_CHECK(!app->document().can_undo() && !app->document().can_redo());
    for (unsigned index = 1; index <= 5; ++index)
    {
        value.position.x = static_cast<float>(index);
        CUE_DOCUMENT_CHECK(app->preview_transform(value).has_value());
    }
    CUE_DOCUMENT_CHECK(app->preview_world() == world && app->document().is_dirty());
    CUE_DOCUMENT_CHECK(!app->add_cube().has_value() && !app->save_scene(files, saveFile).has_value());
    CUE_DOCUMENT_CHECK(app->commit_transform_edit().has_value() && app->document().can_undo());
    // 複数 Frame の Draft は一回で戻り、保存済み内容への復帰で Dirty が消える
    CUE_DOCUMENT_CHECK(app->undo().has_value() && !app->document().is_dirty());
    CUE_DOCUMENT_CHECK(editor_object(*app, cubeId).transform->position.x == 0);
    CUE_DOCUMENT_CHECK(app->redo().has_value() && editor_object(*app, cubeId).transform->position.x == 5);
    CUE_DOCUMENT_CHECK(app->save_scene(files, saveFile).has_value() && !app->document().is_dirty());
    CUE_DOCUMENT_CHECK(app->undo().has_value() && app->document().is_dirty());
    CUE_DOCUMENT_CHECK(app->redo().has_value() && !app->document().is_dirty());

    // Cancel は開始値と保存基準を復元し、Redo を捨てない
    CUE_DOCUMENT_CHECK(app->undo().has_value());
    CUE_DOCUMENT_CHECK(app->begin_transform_edit(cubeId).has_value());
    value.position.x = 99;
    CUE_DOCUMENT_CHECK(app->preview_transform(value).has_value());
    CUE_DOCUMENT_CHECK(app->cancel_transform_edit().has_value() && app->document().can_redo());
    CUE_DOCUMENT_CHECK(editor_object(*app, cubeId).transform->position.x == 0);
    CUE_DOCUMENT_CHECK(app->redo().has_value() && !app->document().is_dirty());

    value.position.x = 6;
    CUE_DOCUMENT_CHECK(app->set_transform(cubeId, value).has_value());
    const auto current = editor_value(app->document().scene().serialize());
    files.failSave = true;
    CUE_DOCUMENT_CHECK(!app->save_scene(files, otherFile).has_value());
    CUE_DOCUMENT_CHECK(app->document().file()->utf8() == saveFile.utf8() && app->document().is_dirty());
    CUE_DOCUMENT_CHECK(!files.entries.contains(otherFile.utf8()));
    files.failSave = false;
    files.isDurabilityConfirmed = false;
    auto unconfirmed = app->save_scene(files, otherFile);
    CUE_DOCUMENT_CHECK(unconfirmed.has_value() && !unconfirmed.try_value()->isDurabilityConfirmed);
    // 公開成功と電源断への耐久性は別契約。通常編集の保存基準は成功した内容へ更新する
    CUE_DOCUMENT_CHECK(app->document().file()->utf8() == otherFile.utf8() && !app->document().is_dirty());
    CUE_DOCUMENT_CHECK(files.entries[otherFile.utf8()].text == current);
    CUE_DOCUMENT_CHECK(app->undo().has_value() && app->document().is_dirty());
    CUE_DOCUMENT_CHECK(app->redo().has_value() && !app->document().is_dirty());
    files.isDurabilityConfirmed = true;
    bool rejectedReentry = false;
    files.onSave = [&]() { rejectedReentry = !app->new_scene().has_value(); };
    CUE_DOCUMENT_CHECK(app->save_scene(files, otherFile).has_value() && rejectedReentry);
    files.onSave = {};
    CUE_DOCUMENT_CHECK(!app->document().is_dirty());

    // 不正な保存先・Link・失敗した読み込みは Document / Preview を維持する
    const unsigned saves = files.saveCount;
    CUE_DOCUMENT_CHECK(!app->save_scene(files, editor_value(Path::create("/ContentOther/a.cuescene"))).has_value());
    CUE_DOCUMENT_CHECK(!app->save_scene(files, editor_value(Path::create("/Content/Scenes/a.txt"))).has_value());
    CUE_DOCUMENT_CHECK(!app->save_scene(files, editor_value(Path::create("/content/Scenes/a.cuescene"))).has_value());
    files.entries["/Content/Link"] = {FileType::Link, {}};
    CUE_DOCUMENT_CHECK(!app->save_scene(files, editor_value(Path::create("/Content/Link/a.cuescene"))).has_value());
    files.entries["/Content/Scenes/link.cuescene"] = {FileType::Link, {}};
    CUE_DOCUMENT_CHECK(
        !app->open_scene(files, editor_value(Path::create("/Content/Scenes/link.cuescene"))).has_value());
    CUE_DOCUMENT_CHECK(files.saveCount == saves);
    files.failRead = true;
    CUE_DOCUMENT_CHECK(!app->open_scene(files, saveFile).has_value());
    files.failRead = false;
    world = app->preview_world();
    failWorld = true;
    CUE_DOCUMENT_CHECK(!app->open_scene(files, saveFile).has_value());
    CUE_DOCUMENT_CHECK(editor_value(app->document().scene().serialize()) == current);
    CUE_DOCUMENT_CHECK(app->document().file()->utf8() == otherFile.utf8() && world == app->preview_world());
    failWorld = false;
    files.entries["/Content/Scenes/bad.cuescene"] = {FileType::Regular, "formatVersion=999\n"};
    CUE_DOCUMENT_CHECK(!app->open_scene(files, editor_value(Path::create("/Content/Scenes/bad.cuescene"))).has_value());
    CUE_DOCUMENT_CHECK(app->open_scene(files, saveFile).has_value());
    CUE_DOCUMENT_CHECK(editor_value(app->document().scene().serialize()) == files.entries[saveFile.utf8()].text);
    CUE_DOCUMENT_CHECK(!app->document().is_dirty() && !app->document().can_undo() &&
                       app->document().selected_object_id().empty());
    CUE_DOCUMENT_CHECK(!savedText.empty());

    // 履歴を無制限に保持せず、128 回より古い状態には戻らない
    for (unsigned index = 1; index <= 140; ++index)
    {
        value.position.x = static_cast<float>(index + 10);
        CUE_DOCUMENT_CHECK(app->set_transform(cubeId, value).has_value());
    }
    unsigned undos = 0;
    while (app->document().can_undo())
    {
        CUE_DOCUMENT_CHECK(app->undo().has_value());
        ++undos;
    }
    CUE_DOCUMENT_CHECK(undos == SceneDocument::k_historyLimit);
    CUE_DOCUMENT_CHECK(editor_object(*app, cubeId).transform->position.x == 22);
    CUE_DOCUMENT_CHECK(app->shutdown().has_value());
    return 0;
}
