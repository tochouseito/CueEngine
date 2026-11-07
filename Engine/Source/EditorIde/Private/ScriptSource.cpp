#include <EditorIde/ScriptSource.h>

#include <algorithm>
#include <optional>
#include <string>
#include <unordered_set>

#include <Foundation/Utf8.h>
#include <Platform/FileSystem.h>
#include <Project/Project.h>

namespace cue
{
namespace
{
/// @brief Source Map の入力失敗を Authoring の診断へ揃える
Error invalid(const char *a_operation)
{
    return {ErrorCategory::InvalidArgument, a_operation};
}
/// @brief Key の区切り以外の文字や行末だけを除き、値内部は解釈しない
std::string_view trim(std::string_view a_text)
{
    const auto first = a_text.find_first_not_of(" \t\r");
    return first == std::string_view::npos ? std::string_view{}
                                           : a_text.substr(first, a_text.find_last_not_of(" \t\r") - first + 1);
}
/// @brief Source の全親要素を確認し、読込時点の Link 参照を拒否する
Result<void> validate_file(IFileSystem &a_files, const Path &a_root, const Path &a_file)
{
    auto root = a_files.stat(a_root);
    if (!root.has_value())
        return Result<void>::failure(*root.try_error());
    if (!*root.try_value() || (**root.try_value()).type != FileType::Directory)
        return Result<void>::failure(invalid("EditorIde.source_root"));
    auto prefix = a_root.utf8();
    if (!prefix.ends_with('/'))
        prefix += '/';
    if (!a_file.utf8().starts_with(prefix))
        return Result<void>::failure(invalid("EditorIde.source_outside_root"));
    for (auto offset = prefix.size();;)
    {
        const auto end = a_file.utf8().find('/', offset);
        auto current = Path::create(end == std::string::npos ? a_file.utf8() : a_file.utf8().substr(0, end));
        if (!current.has_value())
            return Result<void>::failure(*current.try_error());
        auto info = a_files.stat(*current.try_value());
        if (!info.has_value())
            return Result<void>::failure(*info.try_error());
        const auto type = end == std::string::npos ? FileType::Regular : FileType::Directory;
        if (!*info.try_value() || (**info.try_value()).type != type)
            return Result<void>::failure(invalid("EditorIde.source_file_kind"));
        if (end == std::string::npos)
            return Result<void>::success();
        offset = end + 1;
    }
}
/// @brief Portable な相対 Path だけを保存する。Windows Alias / ADS も拒否する
bool valid_relative(std::string_view a_text)
{
    if (a_text.empty() || a_text.size() > 2048 || a_text.find_first_of(":<>\"|?*") != std::string_view::npos)
        return false;
    for (std::size_t begin = 0; begin <= a_text.size();)
    {
        const auto end = a_text.find_first_of("/\\", begin);
        const auto part = a_text.substr(begin, end == std::string_view::npos ? end : end - begin);
        if (part.empty() || part == "." || part == ".." || part.ends_with('.') || part.ends_with(' ') ||
            std::any_of(part.begin(), part.end(), [](unsigned char a_char) { return a_char < 32 || a_char == 127; }))
            return false;
        if (end == std::string_view::npos)
            break;
        begin = end + 1;
    }
    return true;
}
} // namespace

Result<Path> resolve_script_source(IFileSystem &a_files, const Project &a_project, std::string_view a_typeId)
{
    try
    {
        if (a_typeId.empty() || a_typeId.size() > 256 || !is_valid_utf8_text(a_typeId))
            return Result<Path>::failure(invalid("EditorIde.script_type_id"));
        auto map = a_project.source_directory().join("ScriptSources.cueide");
        if (!map.has_value())
            return Result<Path>::failure(*map.try_error());
        auto checked = validate_file(a_files, a_project.root(), *map.try_value());
        if (!checked.has_value())
            return Result<Path>::failure(*checked.try_error());
        auto bytes = a_files.read_all(*map.try_value(), 64 * 1024);
        if (!bytes.has_value())
            return Result<Path>::failure(*bytes.try_error());
        const auto &data = *bytes.try_value();
        std::string_view text(data.empty() ? "" : reinterpret_cast<const char *>(data.data()), data.size());
        if (text.empty() || !is_valid_utf8_text(text))
            return Result<Path>::failure(invalid("EditorIde.source_map_text"));
        if (text.starts_with("\xef\xbb\xbf"))
            text.remove_prefix(3);
        bool hasVersion = false;
        std::unordered_set<std::string> keys;
        std::optional<Path> selected;
        for (std::size_t offset = 0; offset < text.size();)
        {
            const auto end = text.find('\n', offset);
            const auto line = trim(text.substr(offset, end == std::string_view::npos ? end : end - offset));
            offset = end == std::string_view::npos ? text.size() : end + 1;
            if (line.empty() || line.starts_with('#'))
                continue;
            const auto separator = line.find('=');
            if (separator == std::string_view::npos)
                return Result<Path>::failure(invalid("EditorIde.source_map_syntax"));
            const auto key = trim(line.substr(0, separator));
            const auto value = trim(line.substr(separator + 1));
            if (key.empty() || value.empty() || !keys.emplace(key).second || keys.size() > 1025)
                return Result<Path>::failure(invalid("EditorIde.source_map_key"));
            if (key == "formatVersion")
            {
                if (value != "1")
                    return Result<Path>::failure(invalid("EditorIde.source_map_version"));
                hasVersion = true;
                continue;
            }
            if (!key.starts_with("script.") || key.size() <= 7 || key.size() > 263 || !valid_relative(value))
                return Result<Path>::failure(invalid("EditorIde.source_map_entry"));
            auto relative = Path::create(value);
            if (!relative.has_value() || relative.try_value()->is_absolute())
                return Result<Path>::failure(invalid("EditorIde.source_map_path"));
            if (key.substr(7) == a_typeId)
            {
                auto source = a_project.source_directory().join(*relative.try_value());
                if (!source.has_value())
                    return Result<Path>::failure(*source.try_error());
                selected = source.take_value();
            }
        }
        if (!hasVersion || !selected)
            return Result<Path>::failure(
                invalid(hasVersion ? "EditorIde.script_source_not_found" : "EditorIde.source_map_version"));
        checked = validate_file(a_files, a_project.source_directory(), *selected);
        if (!checked.has_value())
            return Result<Path>::failure(*checked.try_error());
        return Result<Path>::success(std::move(*selected));
    }
    catch (...)
    {
        return Result<Path>::failure({ErrorCategory::PlatformFailure, "EditorIde.source_map_exception"});
    }
}
} // namespace cue
