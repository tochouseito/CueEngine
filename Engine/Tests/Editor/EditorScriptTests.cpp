#include <Editor/EditorApplication.h>

#include <cstdio>
#include <thread>

#include "EditorTestSupport.h"

#define CUE_EDITOR_SCRIPT_CHECK(...)                                                                                   \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(__VA_ARGS__))                                                                                            \
        {                                                                                                              \
            std::fprintf(stderr, "Editor Script test line %d: %s\n", __LINE__, #__VA_ARGS__);                          \
            return __LINE__;                                                                                           \
        }                                                                                                              \
    } while (false)

/// @brief Script の割当・編集・削除、Undo / Redo、失敗時の正本維持と Owner Thread を検証する
int main()
{
    using namespace cue;
    using namespace cue::tests;
    unsigned ids = 0;
    bool failWorld = false;
    auto created = EditorApplication::create(std::nullopt, editor_config(ids, &failWorld));
    CUE_EDITOR_SCRIPT_CHECK(created.has_value());
    auto app = created.take_value();
    const auto objectId = editor_value(app->add_cube());
    const auto baseline = editor_value(app->document().scene().serialize());
    ScriptDescriptor script;
    script.instanceId = "3ce1feb2-e711-4ca3-9698-2b32e0d9637c";
    script.typeId = "missing.RotateCube";
    script.properties.push_back({"speed", ScriptPropertyKind::Number, 45, false});
    CUE_EDITOR_SCRIPT_CHECK(app->set_scripts(objectId, {script}).has_value());
    CUE_EDITOR_SCRIPT_CHECK(editor_object(*app, objectId).scripts.front().typeId == "missing.RotateCube");
    CUE_EDITOR_SCRIPT_CHECK(app->document().scene().descriptor().formatVersion == 2);
    CUE_EDITOR_SCRIPT_CHECK(app->undo().has_value() && baseline == editor_value(app->document().scene().serialize()));
    CUE_EDITOR_SCRIPT_CHECK(app->redo().has_value() &&
                            editor_object(*app, objectId).scripts.front().properties.front().number == 45);
    const auto revision = app->document().revision();
    CUE_EDITOR_SCRIPT_CHECK(app->set_scripts(objectId, {script}).has_value());
    CUE_EDITOR_SCRIPT_CHECK(app->document().revision() == revision);

    // 候補 Preview の生成に失敗しても履歴・選択・正本を進めない
    const auto before = editor_value(app->document().scene().serialize());
    auto *world = app->preview_world();
    script.properties.front().number = 90;
    failWorld = true;
    CUE_EDITOR_SCRIPT_CHECK(!app->set_scripts(objectId, {script}).has_value());
    CUE_EDITOR_SCRIPT_CHECK(before == editor_value(app->document().scene().serialize()) &&
                            app->document().revision() == revision && app->preview_world() == world &&
                            app->document().selected_object_id() == objectId);
    failWorld = false;
    CUE_EDITOR_SCRIPT_CHECK(app->set_scripts(objectId, {script}).has_value());
    CUE_EDITOR_SCRIPT_CHECK(app->undo().has_value() &&
                            editor_object(*app, objectId).scripts.front().properties.front().number == 45);
    CUE_EDITOR_SCRIPT_CHECK(app->redo().has_value() &&
                            editor_object(*app, objectId).scripts.front().properties.front().number == 90);
    CUE_EDITOR_SCRIPT_CHECK(app->set_scripts(objectId, {}).has_value() &&
                            editor_object(*app, objectId).scripts.empty());
    CUE_EDITOR_SCRIPT_CHECK(app->undo().has_value() && editor_object(*app, objectId).scripts.size() == 1);
    CUE_EDITOR_SCRIPT_CHECK(!app->set_scripts("missing", {script}).has_value());
    script.instanceId = "invalid";
    CUE_EDITOR_SCRIPT_CHECK(!app->set_scripts(objectId, {script}).has_value());
    CUE_EDITOR_SCRIPT_CHECK(app->begin_transform_edit(objectId).has_value());
    CUE_EDITOR_SCRIPT_CHECK(!app->set_scripts(objectId, {}).has_value());
    CUE_EDITOR_SCRIPT_CHECK(app->cancel_transform_edit().has_value());
    bool wrongThread = false;
    std::thread worker(
        [&]()
        {
            const auto result = app->set_scripts(objectId, {});
            wrongThread = !result.has_value() && result.try_error()->category == ErrorCategory::WrongThread;
        });
    worker.join();
    CUE_EDITOR_SCRIPT_CHECK(wrongThread);
    CUE_EDITOR_SCRIPT_CHECK(app->shutdown().has_value() && !app->set_scripts(objectId, {}).has_value());
    return 0;
}
