#include "EditorGameSettings.h"

#include <array>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <Foundation/Utf8.h>
#include <Platform/FileSystem.h>
#include <Project/Project.h>

namespace cue
{
namespace
{
constexpr std::size_t k_maxSettingsBytes = 1024;
constexpr std::string_view k_languageFile = "editor/language.txt";

/// @brief 設定の形式と保存先の不正を、運用時に表示できる Error として返す
Error invalid_settings(const char *a_reason)
{
    return {ErrorCategory::InvalidArgument, std::string("EditorGameSettings.") + a_reason};
}

/// @brief File と既存の全祖先を検査し、Link や Directory を経由した別配置への保存を拒否する
/// 問合せと実操作間の外部変更を排他するものではない。親生成後にも再検査する
Result<void> validate_settings_path(IFileSystem &a_files, const Path &a_file)
{
    if (!a_file.is_absolute())
        return Result<void>::failure(invalid_settings("absolute_path_required"));
    auto current = a_file;
    bool isFile = true;
    while (true)
    {
        auto info = a_files.stat(current);
        if (!info.has_value())
            return Result<void>::failure(*info.try_error());
        if (*info.try_value() && (*info.try_value())->type != (isFile ? FileType::Regular : FileType::Directory))
            return Result<void>::failure(invalid_settings("path_type"));
        const auto parent = current.parent();
        if (parent.utf8() == current.utf8())
            break;
        current = parent;
        isFile = false;
    }
    return Result<void>::success();
}

/// @brief 固定の相対 File 名だけを結合し、未検証の外部文字列を Path として使用しない
Result<Path> settings_file(const Project &a_project)
{
    return a_project.root().join("Intermediate/Editor/Settings.cueuser");
}

/// @brief 有界 UTF-8 の既知 Field を一度ずつ読み、別 Project の設定や未知構成を拒否する
Result<GameBuildConfiguration> parse_settings(std::string_view a_text, const Project &a_project)
{
    using ConfigurationResult = Result<GameBuildConfiguration>;
    if (a_text.empty() || !is_valid_utf8_text(a_text))
        return ConfigurationResult::failure(invalid_settings("text"));
    std::array<std::optional<std::string_view>, 3> fields;
    while (!a_text.empty())
    {
        const auto end = a_text.find('\n');
        auto line = a_text.substr(0, end);
        if (end == std::string_view::npos)
            a_text = {};
        else
            a_text.remove_prefix(end + 1);
        if (line.ends_with('\r'))
            line.remove_suffix(1);
        if (line.empty())
            continue;
        const auto equal = line.find('=');
        if (equal == std::string_view::npos)
            return ConfigurationResult::failure(invalid_settings("field"));
        const auto key = line.substr(0, equal);
        const auto value = line.substr(equal + 1);
        const auto index = key == "formatVersion" ? 0 : key == "projectId" ? 1 : key == "gameConfiguration" ? 2 : 3;
        if (index == 3 || fields[index])
            return ConfigurationResult::failure(invalid_settings("unknown_or_duplicate_field"));
        fields[index] = value;
    }
    if (!fields[0] || !fields[1] || !fields[2] || *fields[0] != "1" || *fields[1] != a_project.descriptor().projectId)
        return ConfigurationResult::failure(invalid_settings("identity"));
    for (const auto configuration :
         {GameBuildConfiguration::Debug, GameBuildConfiguration::Development, GameBuildConfiguration::Release})
        if (*fields[2] == game_build_configuration_name(configuration))
            return ConfigurationResult::success(configuration);
    return ConfigurationResult::failure(invalid_settings("configuration"));
}
} // namespace

Result<EditorLanguage> load_editor_language(IFileSystem &a_files, const Path &a_dataRoot)
{
    using LanguageResult = Result<EditorLanguage>;
    try
    {
        auto file = a_dataRoot.join(k_languageFile);
        if (!file.has_value())
            return LanguageResult::failure(*file.try_error());
        const auto &path = *file.try_value();
        auto valid = validate_settings_path(a_files, path);
        if (!valid.has_value())
            return LanguageResult::failure(*valid.try_error());
        auto info = a_files.stat(path);
        if (!info.has_value())
            return LanguageResult::failure(*info.try_error());
        if (!*info.try_value())
            return LanguageResult::success(EditorLanguage::English);

        // 保存した言語コードだけを読み、手編集時の CRLF も受け付ける
        auto bytes = a_files.read_all(path, 16);
        if (!bytes.has_value())
            return LanguageResult::failure(*bytes.try_error());
        const auto &data = *bytes.try_value();
        std::string_view code(data.empty() ? "" : reinterpret_cast<const char *>(data.data()), data.size());
        if (code.ends_with('\n'))
        {
            code.remove_suffix(1);
            if (code.ends_with('\r'))
                code.remove_suffix(1);
        }
        if (code == "en")
            return LanguageResult::success(EditorLanguage::English);
        if (code == "ja")
            return LanguageResult::success(EditorLanguage::Japanese);
        return LanguageResult::failure(invalid_settings("language"));
    }
    catch (const std::bad_alloc &)
    {
        return LanguageResult::failure({ErrorCategory::PlatformFailure, "EditorGameSettings.language.allocation"});
    }
}

Result<void> save_editor_language(IFileSystem &a_files, const Path &a_dataRoot, EditorLanguage a_language)
{
    if (a_language != EditorLanguage::English && a_language != EditorLanguage::Japanese)
        return Result<void>::failure(invalid_settings("language"));
    try
    {
        auto file = a_dataRoot.join(k_languageFile);
        if (!file.has_value())
            return Result<void>::failure(*file.try_error());
        const auto &path = *file.try_value();
        auto valid = validate_settings_path(a_files, path);
        if (!valid.has_value())
            return valid;
        auto created = a_files.create_directories(path.parent());
        if (!created.has_value())
            return created;
        // 親 Directory の生成後も確認し、既存の設定は置換成功まで保持する
        valid = validate_settings_path(a_files, path);
        if (!valid.has_value())
            return valid;
        const std::string_view code = editor_text(a_language, "en\n", "ja\n");
        auto saved = a_files.replace_file(path, std::as_bytes(std::span(code.data(), code.size())));
        return saved.has_value() ? Result<void>::success() : Result<void>::failure(*saved.try_error());
    }
    catch (const std::bad_alloc &)
    {
        return Result<void>::failure({ErrorCategory::PlatformFailure, "EditorGameSettings.language.allocation"});
    }
}

Result<GameBuildConfiguration> load_editor_game_configuration(IFileSystem &a_files, const Project &a_project)
{
    using ConfigurationResult = Result<GameBuildConfiguration>;
    try
    {
        auto file = settings_file(a_project);
        if (!file.has_value())
            return ConfigurationResult::failure(*file.try_error());
        auto valid = validate_settings_path(a_files, *file.try_value());
        if (!valid.has_value())
            return ConfigurationResult::failure(*valid.try_error());
        auto info = a_files.stat(*file.try_value());
        if (!info.has_value())
            return ConfigurationResult::failure(*info.try_error());
        if (!*info.try_value())
            return ConfigurationResult::success(GameBuildConfiguration::Development);
        auto bytes = a_files.read_all(*file.try_value(), k_maxSettingsBytes);
        if (!bytes.has_value())
            return ConfigurationResult::failure(*bytes.try_error());
        const auto &data = *bytes.try_value();
        return parse_settings(
            std::string_view(data.empty() ? "" : reinterpret_cast<const char *>(data.data()), data.size()), a_project);
    }
    catch (const std::bad_alloc &)
    {
        return ConfigurationResult::failure({ErrorCategory::PlatformFailure, "EditorGameSettings.allocation"});
    }
}

Result<void> save_editor_game_configuration(IFileSystem &a_files, const Project &a_project,
                                            GameBuildConfiguration a_configuration)
{
    const auto name = game_build_configuration_name(a_configuration);
    if (name.empty())
        return Result<void>::failure(invalid_settings("configuration"));
    try
    {
        auto file = settings_file(a_project);
        if (!file.has_value())
            return Result<void>::failure(*file.try_error());
        auto valid = validate_settings_path(a_files, *file.try_value());
        if (!valid.has_value())
            return valid;
        const auto text = "formatVersion=1\nprojectId=" + a_project.descriptor().projectId +
                          "\ngameConfiguration=" + std::string(name) + '\n';
        auto created = a_files.create_directories(file.try_value()->parent());
        if (!created.has_value())
            return created;
        valid = validate_settings_path(a_files, *file.try_value());
        if (!valid.has_value())
            return valid;
        auto saved = a_files.replace_file(*file.try_value(), std::as_bytes(std::span(text.data(), text.size())));
        return saved.has_value() ? Result<void>::success() : Result<void>::failure(*saved.try_error());
    }
    catch (const std::bad_alloc &)
    {
        return Result<void>::failure({ErrorCategory::PlatformFailure, "EditorGameSettings.allocation"});
    }
}
} // namespace cue
