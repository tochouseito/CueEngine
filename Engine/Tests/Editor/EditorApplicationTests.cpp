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
    auto *world = app->preview_world();
    const auto instance = *app->preview_instance();
    auto entity = world->find_by_id(instance, cubeId);
    CUE_EDITOR_CHECK(entity.has_value());
    auto component = world->get_component<TransformComponent>(*entity.try_value());
    CUE_EDITOR_CHECK(component.has_value());
    auto *transform = *component.try_value();
    math::floatTransform value;
    value.position = {1, 2, 3};
    // Transform の変更は既存 Entity を維持し、行列 Cache まで同期する
    CUE_EDITOR_CHECK(app->set_transform(cubeId, value).has_value());
    CUE_EDITOR_CHECK(world == app->preview_world() && instance == *app->preview_instance());
    CUE_EDITOR_CHECK(transform->local().position == value.position && !transform->is_dirty());
    CUE_EDITOR_CHECK(app->undo().has_value() && editor_object(*app, cubeId).transform->position.x == 0);
    CUE_EDITOR_CHECK(app->redo().has_value() && editor_object(*app, cubeId).transform->position.x == 1);

    const auto beforeText = editor_value(app->document().scene().serialize());
    const auto beforeRevision = app->document().revision();
    value.position.x = std::numeric_limits<float>::quiet_NaN();
    CUE_EDITOR_CHECK(!app->set_transform(cubeId, value).has_value());
    CUE_EDITOR_CHECK(beforeText == editor_value(app->document().scene().serialize()));
    CUE_EDITOR_CHECK(beforeRevision == app->document().revision() && world == app->preview_world());
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
    CUE_EDITOR_CHECK(revision == app->document().revision() && world == app->preview_world());
    CUE_EDITOR_CHECK(app->document().selected_object_id() == cubeId);
    failWorld = false;
    CUE_EDITOR_CHECK(app->select_object({}).has_value());
    CUE_EDITOR_CHECK(!app->select_object("missing").has_value());
    CUE_EDITOR_CHECK(app->document().selected_object_id().empty());

    // Add の Undo / Redo は UUID を再利用し、選択を永続 ID で復元する
    CUE_EDITOR_CHECK(app->new_scene().has_value());
    auto first = editor_value(app->add_cube());
    auto second = editor_value(app->add_cube());
    CUE_EDITOR_CHECK(editor_object(*app, first).name == "Cube");
    CUE_EDITOR_CHECK(editor_object(*app, second).name == "Cube 2");
    failWorld = true;
    CUE_EDITOR_CHECK(!app->undo().has_value() && app->document().selected_object_id() == second);
    failWorld = false;
    CUE_EDITOR_CHECK(app->undo().has_value() && app->document().selected_object_id() == first);
    CUE_EDITOR_CHECK(app->document().scene().descriptor().objects.size() == 2);
    CUE_EDITOR_CHECK(app->redo().has_value() && app->document().selected_object_id() == second);
    CUE_EDITOR_CHECK(editor_object(*app, second).mesh->assetId == editor_object(*app, first).mesh->assetId);

    bool rejected = false;
    std::thread worker(
        [&]()
        {
            auto result = app->add_cube();
            rejected = !result.has_value() && result.try_error()->category == ErrorCategory::WrongThread &&
                       app->preview_world() == nullptr && !app->preview_instance();
        });
    worker.join();
    CUE_EDITOR_CHECK(rejected);
    CUE_EDITOR_CHECK(app->shutdown().has_value() && app->shutdown().has_value());
    CUE_EDITOR_CHECK(app->preview_world() == nullptr && !app->add_cube().has_value());

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
