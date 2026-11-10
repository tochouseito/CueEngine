#include "GameWorkspace.h"

#include <cstddef>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <Foundation/Utf8.h>
#include <Platform/FileSystem.h>
#include <Project/Project.h>

namespace cue::detail
{
namespace
{
constexpr std::string_view k_vendor = "cueengine.dev/Workspace/1.0";
constexpr std::size_t k_maxWorkspaceBytes = 64 * 1024;

/// @brief 未作成の出力も含め、既存の親が通常 Directory であることを確認する
Result<void> validate_parent_directories(IFileSystem &a_files, const Path &a_path)
{
    if (!a_path.is_absolute())
        return Result<void>::failure({ErrorCategory::InvalidArgument, "GameWorkspace.absolute_path"});
    auto current = a_path;
    for (;;)
    {
        auto info = a_files.stat(current);
        if (!info.has_value())
            return Result<void>::failure(*info.try_error());
        if (*info.try_value() && (**info.try_value()).type != FileType::Directory)
            return Result<void>::failure({ErrorCategory::InvalidArgument, "GameWorkspace.directory_kind"});
        const auto parent = current.parent();
        if (parent.utf8() == current.utf8())
            return Result<void>::success();
        current = parent;
    }
}

/// @brief 文字列を JSON の所有値へ変換し、Path の引用符や制御文字を構文として扱わせない
std::string quoted(std::string_view a_text)
{
    constexpr char digits[] = "0123456789abcdef";
    std::string output = "\"";
    for (const auto character : a_text)
    {
        const auto value = static_cast<unsigned char>(character);
        if (character == '"' || character == '\\')
        {
            output += '\\';
            output += character;
        }
        else if (value < 32)
        {
            output += "\\u00";
            output += digits[value >> 4];
            output += digits[value & 15];
        }
        else
            output += character;
    }
    output += '"';
    return output;
}

/// @brief Preset の Macro として Path を再解釈する入力を受け付けない
bool is_preset_path(const Path &a_path)
{
    const auto &text = a_path.utf8();
    return a_path.is_absolute() && text.find("${") == std::string::npos && text.find("$env{") == std::string::npos &&
           text.find("$penv{") == std::string::npos;
}

/// @brief 固定名の通常 File だけを有界取得し、Link と不正 UTF-8 を持ち込まない
Result<std::string> read_text(IFileSystem &a_files, const Path &a_path)
{
    auto valid = validate_parent_directories(a_files, a_path.parent());
    if (!valid.has_value())
        return Result<std::string>::failure(*valid.try_error());
    auto info = a_files.stat(a_path);
    if (!info.has_value())
        return Result<std::string>::failure(*info.try_error());
    if (!*info.try_value() || (**info.try_value()).type != FileType::Regular)
        return Result<std::string>::failure({ErrorCategory::InvalidArgument, "GameWorkspace.file_kind"});
    auto bytes = a_files.read_all(a_path, k_maxWorkspaceBytes);
    if (!bytes.has_value())
        return Result<std::string>::failure(*bytes.try_error());
    const auto &data = *bytes.try_value();
    std::string text(data.empty() ? "" : reinterpret_cast<const char *>(data.data()), data.size());
    if (!is_valid_utf8_text(text))
        return Result<std::string>::failure({ErrorCategory::InvalidArgument, "GameWorkspace.utf8"});
    return Result<std::string>::success(std::move(text));
}

/// @brief 新規保存先を先に予約し、既存 File を競合で置換しない
Result<void> write_new(IFileSystem &a_files, const Path &a_path, std::string_view a_text, bool &a_created)
{
    auto opened = a_files.open(a_path, {FileAccess::Write, FileCreation::CreateNew});
    if (!opened.has_value())
        return Result<void>::failure(*opened.try_error());
    a_created = true;
    auto file = opened.take_value();
    auto remaining = std::as_bytes(std::span(a_text.data(), a_text.size()));
    while (!remaining.empty())
    {
        auto written = file->write(remaining);
        if (!written.has_value())
            return Result<void>::failure(*written.try_error());
        if (*written.try_value() == 0 || *written.try_value() > remaining.size())
            return Result<void>::failure({ErrorCategory::PlatformFailure, "GameWorkspace.short_write"});
        remaining = remaining.subspan(*written.try_value());
    }
    auto flushed = file->flush();
    if (!flushed.has_value())
        return flushed;
    return file->close();
}

/// @brief Editor と Visual Studio が同じ SDK・構成・生成先を使うローカル Preset を作る
std::string user_presets(const Project &a_project, const Path &a_engineRoot, const Path &a_ideTree,
                         const Path &a_appTree, std::string_view a_platform, std::string_view a_architecture)
{
    std::string text = "{\n    \"version\": 9,\n    \"vendor\": {\n        " + quoted(k_vendor) +
                       ": {\n            \"projectId\": " + quoted(a_project.descriptor().projectId) +
                       "\n        }\n    },\n    \"configurePresets\": [\n";
    const auto append = [&](std::string_view a_name, const Path &a_tree, bool a_isApp)
    {
        text +=
            "        {\n            \"name\": " + quoted(a_name) +
            ",\n            \"inherits\": \"windows-vs2026\",\n            \"binaryDir\": " + quoted(a_tree.utf8()) +
            ",\n            \"cacheVariables\": {\n                \"CUE_ENGINE_ROOT\": " +
            quoted(a_engineRoot.utf8()) +
            ",\n                \"CUE_GAME_PROJECT\": " + quoted(a_project.file().utf8()) +
            ",\n                \"CUE_GAME_PLATFORM\": " + quoted(a_platform) +
            ",\n                \"CUE_GAME_ARCHITECTURE\": " + quoted(a_architecture) +
            ",\n                \"CUE_GAME_BUILD_APP\": " + (a_isApp ? "true" : "false") +
            "\n            }\n        }";
    };
    append("cue-engine", a_ideTree, false);
    text += ",\n";
    append("cue-app", a_appTree, true);
    text += "\n    ],\n    \"buildPresets\": [\n";
    bool isFirst = true;
    for (const auto *preset : {"cue-engine", "cue-app"})
    {
        for (const auto *configuration : {"Debug", "Development", "Release"})
        {
            if (!isFirst)
                text += ",\n";
            isFirst = false;
            text += "        { \"name\": " + quoted(std::string(preset) + '-' + configuration) +
                    ", \"configurePreset\": " + quoted(preset) + ", \"configuration\": " + quoted(configuration) + " }";
        }
    }
    return text + "\n    ]\n}\n";
}
} // namespace

Result<void> prepare_game_workspace(IFileSystem &a_files, const Project &a_project, const Path &a_engineRoot,
                                    const Path &a_ideTree, const Path &a_appTree, std::string_view a_platform,
                                    std::string_view a_architecture)
{
    struct PendingFile
    {
        Path path;
        std::string text;
        bool isCreated = false;
    };
    std::vector<PendingFile> pending;
    // この呼出しで新規予約した通常 File だけを回収し、利用者の共有設定には触れない
    const auto fail = [&](Error a_error)
    {
        for (auto item = pending.rbegin(); item != pending.rend(); ++item)
            if (item->isCreated)
            {
                auto removed = a_files.remove(item->path);
                if (!removed.has_value())
                    a_error.operation += ";cleanup:" + removed.try_error()->operation;
            }
        return Result<void>::failure(std::move(a_error));
    };
    try
    {
        if (!is_preset_path(a_project.file()) || !is_preset_path(a_engineRoot) || !is_preset_path(a_ideTree) ||
            !is_preset_path(a_appTree))
            return Result<void>::failure({ErrorCategory::InvalidArgument, "GameWorkspace.preset_path"});
        for (const auto &directory : {a_project.root(), a_engineRoot, a_ideTree, a_appTree})
        {
            auto valid = validate_parent_directories(a_files, directory);
            if (!valid.has_value())
                return valid;
        }
        auto userFile = a_project.root().join("CMakeUserPresets.json");
        if (!userFile.has_value())
            return Result<void>::failure(*userFile.try_error());
        auto userInfo = a_files.stat(*userFile.try_value());
        if (!userInfo.has_value())
            return Result<void>::failure(*userInfo.try_error());
        std::optional<std::string> previous;
        if (*userInfo.try_value())
        {
            auto text = read_text(a_files, *userFile.try_value());
            if (!text.has_value())
                return Result<void>::failure(*text.try_error());
            previous = text.take_value();
            if (previous->find(quoted(k_vendor)) == std::string::npos ||
                previous->find("\"projectId\": " + quoted(a_project.descriptor().projectId)) == std::string::npos)
                return Result<void>::failure({ErrorCategory::InvalidState, "GameWorkspace.foreign_user_presets"});
        }
        for (const auto *name : {"CMakeLists.txt", "CMakePresets.json"})
        {
            auto destination = a_project.root().join(name);
            auto source = a_engineRoot.join(std::string("Templates/GameWorkspace/") + name);
            if (!destination.has_value() || !source.has_value())
                return Result<void>::failure(destination.has_value() ? *source.try_error() : *destination.try_error());
            auto info = a_files.stat(*destination.try_value());
            if (!info.has_value())
                return Result<void>::failure(*info.try_error());
            if (*info.try_value())
            {
                if ((**info.try_value()).type != FileType::Regular)
                    return Result<void>::failure({ErrorCategory::InvalidArgument, "GameWorkspace.file_kind"});
                continue;
            }
            auto text = read_text(a_files, *source.try_value());
            if (!text.has_value())
                return Result<void>::failure(*text.try_error());
            pending.push_back({destination.take_value(), text.take_value()});
        }
        auto presets = user_presets(a_project, a_engineRoot, a_ideTree, a_appTree, a_platform, a_architecture);
        if (presets.size() > k_maxWorkspaceBytes)
            return Result<void>::failure({ErrorCategory::InvalidArgument, "GameWorkspace.preset_size"});
        if (!previous)
            pending.push_back({*userFile.try_value(), std::move(presets)});
        for (auto &item : pending)
        {
            auto written = write_new(a_files, item.path, item.text, item.isCreated);
            if (!written.has_value())
                return fail(*written.try_error());
        }
        if (previous && *previous != presets)
        {
            auto latest = read_text(a_files, *userFile.try_value());
            if (!latest.has_value())
                return fail(*latest.try_error());
            if (*latest.try_value() != *previous)
                return fail({ErrorCategory::InvalidState, "GameWorkspace.user_presets_changed"});
            // 管理するローカル値は原子的に差し替え、失敗時の旧 Preset は FileSystem が保持する
            auto saved =
                a_files.replace_file(*userFile.try_value(), std::as_bytes(std::span(presets.data(), presets.size())));
            if (!saved.has_value())
                return fail(*saved.try_error());
        }
        return Result<void>::success();
    }
    catch (const std::bad_alloc &)
    {
        return fail({ErrorCategory::PlatformFailure, "GameWorkspace.allocation"});
    }
    catch (...)
    {
        return fail({ErrorCategory::PlatformFailure, "GameWorkspace.exception"});
    }
}
} // namespace cue::detail
