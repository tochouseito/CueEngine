#include <Editor/EditorApplication.h>

#include <cstdio>
#include <limits>
#include <thread>

#include "EditorTestSupport.h"

#define CUE_EDITOR_CHECK(...)                                                                                          \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(__VA_ARGS__))                                                                                            \
        {                                                                                                              \
            std::fprintf(stderr, "Editor test line %d: %s\n", __LINE__, #__VA_ARGS__);                                 \
            return __LINE__;                                                                                           \
        }                                                                                                              \
    } while (false)

/// @brief Scene 操作・Preview の同一性・失敗時維持と Owner Thread を検証する
int main()
{
    using namespace cue;
    using namespace cue::tests;
    unsigned ids = 0;
    bool failWorld = false;
    auto created = EditorApplication::create(std::nullopt, editor_config(ids, &failWorld));
    CUE_EDITOR_CHECK(created.has_value());
    auto app = created.take_value();
    CUE_EDITOR_CHECK(app->document().is_dirty() && !app->document().can_undo());
    CUE_EDITOR_CHECK(app->document().file() == nullptr && app->document().selected_object_id().empty());
    CUE_EDITOR_CHECK(app->document().scene().descriptor().objects.size() == 1);
    const auto &camera = app->document().scene().descriptor().objects.front();
    CUE_EDITOR_CHECK(camera.camera && camera.transform && camera.transform->position.z == 3);

    auto cube = app->add_cube();
    CUE_EDITOR_CHECK(cube.has_value());
    const std::string cubeId = cube.take_value();
    CUE_EDITOR_CHECK(app->document().selected_object_id() == cubeId);
    CUE_EDITOR_CHECK(editor_object(*app, cubeId).mesh->assetId == k_cubeMeshId);
    const auto instance = *app->preview_instance();
    auto initialTransform = app->read_preview_transform(cubeId);
    CUE_EDITOR_CHECK(initialTransform.has_value());
    math::floatTransform value;
    value.position = {1, 2, 3};
    // Transform の変更は既存 Entity を維持し、行列 Cache まで同期する
    CUE_EDITOR_CHECK(app->set_transform(cubeId, value).has_value());
    CUE_EDITOR_CHECK(app->has_preview() && instance == *app->preview_instance());
    CUE_EDITOR_CHECK(editor_value(app->read_preview_transform(cubeId)).position == value.position);
    auto preview = app->preview_snapshot();
    CUE_EDITOR_CHECK(preview.has_value() && preview.try_value()->objects.front().world.values[3][0] == 1);
    CUE_EDITOR_CHECK(initialTransform.try_value()->position == math::float3{});
    CUE_EDITOR_CHECK(app->undo().has_value() && editor_object(*app, cubeId).transform->position.x == 0);
    CUE_EDITOR_CHECK(app->redo().has_value() && editor_object(*app, cubeId).transform->position.x == 1);

    const auto beforeText = editor_value(app->document().scene().serialize());
    const auto beforeRevision = app->document().revision();
    value.position.x = std::numeric_limits<float>::quiet_NaN();
    CUE_EDITOR_CHECK(!app->set_transform(cubeId, value).has_value());
    CUE_EDITOR_CHECK(beforeText == editor_value(app->document().scene().serialize()));
    CUE_EDITOR_CHECK(beforeRevision == app->document().revision() && instance == *app->preview_instance());
    value.position.x = 1;
    value.rotation = {0, 0, 0, 0};
    CUE_EDITOR_CHECK(!app->set_transform(cubeId, value).has_value());
    value.rotation = {};
    value.scale = {-1, 0, 2};
    CUE_EDITOR_CHECK(app->set_transform(cubeId, value).has_value());

    // 候補 World の作成失敗は、選択・履歴・内容・Preview を全て保持する
    const auto previous = editor_value(app->document().scene().serialize());
    const auto revision = app->document().revision();
    failWorld = true;
    CUE_EDITOR_CHECK(!app->add_cube().has_value());
    CUE_EDITOR_CHECK(!app->new_scene().has_value());
    CUE_EDITOR_CHECK(previous == editor_value(app->document().scene().serialize()));
    CUE_EDITOR_CHECK(revision == app->document().revision() && instance == *app->preview_instance());
    CUE_EDITOR_CHECK(app->document().selected_object_id() == cubeId);
    failWorld = false;
    CUE_EDITOR_CHECK(app->select_object({}).has_value());
    CUE_EDITOR_CHECK(!app->select_object("missing").has_value());
    CUE_EDITOR_CHECK(app->document().selected_object_id().empty());

    // Add の Undo / Redo は UUID を再利用し、選択を永続 ID で復元する
    CUE_EDITOR_CHECK(app->new_scene().has_value());
    auto first = editor_value(app->add_cube());
    auto second = editor_value(app->add_cube());
    const auto third = editor_value(app->add_cube());
    CUE_EDITOR_CHECK(editor_object(*app, first).name == "Cube");
    CUE_EDITOR_CHECK(editor_object(*app, second).name == "Cube(1)");
    CUE_EDITOR_CHECK(editor_object(*app, third).name == "Cube(2)");
    CUE_EDITOR_CHECK(app->undo().has_value() && app->document().selected_object_id() == second);
    failWorld = true;
    CUE_EDITOR_CHECK(!app->undo().has_value() && app->document().selected_object_id() == second);
    failWorld = false;
    CUE_EDITOR_CHECK(app->undo().has_value() && app->document().selected_object_id() == first);
    CUE_EDITOR_CHECK(app->document().scene().descriptor().objects.size() == 2);
    CUE_EDITOR_CHECK(app->redo().has_value() && app->document().selected_object_id() == second);
    CUE_EDITOR_CHECK(editor_object(*app, second).mesh->assetId == editor_object(*app, first).mesh->assetId);

    ScriptDescriptor script;
    script.instanceId = "3ce1feb2-e711-4ca3-9698-2b32e0d9637c";
    script.typeId = "test.Move";
    CUE_EDITOR_CHECK(app->set_scripts(first, {script}).has_value());
    EditorMemoryFiles files;
    const auto saveFile = editor_value(Path::create("/Content/Scenes/Remove.cuescene"));
    CUE_EDITOR_CHECK(app->save_scene(files, saveFile).has_value());
    const auto beforeDelete = files.entries[saveFile.utf8()].text;
    const auto cameraId = app->document().scene().descriptor().objects.front().objectId;
    const auto deleteRevision = app->document().revision();
    const auto deleteInstance = app->preview_instance();
    // 最後の有効な Camera や候補 World の生成失敗では削除を確定しない
    auto deletedCamera = app->delete_object(cameraId);
    CUE_EDITOR_CHECK(!deletedCamera.has_value() && deletedCamera.try_error()->operation == "Editor.preview.camera_count");
    failWorld = true;
    CUE_EDITOR_CHECK(!app->delete_object(first).has_value());
    failWorld = false;
    CUE_EDITOR_CHECK(!app->delete_object("missing").has_value());
    CUE_EDITOR_CHECK(app->document().revision() == deleteRevision && app->preview_instance() == deleteInstance &&
                     app->document().selected_object_id() == second && !app->document().is_dirty());

    // 右クリック対象が未選択でも削除し、Undo で元の順序と設定を戻す
    CUE_EDITOR_CHECK(app->delete_object(first).has_value() && app->document().selected_object_id() == second);
    CUE_EDITOR_CHECK(app->document().is_dirty() && app->document().scene().descriptor().objects.size() == 2);
    CUE_EDITOR_CHECK(!app->read_preview_transform(first).has_value());
    CUE_EDITOR_CHECK(app->undo().has_value() && !app->document().is_dirty());
    CUE_EDITOR_CHECK(editor_value(app->document().scene().serialize()) == beforeDelete);
    CUE_EDITOR_CHECK(app->read_preview_transform(first).has_value());
    CUE_EDITOR_CHECK(app->redo().has_value() && app->document().scene().descriptor().objects.size() == 2);
    CUE_EDITOR_CHECK(app->delete_object(second).has_value() && app->document().selected_object_id().empty());
    CUE_EDITOR_CHECK(app->undo().has_value() && app->document().selected_object_id() == second);

    bool rejected = false;
    std::thread worker(
        [&]()
        {
            auto result = app->add_cube();
            rejected = !result.has_value() && result.try_error()->category == ErrorCategory::WrongThread &&
                       !app->has_preview() && !app->preview_instance() &&
                       !app->read_preview_transform(first).has_value() && !app->preview_snapshot().has_value();
        });
    worker.join();
    CUE_EDITOR_CHECK(rejected);
    CUE_EDITOR_CHECK(app->shutdown().has_value() && app->shutdown().has_value());
    CUE_EDITOR_CHECK(!app->has_preview() && !app->add_cube().has_value() &&
                     !app->read_preview_transform(first).has_value() && !app->preview_snapshot().has_value());

    // 入力サービスの欠損・不正な UUID・描画できない Camera 構成を公開しない
    CUE_EDITOR_CHECK(!EditorApplication::create(std::nullopt).has_value());
    auto invalidConfig = editor_config(ids);
    invalidConfig.generateId = []() { return Result<std::string>::success("not-a-uuid"); };
    CUE_EDITOR_CHECK(!EditorApplication::create(std::nullopt, std::move(invalidConfig)).has_value());
    auto descriptor = app->document().scene().descriptor();
    descriptor.objects.erase(descriptor.objects.begin());
    auto noCamera = Scene::create(std::move(descriptor));
    CUE_EDITOR_CHECK(noCamera.has_value());
    CUE_EDITOR_CHECK(!EditorApplication::create(noCamera.take_value(), editor_config(ids)).has_value());
    return 0;
}
