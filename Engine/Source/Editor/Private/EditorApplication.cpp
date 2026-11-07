#include <Editor/EditorApplication.h>

#include <algorithm>
#include <chrono>
#include <limits>
#include <new>
#include <utility>

#include <Assets/BuiltinMesh.h>
#include <Foundation/ScopedFlag.h>
#include <Foundation/Uuid.h>
#include <Runtime/Components/TransformComponent.h>
#include <Runtime/World.h>
#include <SceneRuntime/SceneInstantiation.h>

namespace cue
{
namespace
{
/// @brief 名前と属性の正本から Object の位置を検索する
std::size_t find_object(const SceneDescriptor &a_scene, std::string_view a_id) noexcept
{
    for (std::size_t index = 0; index < a_scene.objects.size(); ++index)
        if (a_scene.objects[index].objectId == a_id)
            return index;
    return a_scene.objects.size();
}

/// @brief 保存値の厳密一致を用い、入力していない回転や Scale を変更しない
bool same_transform(const math::floatTransform &a_left, const math::floatTransform &a_right) noexcept
{
    return a_left.position == a_right.position && a_left.rotation.x == a_right.rotation.x &&
           a_left.rotation.y == a_right.rotation.y && a_left.rotation.z == a_right.rotation.z &&
           a_left.rotation.w == a_right.rotation.w && a_left.scale == a_right.scale;
}

/// @brief 編集の呼出元が対処できる状態 Error を作る
Error operation_error(const char *a_operation)
{
    return {ErrorCategory::InvalidState, a_operation};
}

/// @brief 区切り末尾だけを除き、大小文字を区別する FileSystem でも境界を広げない
std::string comparison_path(std::string a_text)
{
    while (a_text.size() > 1 && a_text.back() == '/')
        a_text.pop_back();
    return a_text;
}
} // namespace

struct EditorApplication::PreparedPreview final
{
    std::unique_ptr<World> world;
    SceneInstanceHandle instance;
};

EditorApplication::EditorApplication(CreateToken) noexcept : m_owner(std::this_thread::get_id())
{
}

EditorApplication::~EditorApplication()
{
    // 明示 shutdown で診断を返し、破棄中の Allocation 失敗も最後の World RAII に委ねる
    try
    {
        auto stopped = shutdown();
        (void)stopped;
    }
    catch (...)
    {
    }
}

Result<std::unique_ptr<EditorApplication>> EditorApplication::create(std::optional<Scene> a_initialScene,
                                                                     EditorApplicationConfig a_config)
{
    using AppResult = Result<std::unique_ptr<EditorApplication>>;
    try
    {
        if (a_config.contentRoot && !a_config.contentRoot->is_absolute())
            return AppResult::failure({ErrorCategory::InvalidArgument, "Editor.create.content_root"});
        if (a_config.sceneFile &&
            (!a_config.sceneFile->is_absolute() || a_config.sceneFile->extension() != ".cuescene"))
            return AppResult::failure({ErrorCategory::InvalidArgument, "Editor.create.scene_file"});
        if (a_config.sceneFile && a_config.contentRoot &&
            !comparison_path(a_config.sceneFile->utf8())
                 .starts_with(comparison_path(a_config.contentRoot->utf8()) + '/'))
            return AppResult::failure({ErrorCategory::InvalidArgument, "Editor.create.outside_content"});
        auto app = std::make_unique<EditorApplication>(CreateToken{});
        app->m_config = std::move(a_config);
        const bool hasLoadedScene = a_initialScene.has_value();
        if (!a_initialScene)
        {
            auto created = app->make_new_scene("Untitled");
            if (!created.has_value())
                return AppResult::failure(*created.try_error());
            a_initialScene = created.take_value();
        }
        auto text = a_initialScene->serialize();
        if (!text.has_value())
            return AppResult::failure(*text.try_error());
        // Document の割当と候補 World の生成を公開前に全て終える
        auto document = std::make_unique<SceneDocument>(SceneDocument::ConstructionKey{}, std::move(*a_initialScene),
                                                        text.take_value(), app->m_config.sceneFile, hasLoadedScene);
        auto preview = app->prepare_preview(document->scene());
        if (!preview.has_value())
            return AppResult::failure(*preview.try_error());
        app->m_document = std::move(document);
        app->replace_preview(preview.take_value());
        return AppResult::success(std::move(app));
    }
    catch (const std::bad_alloc &)
    {
        return AppResult::failure({ErrorCategory::PlatformFailure, "Editor.create.allocation"});
    }
    catch (...)
    {
        return AppResult::failure({ErrorCategory::PlatformFailure, "Editor.create.callback"});
    }
}

const SceneDocument &EditorApplication::document() const noexcept
{
    return *m_document;
}
World *EditorApplication::preview_world() noexcept
{
    return std::this_thread::get_id() == m_owner && !m_isStopped ? m_world.get() : nullptr;
}
std::optional<SceneInstanceHandle> EditorApplication::preview_instance() const noexcept
{
    return std::this_thread::get_id() == m_owner && !m_isStopped ? m_instance : std::nullopt;
}

Result<void> EditorApplication::check_access() const
{
    if (std::this_thread::get_id() != m_owner)
        return Result<void>::failure({ErrorCategory::WrongThread, "Editor.owner_thread"});
    if (m_isStopped || m_isMutating)
        return Result<void>::failure(operation_error("Editor.operation_state"));
    if (m_document && m_document->m_revision == (std::numeric_limits<std::uint64_t>::max)())
        return Result<void>::failure(operation_error("Editor.revision_exhausted"));
    return Result<void>::success();
}

Result<EditorApplication::PreparedPreview> EditorApplication::prepare_preview(const Scene &a_scene)
{
    // 描画対象を持つ Document は Preview 可能な Camera を要求し、旧 Document の公開状態を守る
    std::size_t cameras = 0;
    bool hasMesh = false;
    for (const auto &object : a_scene.descriptor().objects)
    {
        if (!object.isActive)
            continue;
        cameras += object.camera.has_value() ? 1 : 0;
        hasMesh = hasMesh || object.mesh.has_value();
    }
    if ((hasMesh && cameras != 1) || cameras > 1)
        return Result<PreparedPreview>::failure({ErrorCategory::InvalidArgument, "Editor.preview.camera_count"});
    auto world = m_config.createWorld ? m_config.createWorld() : World::create();
    if (!world.has_value())
        return Result<PreparedPreview>::failure(*world.try_error());
    auto candidate = world.take_value();
    if (!candidate)
        return Result<PreparedPreview>::failure(operation_error("Editor.preview.null_world"));
    auto instance = instantiate_scene(a_scene, *candidate);
    if (!instance.has_value())
        return Result<PreparedPreview>::failure(*instance.try_error());
    auto initialized = candidate->initialize();
    if (!initialized.has_value())
        return Result<PreparedPreview>::failure(*initialized.try_error());
    return Result<PreparedPreview>::success({std::move(candidate), instance.take_value()});
}

void EditorApplication::replace_preview(PreparedPreview a_preview) noexcept
{
    auto old = std::move(m_world);
    m_world = std::move(a_preview.world);
    m_instance = a_preview.instance;
    // 旧 World の Entity を参照する描画値は存在せず、Snapshot は独立した値を保持する
    if (old)
    {
        auto stopped = old->shutdown();
        (void)stopped;
    }
}

Result<std::string> EditorApplication::generate_id()
{
    if (!m_config.generateId)
        return Result<std::string>::failure(operation_error("Editor.id_generator_missing"));
    auto id = m_config.generateId();
    if (!id.has_value())
        return id;
    if (!is_valid_uuid_text(*id.try_value()))
        return Result<std::string>::failure({ErrorCategory::InvalidArgument, "Editor.id_generator_invalid"});
    return id;
}

Result<Scene> EditorApplication::make_new_scene(std::string_view a_name)
{
    auto sceneId = generate_id();
    if (!sceneId.has_value())
        return Result<Scene>::failure(*sceneId.try_error());
    auto cameraId = generate_id();
    if (!cameraId.has_value())
        return Result<Scene>::failure(*cameraId.try_error());
    SceneDescriptor descriptor;
    descriptor.sceneId = sceneId.take_value();
    descriptor.name = a_name;
    SceneObjectDescriptor camera;
    camera.objectId = cameraId.take_value();
    camera.name = "Camera";
    camera.transform.emplace();
    camera.transform->position.z = 3;
    camera.camera.emplace();
    descriptor.objects.push_back(std::move(camera));
    return Scene::create(std::move(descriptor));
}

Result<void> EditorApplication::new_scene(std::string_view a_name)
{
    auto access = check_access();
    if (!access.has_value())
        return access;
    if (m_document->m_edit)
        return Result<void>::failure(operation_error("Editor.new_scene.editing"));
    const ScopedFlag mutation(m_isMutating);
    try
    {
        auto scene = make_new_scene(a_name);
        if (!scene.has_value())
            return Result<void>::failure(*scene.try_error());
        auto text = scene.try_value()->serialize();
        if (!text.has_value())
            return Result<void>::failure(*text.try_error());
        auto document = std::make_unique<SceneDocument>(SceneDocument::ConstructionKey{}, scene.take_value(),
                                                        text.take_value(), std::nullopt, false);
        document->m_revision = m_document->m_revision + 1;
        auto preview = prepare_preview(document->scene());
        if (!preview.has_value())
            return Result<void>::failure(*preview.try_error());
        replace_preview(preview.take_value());
        m_document = std::move(document);
        return Result<void>::success();
    }
    catch (const std::bad_alloc &)
    {
        return Result<void>::failure({ErrorCategory::PlatformFailure, "Editor.new_scene.allocation"});
    }
    catch (...)
    {
        return Result<void>::failure({ErrorCategory::PlatformFailure, "Editor.new_scene.callback"});
    }
}

Result<Path> EditorApplication::validate_file(IFileSystem &a_files, const Path &a_file, bool a_isSaving)
{
    if (a_file.is_empty() || a_file.extension() != ".cuescene")
        return Result<Path>::failure({ErrorCategory::InvalidArgument, "Editor.scene_file.extension"});
    auto absolute = a_files.absolute_path(a_file);
    if (!absolute.has_value())
        return absolute;
    const Path path = absolute.take_value();
    if (!path.is_absolute())
        return Result<Path>::failure({ErrorCategory::InvalidArgument, "Editor.scene_file.absolute"});
    if (m_config.contentRoot)
    {
        const auto root = comparison_path(m_config.contentRoot->utf8());
        const auto destination = comparison_path(path.utf8());
        if (!destination.starts_with(root + '/'))
            return Result<Path>::failure({ErrorCategory::InvalidArgument, "Editor.scene_file.outside_content"});
    }
    // すべての親を調べ、字句上は Content 内でも Link から外へ出る経路を拒否する
    Path cursor = path;
    bool isLeaf = true;
    while (true)
    {
        auto info = a_files.stat(cursor);
        if (!info.has_value())
            return Result<Path>::failure(*info.try_error());
        if (*info.try_value())
        {
            const auto type = (**info.try_value()).type;
            if (type == FileType::Link || (isLeaf && type != FileType::Regular) ||
                (!isLeaf && type != FileType::Directory))
                return Result<Path>::failure({ErrorCategory::InvalidArgument, "Editor.scene_file.kind"});
        }
        else if (!isLeaf || !a_isSaving)
            return Result<Path>::failure({ErrorCategory::InvalidArgument, "Editor.scene_file.missing"});
        const auto parent = cursor.parent();
        if (parent.utf8() == cursor.utf8())
            break;
        cursor = parent;
        isLeaf = false;
    }
    return Result<Path>::success(path);
}

Result<void> EditorApplication::open_scene(IFileSystem &a_files, const Path &a_file)
{
    auto access = check_access();
    if (!access.has_value())
        return access;
    if (m_document->m_edit)
        return Result<void>::failure(operation_error("Editor.open_scene.editing"));
    const ScopedFlag mutation(m_isMutating);
    try
    {
        auto path = validate_file(a_files, a_file, false);
        if (!path.has_value())
            return Result<void>::failure(*path.try_error());
        auto scene = Scene::load(a_files, *path.try_value());
        if (!scene.has_value())
            return Result<void>::failure(*scene.try_error());
        auto text = scene.try_value()->serialize();
        if (!text.has_value())
            return Result<void>::failure(*text.try_error());
        auto document = std::make_unique<SceneDocument>(SceneDocument::ConstructionKey{}, scene.take_value(),
                                                        text.take_value(), path.take_value(), true);
        document->m_revision = m_document->m_revision + 1;
        auto preview = prepare_preview(document->scene());
        if (!preview.has_value())
            return Result<void>::failure(*preview.try_error());
        replace_preview(preview.take_value());
        m_document = std::move(document);
        return Result<void>::success();
    }
    catch (const std::bad_alloc &)
    {
        return Result<void>::failure({ErrorCategory::PlatformFailure, "Editor.open_scene.allocation"});
    }
    catch (...)
    {
        return Result<void>::failure({ErrorCategory::PlatformFailure, "Editor.open_scene.callback"});
    }
}

Result<FileSaveOutcome> EditorApplication::save_scene(IFileSystem &a_files, const Path &a_file)
{
    auto access = check_access();
    if (!access.has_value())
        return Result<FileSaveOutcome>::failure(*access.try_error());
    if (m_document->m_edit)
        return Result<FileSaveOutcome>::failure(operation_error("Editor.save_scene.editing"));
    const ScopedFlag mutation(m_isMutating);
    try
    {
        auto path = validate_file(a_files, a_file, true);
        if (!path.has_value())
            return Result<FileSaveOutcome>::failure(*path.try_error());
        // 公開後に Allocation 失敗を起こさないよう保存基準を先に所有する
        std::string savedText = m_document->m_text;
        std::optional<Path> savedPath = path.take_value();
        auto saved = a_files.replace_file(*savedPath, std::as_bytes(std::span{savedText}));
        if (!saved.has_value())
            return saved;
        m_document->m_file.swap(savedPath);
        // File の公開成功を編集上の保存基準にする。電源断への耐久性は成功値で別途伝える
        m_document->m_savedText.swap(savedText);
        m_document->m_hasSavedBaseline = true;
        return saved;
    }
    catch (const std::bad_alloc &)
    {
        return Result<FileSaveOutcome>::failure({ErrorCategory::PlatformFailure, "Editor.save_scene.allocation"});
    }
    catch (...)
    {
        return Result<FileSaveOutcome>::failure({ErrorCategory::PlatformFailure, "Editor.save_scene.callback"});
    }
}

Result<std::string> EditorApplication::add_cube()
{
    auto access = check_access();
    if (!access.has_value())
        return Result<std::string>::failure(*access.try_error());
    if (m_document->m_edit)
        return Result<std::string>::failure(operation_error("Editor.add_cube.editing"));
    const ScopedFlag mutation(m_isMutating);
    try
    {
        auto id = generate_id();
        if (!id.has_value())
            return id;
        auto descriptor = m_document->m_scene.descriptor();
        if (find_object(descriptor, *id.try_value()) != descriptor.objects.size())
            return Result<std::string>::failure({ErrorCategory::InvalidArgument, "Editor.add_cube.duplicate_id"});
        SceneObjectDescriptor object;
        object.objectId = *id.try_value();
        object.name = "Cube";
        for (std::size_t suffix = 2; std::any_of(descriptor.objects.begin(), descriptor.objects.end(),
                                                 [&](const auto &a_object) { return a_object.name == object.name; });
             ++suffix)
            object.name = "Cube " + std::to_string(suffix);
        object.transform.emplace();
        object.mesh.emplace();
        SceneDocument::Command command;
        command.kind = SceneDocument::CommandKind::AddCube;
        command.object = object;
        command.selectedBefore = m_document->m_selected;
        command.selectedAfter = object.objectId;
        std::string selected = object.objectId;
        descriptor.objects.push_back(std::move(object));
        auto scene = Scene::create(std::move(descriptor));
        if (!scene.has_value())
            return Result<std::string>::failure(*scene.try_error());
        auto text = scene.try_value()->serialize();
        if (!text.has_value())
            return Result<std::string>::failure(*text.try_error());
        auto preview = prepare_preview(*scene.try_value());
        if (!preview.has_value())
            return Result<std::string>::failure(*preview.try_error());
        replace_preview(preview.take_value());
        m_document->m_scene = scene.take_value();
        m_document->m_text = text.take_value();
        m_document->m_selected.swap(selected);
        m_document->append_command(std::move(command));
        ++m_document->m_revision;
        return id;
    }
    catch (const std::bad_alloc &)
    {
        return Result<std::string>::failure({ErrorCategory::PlatformFailure, "Editor.add_cube.allocation"});
    }
    catch (...)
    {
        return Result<std::string>::failure({ErrorCategory::PlatformFailure, "Editor.add_cube.callback"});
    }
}

Result<void> EditorApplication::select_object(std::string_view a_objectId)
{
    auto access = check_access();
    if (!access.has_value())
        return access;
    if (m_document->m_edit)
        return Result<void>::failure(operation_error("Editor.select_object.editing"));
    if (!a_objectId.empty() &&
        find_object(m_document->m_scene.descriptor(), a_objectId) == m_document->m_scene.descriptor().objects.size())
        return Result<void>::failure({ErrorCategory::InvalidArgument, "Editor.select_object.id"});
    try
    {
        m_document->m_selected = a_objectId;
        return Result<void>::success();
    }
    catch (const std::bad_alloc &)
    {
        return Result<void>::failure({ErrorCategory::PlatformFailure, "Editor.select_object.allocation"});
    }
}

Result<void> EditorApplication::apply_preview_transform(std::string_view a_id, const math::floatTransform &a_value)
{
    if (!m_world || !m_instance)
        return Result<void>::failure(operation_error("Editor.preview.missing"));
    auto entity = m_world->find_by_id(*m_instance, a_id);
    if (!entity.has_value())
        return Result<void>::failure(*entity.try_error());
    auto component = m_world->get_component<TransformComponent>(*entity.try_value());
    if (!component.has_value())
        return Result<void>::failure(*component.try_error());
    auto changed = (*component.try_value())->set_local(a_value);
    if (!changed.has_value())
        return changed;
    auto synchronized = m_world->update(std::chrono::nanoseconds{0});
    if (!synchronized.has_value())
    {
        // Preview は派生値なので、異常失敗した World は編集正本から復元する
        auto recovery = prepare_preview(m_document->m_scene);
        if (recovery.has_value())
            replace_preview(recovery.take_value());
        return synchronized;
    }
    return Result<void>::success();
}

Result<void> EditorApplication::change_transform(std::string_view a_id, const math::floatTransform &a_value)
{
    auto descriptor = m_document->m_scene.descriptor();
    const auto index = find_object(descriptor, a_id);
    if (index == descriptor.objects.size() || !descriptor.objects[index].transform)
        return Result<void>::failure({ErrorCategory::InvalidArgument, "Editor.transform.object"});
    if (same_transform(*descriptor.objects[index].transform, a_value))
        return Result<void>::success();
    descriptor.objects[index].transform = a_value;
    auto scene = Scene::create(std::move(descriptor));
    if (!scene.has_value())
        return Result<void>::failure(*scene.try_error());
    auto text = scene.try_value()->serialize();
    if (!text.has_value())
        return Result<void>::failure(*text.try_error());
    auto applied = apply_preview_transform(a_id, a_value);
    if (!applied.has_value())
        return applied;
    m_document->m_scene = scene.take_value();
    m_document->m_text = text.take_value();
    ++m_document->m_revision;
    return Result<void>::success();
}

Result<void> EditorApplication::set_transform(std::string_view a_objectId, const math::floatTransform &a_transform)
{
    auto access = check_access();
    if (!access.has_value())
        return access;
    if (m_document->m_edit)
        return Result<void>::failure(operation_error("Editor.set_transform.editing"));
    const ScopedFlag mutation(m_isMutating);
    try
    {
        const auto &descriptor = m_document->m_scene.descriptor();
        const auto index = find_object(descriptor, a_objectId);
        if (index == descriptor.objects.size() || !descriptor.objects[index].transform)
            return Result<void>::failure({ErrorCategory::InvalidArgument, "Editor.transform.object"});
        const auto before = *descriptor.objects[index].transform;
        if (same_transform(before, a_transform))
            return Result<void>::success();
        SceneDocument::Command command;
        command.object.objectId = a_objectId;
        command.before = before;
        command.after = a_transform;
        command.selectedBefore = m_document->m_selected;
        command.selectedAfter = m_document->m_selected;
        auto changed = change_transform(a_objectId, a_transform);
        if (!changed.has_value())
            return changed;
        m_document->append_command(std::move(command));
        return Result<void>::success();
    }
    catch (const std::bad_alloc &)
    {
        return Result<void>::failure({ErrorCategory::PlatformFailure, "Editor.transform.allocation"});
    }
    catch (...)
    {
        return Result<void>::failure({ErrorCategory::PlatformFailure, "Editor.transform.callback"});
    }
}

Result<void> EditorApplication::set_scripts(std::string_view a_objectId, std::vector<ScriptDescriptor> a_scripts)
{
    auto access = check_access();
    if (!access.has_value())
        return access;
    if (m_document->m_edit)
        return Result<void>::failure(operation_error("Editor.set_scripts.editing"));
    const ScopedFlag mutation(m_isMutating);
    try
    {
        auto descriptor = m_document->m_scene.descriptor();
        const auto index = find_object(descriptor, a_objectId);
        if (index == descriptor.objects.size())
            return Result<void>::failure({ErrorCategory::InvalidArgument, "Editor.set_scripts.object"});
        SceneDocument::Command command;
        command.kind = SceneDocument::CommandKind::Scripts;
        command.object.objectId = a_objectId;
        command.scriptsBefore = descriptor.objects[index].scripts;
        command.scriptsAfter = a_scripts;
        command.formatBefore = descriptor.formatVersion;
        command.selectedBefore = m_document->m_selected;
        command.selectedAfter = m_document->m_selected;
        descriptor.objects[index].scripts = std::move(a_scripts);
        auto scene = Scene::create(std::move(descriptor));
        if (!scene.has_value())
            return Result<void>::failure(*scene.try_error());
        auto text = scene.try_value()->serialize();
        if (!text.has_value())
            return Result<void>::failure(*text.try_error());
        if (*text.try_value() == m_document->m_text)
            return Result<void>::success();
        command.formatAfter = scene.try_value()->descriptor().formatVersion;
        // Editor の Preview は Script 設定だけを持ち、登録・実行を行わない
        // 未知型も削除せず保存する。候補 World まで完成させてから正本を切り替える
        auto preview = prepare_preview(*scene.try_value());
        if (!preview.has_value())
            return Result<void>::failure(*preview.try_error());
        replace_preview(preview.take_value());
        m_document->m_scene = scene.take_value();
        m_document->m_text = text.take_value();
        m_document->append_command(std::move(command));
        ++m_document->m_revision;
        return Result<void>::success();
    }
    catch (const std::bad_alloc &)
    {
        return Result<void>::failure({ErrorCategory::PlatformFailure, "Editor.set_scripts.allocation"});
    }
    catch (...)
    {
        return Result<void>::failure({ErrorCategory::PlatformFailure, "Editor.set_scripts.callback"});
    }
}

Result<void> EditorApplication::begin_transform_edit(std::string_view a_objectId)
{
    auto access = check_access();
    if (!access.has_value())
        return access;
    if (m_document->m_edit)
        return Result<void>::failure(operation_error("Editor.begin_transform_edit.active"));
    const auto &descriptor = m_document->m_scene.descriptor();
    const auto index = find_object(descriptor, a_objectId);
    if (index == descriptor.objects.size() || !descriptor.objects[index].transform)
        return Result<void>::failure({ErrorCategory::InvalidArgument, "Editor.transform.object"});
    try
    {
        m_document->m_edit =
            SceneDocument::TransformEdit{std::string(a_objectId), *descriptor.objects[index].transform};
        return Result<void>::success();
    }
    catch (const std::bad_alloc &)
    {
        return Result<void>::failure({ErrorCategory::PlatformFailure, "Editor.begin_transform_edit.allocation"});
    }
}

Result<void> EditorApplication::preview_transform(const math::floatTransform &a_transform)
{
    auto access = check_access();
    if (!access.has_value())
        return access;
    if (!m_document->m_edit)
        return Result<void>::failure(operation_error("Editor.preview_transform.inactive"));
    const ScopedFlag mutation(m_isMutating);
    try
    {
        return change_transform(m_document->m_edit->objectId, a_transform);
    }
    catch (const std::bad_alloc &)
    {
        return Result<void>::failure({ErrorCategory::PlatformFailure, "Editor.preview_transform.allocation"});
    }
    catch (...)
    {
        return Result<void>::failure({ErrorCategory::PlatformFailure, "Editor.preview_transform.callback"});
    }
}

Result<void> EditorApplication::commit_transform_edit()
{
    auto access = check_access();
    if (!access.has_value())
        return access;
    if (!m_document->m_edit)
        return Result<void>::failure(operation_error("Editor.commit_transform_edit.inactive"));
    try
    {
        const auto &descriptor = m_document->m_scene.descriptor();
        const auto index = find_object(descriptor, m_document->m_edit->objectId);
        const auto after = *descriptor.objects[index].transform;
        if (!same_transform(m_document->m_edit->before, after))
        {
            SceneDocument::Command command;
            command.object.objectId = m_document->m_edit->objectId;
            command.before = m_document->m_edit->before;
            command.after = after;
            command.selectedBefore = m_document->m_selected;
            command.selectedAfter = m_document->m_selected;
            m_document->append_command(std::move(command));
        }
        m_document->m_edit.reset();
        return Result<void>::success();
    }
    catch (const std::bad_alloc &)
    {
        return Result<void>::failure({ErrorCategory::PlatformFailure, "Editor.commit_transform_edit.allocation"});
    }
}

Result<void> EditorApplication::cancel_transform_edit()
{
    auto access = check_access();
    if (!access.has_value())
        return access;
    if (!m_document->m_edit)
        return Result<void>::failure(operation_error("Editor.cancel_transform_edit.inactive"));
    const ScopedFlag mutation(m_isMutating);
    try
    {
        auto changed = change_transform(m_document->m_edit->objectId, m_document->m_edit->before);
        if (changed.has_value())
            m_document->m_edit.reset();
        return changed;
    }
    catch (const std::bad_alloc &)
    {
        return Result<void>::failure({ErrorCategory::PlatformFailure, "Editor.cancel_transform_edit.allocation"});
    }
    catch (...)
    {
        return Result<void>::failure({ErrorCategory::PlatformFailure, "Editor.cancel_transform_edit.callback"});
    }
}

Result<void> EditorApplication::apply_history(bool a_isRedo)
{
    if (m_document->m_edit || (a_isRedo ? !m_document->can_redo() : !m_document->can_undo()))
        return Result<void>::failure(operation_error("Editor.history.unavailable"));
    const auto index = a_isRedo ? m_document->m_cursor : m_document->m_cursor - 1;
    const auto &command = m_document->m_history[index];
    std::string selected = a_isRedo ? command.selectedAfter : command.selectedBefore;
    if (command.kind == SceneDocument::CommandKind::Transform)
    {
        auto result = change_transform(command.object.objectId, a_isRedo ? command.after : command.before);
        if (!result.has_value())
            return result;
    }
    else
    {
        auto descriptor = m_document->m_scene.descriptor();
        if (command.kind == SceneDocument::CommandKind::Scripts)
        {
            const auto object = find_object(descriptor, command.object.objectId);
            if (object == descriptor.objects.size())
                return Result<void>::failure(operation_error("Editor.history.object_missing"));
            descriptor.objects[object].scripts = a_isRedo ? command.scriptsAfter : command.scriptsBefore;
            // v1 から初めて Script を追加した操作の Undo では保存基準の形式まで戻す
            descriptor.formatVersion = a_isRedo ? command.formatAfter : command.formatBefore;
        }
        else if (a_isRedo)
            descriptor.objects.push_back(command.object);
        else
        {
            const auto object = find_object(descriptor, command.object.objectId);
            if (object == descriptor.objects.size())
                return Result<void>::failure(operation_error("Editor.history.object_missing"));
            descriptor.objects.erase(descriptor.objects.begin() + object);
        }
        auto scene = Scene::create(std::move(descriptor));
        if (!scene.has_value())
            return Result<void>::failure(*scene.try_error());
        auto text = scene.try_value()->serialize();
        if (!text.has_value())
            return Result<void>::failure(*text.try_error());
        auto preview = prepare_preview(*scene.try_value());
        if (!preview.has_value())
            return Result<void>::failure(*preview.try_error());
        replace_preview(preview.take_value());
        m_document->m_scene = scene.take_value();
        m_document->m_text = text.take_value();
        ++m_document->m_revision;
    }
    // 選択だけは履歴後の利用者操作で変わり得るため、存在する ID だけを復元する
    if (!selected.empty() &&
        find_object(m_document->m_scene.descriptor(), selected) == m_document->m_scene.descriptor().objects.size())
        selected.clear();
    m_document->m_selected.swap(selected);
    m_document->m_cursor = a_isRedo ? index + 1 : index;
    return Result<void>::success();
}

Result<void> EditorApplication::undo()
{
    auto access = check_access();
    if (!access.has_value())
        return access;
    const ScopedFlag mutation(m_isMutating);
    try
    {
        return apply_history(false);
    }
    catch (const std::bad_alloc &)
    {
        return Result<void>::failure({ErrorCategory::PlatformFailure, "Editor.undo.allocation"});
    }
    catch (...)
    {
        return Result<void>::failure({ErrorCategory::PlatformFailure, "Editor.undo.callback"});
    }
}
Result<void> EditorApplication::redo()
{
    auto access = check_access();
    if (!access.has_value())
        return access;
    const ScopedFlag mutation(m_isMutating);
    try
    {
        return apply_history(true);
    }
    catch (const std::bad_alloc &)
    {
        return Result<void>::failure({ErrorCategory::PlatformFailure, "Editor.redo.allocation"});
    }
    catch (...)
    {
        return Result<void>::failure({ErrorCategory::PlatformFailure, "Editor.redo.callback"});
    }
}

Result<void> EditorApplication::synchronize_preview()
{
    auto access = check_access();
    if (!access.has_value())
        return access;
    if (!m_world)
        return Result<void>::failure(operation_error("Editor.preview.missing"));
    const ScopedFlag mutation(m_isMutating);
    return m_world->update(std::chrono::nanoseconds{0});
}

Result<void> EditorApplication::shutdown()
{
    if (std::this_thread::get_id() != m_owner)
        return Result<void>::failure({ErrorCategory::WrongThread, "Editor.shutdown.owner"});
    if (m_isMutating)
        return Result<void>::failure(operation_error("Editor.shutdown.reentrant"));
    if (m_isStopped)
        return Result<void>::success();
    const ScopedFlag mutation(m_isMutating);
    if (m_world)
    {
        auto stopped = m_world->shutdown();
        if (!stopped.has_value())
            return stopped;
    }
    m_world.reset();
    m_instance.reset();
    m_isStopped = true;
    return Result<void>::success();
}
} // namespace cue
