#include "EditorAssetBrowser.h"

#include <algorithm>
#include <array>
#include <iterator>
#include <new>
#include <span>
#include <unordered_map>
#include <utility>

#include <Editor/SceneDocument.h>
#include <EditorIde/ScriptSource.h>
#include <Platform/FileSystem.h>
#include <Project/Project.h>
#include <ProjectBuild/GameBuild.h>

namespace cue
{
namespace
{
constexpr std::size_t k_maxEntries = 32768;
constexpr std::size_t k_maxDepth = 64;
constexpr std::size_t k_maxChangeBytes = 64 * 1024 * 1024;

/// @brief 操作失敗を Status に表示可能な Error にする
Error asset_error(std::string a_operation)
{
    return {ErrorCategory::InvalidState, std::move(a_operation)};
}

/// @brief Link をたどらず、存在する親と File の種別を検証する
Result<std::optional<FileInfo>> inspect_file(IFileSystem &a_files, const Path &a_path,
                                             FileType a_type = FileType::Regular)
{
    using InfoResult = Result<std::optional<FileInfo>>;
    if (!a_path.is_absolute())
        return InfoResult::failure(asset_error("EditorAssets.path"));
    auto info = a_files.stat(a_path);
    if (!info.has_value())
        return info;
    if (*info.try_value() && (**info.try_value()).type != a_type)
        return InfoResult::failure(asset_error("EditorAssets.file_kind:" + a_path.utf8()));
    auto path = a_path;
    for (auto parent = path.parent(); parent.utf8() != path.utf8(); parent = path.parent())
    {
        auto entry = a_files.stat(parent);
        if (!entry.has_value())
            return entry;
        if (*entry.try_value() && (**entry.try_value()).type != FileType::Directory)
            return InfoResult::failure(asset_error("EditorAssets.parent_kind:" + parent.utf8()));
        path = std::move(parent);
    }
    return info;
}

/// @brief Windows の通常の Path 表記差で同じ Scene を見落とさない
bool same_path(const Path &a_left, const Path &a_right)
{
    const auto fold = [](unsigned char a_value)
    { return a_value >= 'A' && a_value <= 'Z' ? a_value + ('a' - 'A') : a_value; };
    return std::ranges::equal(a_left.utf8(), a_right.utf8(), {}, fold, fold);
}

/// @brief 既存の命名規約で一組になっている Script Header の Path を返す
Result<Path> script_header(const Path &a_source)
{
    return a_source.parent().join(a_source.stem() + ".h");
}

/// @brief 一覧中の汎用ファイルから専用の Script 項目へまとめる対象か判定する
bool is_script_file(const Path &a_path, const ScriptSourceEntry &a_entry)
{
    if (same_path(a_path, a_entry.source))
        return true;
    auto header = script_header(a_entry.source);
    return header.has_value() && same_path(a_path, *header.try_value());
}

/// @brief 走査を Project 内に限定し、Link と深過ぎる Directory で無制限に再帰しない
Result<EditorAssetFolder> scan_folder(IFileSystem &a_files, const Path &a_path,
                                      std::span<const ScriptSourceEntry> a_scripts, std::size_t a_depth,
                                      std::size_t &a_remaining)
{
    using FolderResult = Result<EditorAssetFolder>;
    if (a_depth == k_maxDepth)
        return FolderResult::failure(asset_error("EditorAssets.folder_depth"));
    auto children = a_files.list_directory(a_path);
    if (!children.has_value())
        return FolderResult::failure(*children.try_error());
    if (children.try_value()->size() > a_remaining)
        return FolderResult::failure(asset_error("EditorAssets.entry_limit"));
    a_remaining -= children.try_value()->size();
    auto paths = children.take_value();
    std::ranges::sort(paths, {}, &Path::utf8);
    EditorAssetFolder folder{a_path, a_path.filename()};
    for (const auto &path : paths)
    {
        auto info = a_files.stat(path);
        if (!info.has_value())
            return FolderResult::failure(*info.try_error());
        if (!*info.try_value() || (**info.try_value()).type == FileType::Link)
            continue;
        if ((**info.try_value()).type == FileType::Directory)
        {
            auto child = scan_folder(a_files, path, a_scripts, a_depth + 1, a_remaining);
            if (!child.has_value())
                return FolderResult::failure(*child.try_error());
            folder.folders.push_back(child.take_value());
        }
        else if (path.filename() != "ScriptSources.cueide" && path.filename() != "CMakeLists.txt" &&
                 !std::ranges::any_of(a_scripts, [&](const auto &a_script) { return is_script_file(path, a_script); }))
        {
            const auto kind = path.extension() == ".cuescene" ? EditorAssetKind::Scene : EditorAssetKind::File;
            folder.assets.push_back({path, path.filename(), kind});
        }
    }
    for (const auto &script : a_scripts)
    {
        if (!same_path(script.source.parent(), a_path))
            continue;
        auto source = a_files.stat(script.source);
        if (!source.has_value())
            return FolderResult::failure(*source.try_error());
        const bool isMissing = !*source.try_value() || (**source.try_value()).type != FileType::Regular;
        folder.assets.push_back(
            {script.source, script.source.stem(), EditorAssetKind::Script, script.typeId, isMissing});
    }
    std::ranges::sort(folder.assets, {}, &EditorAsset::name);
    return FolderResult::success(std::move(folder));
}

/// @brief File 内容を byte 単位で保持し、差分検査と Rollback で同じ原文を使う
Result<std::string> read_text(IFileSystem &a_files, const Path &a_path, std::size_t a_limit)
{
    auto bytes = a_files.read_all(a_path, a_limit);
    if (!bytes.has_value())
        return Result<std::string>::failure(*bytes.try_error());
    const auto &data = *bytes.try_value();
    return Result<std::string>::success(
        std::string(data.empty() ? "" : reinterpret_cast<const char *>(data.data()), data.size()));
}

/// @brief 解析済み File の Key 比較用に両端の空白だけを除く
std::string_view trim_line(std::string_view a_line)
{
    const auto begin = a_line.find_first_not_of(" \t\r\n");
    return begin == std::string_view::npos ? std::string_view{}
                                           : a_line.substr(begin, a_line.find_last_not_of(" \t\r\n") - begin + 1);
}

/// @brief 対象行だけを除き、空白・コメント・改行と BOM を原文のまま保存する
template <typename Predicate> std::string remove_lines(std::string_view a_text, const Predicate &a_remove)
{
    std::string output;
    if (a_text.starts_with("\xef\xbb\xbf"))
    {
        output.append(a_text.substr(0, 3));
        a_text.remove_prefix(3);
    }
    while (!a_text.empty())
    {
        const auto end = a_text.find('\n');
        const auto line = a_text.substr(0, end == std::string_view::npos ? end : end + 1);
        a_text.remove_prefix(line.size());
        if (!a_remove(trim_line(line)))
            output.append(line);
    }
    return output;
}

/// @brief Source Map から対象型の登録対だけを除く
std::string remove_source_lines(std::string_view a_text, std::string_view a_typeId)
{
    const auto sourceKey = "script." + std::string(a_typeId);
    const auto registrationKey = "registration." + std::string(a_typeId);
    return remove_lines(a_text,
                        [&](std::string_view a_line)
                        {
                            if (a_line.starts_with('#'))
                                return false;
                            const auto key = trim_line(a_line.substr(0, a_line.find('=')));
                            return key == sourceKey || key == registrationKey;
                        });
}

/// @brief 同じ Object Section の対象 Instance を識別し、割当行と Property 行だけを除く
std::string remove_scene_lines(std::string_view a_text, const SceneDescriptor &a_scene, std::string_view a_typeId)
{
    std::unordered_map<std::string_view, std::vector<std::string>> prefixes;
    for (const auto &object : a_scene.objects)
        for (const auto &script : object.scripts)
            if (script.typeId == a_typeId)
                prefixes[object.objectId].push_back("script." + script.instanceId + '.');
    auto active = prefixes.end();
    return remove_lines(a_text,
                        [&](std::string_view a_line)
                        {
                            if (a_line.starts_with("[object ") && a_line.ends_with(']'))
                                active = prefixes.find(trim_line(a_line.substr(8, a_line.size() - 9)));
                            if (active == prefixes.end() || !a_line.starts_with("script."))
                                return false;
                            const auto key = trim_line(a_line.substr(0, a_line.find('=')));
                            return std::ranges::any_of(active->second,
                                                       [&](const auto &a_prefix) { return key.starts_with(a_prefix); });
                        });
}

/// @brief 対象型を使う Object と表示用の Scene 名を記録する
bool collect_uses(const SceneDescriptor &a_scene, std::string_view a_typeId, std::string_view a_label,
                  std::vector<std::string> &a_uses)
{
    bool hasChanges = false;
    for (const auto &object : a_scene.objects)
    {
        if (std::ranges::find(object.scripts, a_typeId, &ScriptDescriptor::typeId) == object.scripts.end())
            continue;
        a_uses.push_back(std::string(a_label) + " / " + object.name + " [" + object.objectId + ']');
        hasChanges = true;
    }
    return hasChanges;
}

/// @brief Content 内の通常 Scene を検証し、変更が必要なものだけを候補へ加える
Result<void> inspect_scenes(IFileSystem &a_files, const Path &a_folder, const Path &a_content, const Path *a_open,
                            EditorScriptRemoval &a_removal, std::size_t &a_entries, std::size_t &a_bytes,
                            std::size_t a_depth)
{
    if (a_depth == k_maxDepth)
        return Result<void>::failure(asset_error("EditorAssets.folder_depth"));
    auto children = a_files.list_directory(a_folder);
    if (!children.has_value())
        return Result<void>::failure(*children.try_error());
    auto paths = children.take_value();
    if (paths.size() > a_entries)
        return Result<void>::failure(asset_error("EditorAssets.entry_limit"));
    a_entries -= paths.size();
    std::ranges::sort(paths, {}, &Path::utf8);
    for (const auto &path : paths)
    {
        auto info = a_files.stat(path);
        if (!info.has_value() || !*info.try_value())
            return Result<void>::failure(info.has_value() ? asset_error("EditorAssets.scene_changed")
                                                          : *info.try_error());
        if ((**info.try_value()).type == FileType::Directory)
        {
            auto scanned = inspect_scenes(a_files, path, a_content, a_open, a_removal, a_entries, a_bytes, a_depth + 1);
            if (!scanned.has_value())
                return scanned;
            continue;
        }
        if (path.extension() != ".cuescene")
            continue;
        if ((**info.try_value()).type != FileType::Regular)
            return Result<void>::failure(asset_error("EditorAssets.scene_kind:" + path.utf8()));
        auto text = read_text(a_files, path, Scene::k_maxFileBytes);
        if (!text.has_value())
            return Result<void>::failure(*text.try_error());
        auto scene = Scene::parse(*text.try_value());
        if (!scene.has_value())
            return Result<void>::failure(
                asset_error("EditorAssets.scene:" + path.utf8() + ": " + scene.try_error()->operation));
        const auto &descriptor = scene.try_value()->descriptor();
        const auto label = std::string_view(path.utf8()).substr(a_content.utf8().size() + 1);
        if (!collect_uses(descriptor, a_removal.typeId, label, a_removal.uses))
            continue;
        auto replacement = remove_scene_lines(*text.try_value(), descriptor, a_removal.typeId);
        auto checked = Scene::parse(replacement);
        if (!checked.has_value())
            return Result<void>::failure(*checked.try_error());
        const auto bytes = text.try_value()->size() + replacement.size();
        if (bytes > a_bytes)
            return Result<void>::failure(asset_error("EditorAssets.removal_size"));
        a_bytes -= bytes;
        a_removal.changes.push_back({path, text.take_value(), std::move(replacement)});
        if (a_open && same_path(*a_open, path))
            a_removal.activeScene = path;
    }
    return Result<void>::success();
}

/// @brief 削除予定の通常 File があれば原本を記録する。未存在なら何もしない
Result<void> add_removal(IFileSystem &a_files, const Path &a_path, EditorScriptRemoval &a_removal,
                         std::size_t &a_remaining)
{
    auto info = inspect_file(a_files, a_path);
    if (!info.has_value())
        return Result<void>::failure(*info.try_error());
    if (!*info.try_value())
        return Result<void>::success();
    auto text = read_text(a_files, a_path, a_remaining);
    if (!text.has_value())
        return Result<void>::failure(*text.try_error());
    a_remaining -= text.try_value()->size();
    a_removal.changes.push_back({a_path, text.take_value(), std::nullopt});
    return Result<void>::success();
}

/// @brief File の公開前に内容を再照合し、外部編集された File を上書きしない
bool matches(IFileSystem &a_files, const Path &a_path, const std::string *a_expected)
{
    auto info = inspect_file(a_files, a_path);
    if (!info.has_value())
        return false;
    if (!a_expected)
        return !*info.try_value();
    if (!*info.try_value() || (**info.try_value()).size != a_expected->size())
        return false;
    auto current = read_text(a_files, a_path, a_expected->size());
    return current.has_value() && *current.try_value() == *a_expected;
}

/// @brief 単一 File の完成内容を既存の原子的書込み入口へ渡す
Result<void> replace_asset_file(IFileSystem &a_files, const Path &a_path, const std::string &a_text)
{
    auto saved = a_files.replace_file(a_path, std::as_bytes(std::span(a_text.data(), a_text.size())));
    return saved.has_value() ? Result<void>::success() : Result<void>::failure(*saved.try_error());
}

/// @brief 作成・Build・削除が同じ成果物を変更する間だけ排他する
Result<std::unique_ptr<IFile>> lock_asset_operation(IFileSystem &a_files, const Project &a_project,
                                                    std::string_view a_relativePath)
{
    auto path = a_project.root().join(a_relativePath);
    if (!path.has_value())
        return Result<std::unique_ptr<IFile>>::failure(*path.try_error());
    auto kind = inspect_file(a_files, *path.try_value());
    if (!kind.has_value())
        return Result<std::unique_ptr<IFile>>::failure(*kind.try_error());
    auto parents = a_files.create_directories(path.try_value()->parent());
    if (!parents.has_value())
        return Result<std::unique_ptr<IFile>>::failure(*parents.try_error());
    kind = inspect_file(a_files, *path.try_value());
    if (!kind.has_value())
        return Result<std::unique_ptr<IFile>>::failure(*kind.try_error());
    return a_files.open(*path.try_value(), {FileAccess::ReadWrite, FileCreation::OpenAlways, false, false, false});
}
} // namespace

Result<std::vector<EditorAssetFolder>> scan_editor_assets(IFileSystem &a_files, const Project &a_project)
{
    using ScanResult = Result<std::vector<EditorAssetFolder>>;
    try
    {
        std::vector<EditorAssetFolder> folders;
        const auto &source = a_project.source_directory();
        auto mapPath = source.join("ScriptSources.cueide");
        if (!mapPath.has_value())
            return ScanResult::failure(*mapPath.try_error());
        auto info = inspect_file(a_files, *mapPath.try_value());
        if (!info.has_value())
            return ScanResult::failure(*info.try_error());
        std::vector<ScriptSourceEntry> scripts;
        if (*info.try_value())
        {
            auto map = load_script_source_map(a_files, a_project);
            if (!map.has_value())
                return ScanResult::failure(*map.try_error());
            scripts = std::move(map.try_value()->entries);
        }
        std::size_t budget = k_maxEntries;
        for (const Path *root : {&a_project.content_directory(), &source})
        {
            auto checked = inspect_file(a_files, *root, FileType::Directory);
            if (!checked.has_value())
                return ScanResult::failure(*checked.try_error());
            if (!*checked.try_value())
                continue;
            const auto entries =
                root == &source ? std::span<const ScriptSourceEntry>(scripts) : std::span<const ScriptSourceEntry>{};
            auto folder = scan_folder(a_files, *root, entries, 0, budget);
            if (!folder.has_value())
                return ScanResult::failure(*folder.try_error());
            folders.push_back(folder.take_value());
        }
        // Subfolder ごと外部で消された Script も、復旧・削除の入口を Source Root に残す
        if (!folders.empty() && same_path(folders.back().path, source))
            for (const auto &script : scripts)
            {
                auto parent = inspect_file(a_files, script.source.parent(), FileType::Directory);
                if (!parent.has_value() || !*parent.try_value())
                    folders.back().assets.push_back(
                        {script.source, script.source.stem(), EditorAssetKind::Script, script.typeId, true});
            }
        return ScanResult::success(std::move(folders));
    }
    catch (const std::bad_alloc &)
    {
        return ScanResult::failure(asset_error("EditorAssets.scan_allocation"));
    }
}

Result<EditorScriptRemoval> inspect_script_removal(IFileSystem &a_files, const Project &a_project,
                                                   const SceneDocument &a_document, std::string_view a_typeId)
{
    using RemovalResult = Result<EditorScriptRemoval>;
    try
    {
        auto map = load_script_source_map(a_files, a_project);
        if (!map.has_value())
            return RemovalResult::failure(*map.try_error());
        const auto &catalog = *map.try_value();
        const auto script = std::ranges::find(catalog.entries, a_typeId, &ScriptSourceEntry::typeId);
        if (catalog.version != 2 || script == catalog.entries.end())
            return RemovalResult::failure(asset_error("EditorAssets.script_registration"));

        EditorScriptRemoval removal;
        removal.typeId = a_typeId;
        removal.name = script->source.stem();
        const auto &content = a_project.content_directory();
        auto contentInfo = inspect_file(a_files, content, FileType::Directory);
        if (!contentInfo.has_value())
            return RemovalResult::failure(*contentInfo.try_error());
        std::size_t entries = k_maxEntries;
        std::size_t bytes = k_maxChangeBytes;
        if (*contentInfo.try_value())
        {
            auto inspected = inspect_scenes(a_files, content, content, a_document.file(), removal, entries, bytes, 0);
            if (!inspected.has_value())
                return RemovalResult::failure(*inspected.try_error());
        }
        const auto &current = a_document.scene().descriptor();
        std::vector<std::string> openUses;
        const bool isCurrentAffected = collect_uses(current, a_typeId, current.name, openUses);
        removal.requiresSceneSave = a_document.is_dirty() && (isCurrentAffected || removal.activeScene.has_value());
        if (isCurrentAffected && (removal.requiresSceneSave || !removal.activeScene))
            removal.uses.insert(removal.uses.end(), std::make_move_iterator(openUses.begin()),
                                std::make_move_iterator(openUses.end()));
        if (isCurrentAffected && a_document.file() && !removal.activeScene)
            removal.activeScene = *a_document.file();
        removal.changes.push_back({catalog.file, catalog.text, remove_source_lines(catalog.text, a_typeId)});

        auto header = script_header(script->source);
        if (!header.has_value())
            return RemovalResult::failure(*header.try_error());
        for (const auto &path : {script->source, header.take_value()})
        {
            auto added = add_removal(a_files, path, removal, bytes);
            if (!added.has_value())
                return RemovalResult::failure(*added.try_error());
            if (removal.changes.back().path.utf8() == path.utf8())
                removal.files.push_back(path.utf8());
        }
        // ソース削除後は構成ごとの旧 DLL を成功済み成果物として再利用しない
        for (const auto configuration :
             {GameBuildConfiguration::Debug, GameBuildConfiguration::Development, GameBuildConfiguration::Release})
        {
            auto build = game_build_directory(a_project, configuration);
            if (!build.has_value())
                return RemovalResult::failure(*build.try_error());
            auto manifest = build.try_value()->join("GameModule.cuebuild");
            if (!manifest.has_value())
                return RemovalResult::failure(*manifest.try_error());
            auto added = add_removal(a_files, *manifest.try_value(), removal, bytes);
            if (!added.has_value())
                return RemovalResult::failure(*added.try_error());
        }
        return RemovalResult::success(std::move(removal));
    }
    catch (const std::bad_alloc &)
    {
        return RemovalResult::failure(asset_error("EditorAssets.removal_allocation"));
    }
}

Result<void> execute_script_removal(IFileSystem &a_files, const Project &a_project, const SceneDocument &a_document,
                                    const EditorScriptRemoval &a_removal, const std::function<Result<void>()> &a_reload)
{
    const auto fail = [&](const char *a_operation) { return Result<void>::failure(asset_error(a_operation)); };
    auto buildLock = lock_asset_operation(a_files, a_project, "Intermediate/.cue-editor-build.lock");
    if (!buildLock.has_value())
        return Result<void>::failure(*buildLock.try_error());
    auto locked = lock_asset_operation(a_files, a_project, "Intermediate/Editor/ScriptCreation.lock");
    if (!locked.has_value())
        return Result<void>::failure(*locked.try_error());
    auto latest = inspect_script_removal(a_files, a_project, a_document, a_removal.typeId);
    if (!latest.has_value())
        return Result<void>::failure(*latest.try_error());
    const auto &current = *latest.try_value();
    const auto &changes = a_removal.changes;
    if (current.requiresSceneSave || current.uses != a_removal.uses ||
        !std::ranges::equal(current.changes, changes,
                            [](const auto &a_left, const auto &a_right)
                            {
                                return a_left.path.utf8() == a_right.path.utf8() &&
                                       a_left.original == a_right.original && a_left.replacement == a_right.replacement;
                            }))
        return fail("EditorAssets.removal_changed");

    std::size_t applied = 0;
    // 実際に公開した File だけを逆順で復元する。途中で書き換わった File は保全して診断に残す
    const auto rollback = [&](Error a_error)
    {
        while (applied)
        {
            const auto &change = changes[--applied];
            if (!matches(a_files, change.path, change.replacement ? &*change.replacement : nullptr))
                a_error.operation += ";rollback:EditorAssets.concurrent_change:" + change.path.utf8();
            else if (auto restored = replace_asset_file(a_files, change.path, change.original); !restored.has_value())
                a_error.operation += ";rollback:" + change.path.utf8() + ": " + restored.try_error()->operation;
        }
        return Result<void>::failure(std::move(a_error));
    };
    try
    {
        for (const auto &change : changes)
        {
            if (!matches(a_files, change.path, &change.original))
                return rollback(asset_error("EditorAssets.concurrent_change:" + change.path.utf8()));
            if (change.replacement)
            {
                auto replaced = replace_asset_file(a_files, change.path, *change.replacement);
                if (!replaced.has_value())
                    return rollback(*replaced.try_error());
            }
            else
            {
                auto removed = a_files.remove(change.path);
                if (!removed.has_value() || !*removed.try_value())
                    return rollback(removed.has_value() ? asset_error("EditorAssets.removal_changed")
                                                        : *removed.try_error());
            }
            ++applied;
        }
        auto reloaded = a_reload();
        if (!reloaded.has_value())
            return rollback(*reloaded.try_error());
        return Result<void>::success();
    }
    catch (const std::bad_alloc &)
    {
        return rollback(asset_error("EditorAssets.removal_allocation"));
    }
}
} // namespace cue
