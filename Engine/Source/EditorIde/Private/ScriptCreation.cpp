#include <EditorIde/ScriptCreation.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <EditorIde/ScriptSource.h>
#include <Foundation/Utf8.h>
#include <Platform/FileSystem.h>
#include <Project/Project.h>

namespace cue
{
namespace
{
constexpr std::size_t k_maxCatalogBytes = 64 * 1024;

/// @brief File 操作前の失敗を Authoring の入力 Error へ揃える
Error invalid(const char *a_operation)
{
    return {ErrorCategory::InvalidArgument, a_operation};
}

/// @brief ASCII の比較だけを正規化し、OS の文字 Locale に依存させない
std::string folded(std::string_view a_value)
{
    std::string value(a_value);
    for (auto &character : value)
        if (character >= 'A' && character <= 'Z')
            character += 'a' - 'A';
    return value;
}

/// @brief 行末と Key の周囲だけを除き、値内部を解釈しない
std::string_view trim(std::string_view a_value)
{
    const auto first = a_value.find_first_not_of(" \t\r");
    return first == std::string_view::npos ? std::string_view{}
                                           : a_value.substr(first, a_value.find_last_not_of(" \t\r") - first + 1);
}

/// @brief Comment 内の記述を登録用 CMake の入口として誤認しない
bool has_catalog_command(std::string_view a_text)
{
    for (std::size_t offset = 0; offset < a_text.size();)
    {
        const auto end = a_text.find('\n', offset);
        const auto line = trim(a_text.substr(offset, end == std::string_view::npos ? end : end - offset));
        if (line.starts_with("cue_add_game_scripts("))
            return true;
        offset = end == std::string_view::npos ? a_text.size() : end + 1;
    }
    return false;
}

/// @brief CMake と C++ へ文字列として転記できる型 ID だけを受け付ける
bool valid_type_id(std::string_view a_value)
{
    if (a_value.empty() || a_value.size() > 128 ||
        !((a_value.front() >= 'A' && a_value.front() <= 'Z') || (a_value.front() >= 'a' && a_value.front() <= 'z')))
        return false;
    return std::all_of(a_value.begin(), a_value.end(),
                       [](char a_character)
                       {
                           return (a_character >= 'A' && a_character <= 'Z') ||
                                  (a_character >= 'a' && a_character <= 'z') ||
                                  (a_character >= '0' && a_character <= '9') || a_character == '_' ||
                                  a_character == '.';
                       });
}

/// @brief 存在する全親要素を Drive Root まで確認し、Link を越えて作成しない
Result<void> validate_directory(IFileSystem &a_files, const Path &a_path, bool a_allowMissing = false)
{
    if (!a_path.is_absolute())
        return Result<void>::failure(invalid("EditorIde.script_directory_absolute"));
    auto current = a_path;
    for (;;)
    {
        auto info = a_files.stat(current);
        if (!info.has_value())
            return Result<void>::failure(*info.try_error());
        if ((!*info.try_value() && !a_allowMissing) ||
            (*info.try_value() && (**info.try_value()).type != FileType::Directory))
            return Result<void>::failure(invalid("EditorIde.script_directory_kind"));
        const auto parent = current.parent();
        if (parent.utf8() == current.utf8())
            return Result<void>::success();
        current = parent;
    }
}

/// @brief 通常 File だけを読込み、Size と UTF-8 を上限内で検証する
Result<std::string> read_text(IFileSystem &a_files, const Path &a_file)
{
    auto parent = validate_directory(a_files, a_file.parent());
    if (!parent.has_value())
        return Result<std::string>::failure(*parent.try_error());
    auto info = a_files.stat(a_file);
    if (!info.has_value())
        return Result<std::string>::failure(*info.try_error());
    if (!*info.try_value() || (**info.try_value()).type != FileType::Regular ||
        (**info.try_value()).size > k_maxCatalogBytes)
        return Result<std::string>::failure(invalid("EditorIde.script_file_kind"));
    auto bytes = a_files.read_all(a_file, k_maxCatalogBytes);
    if (!bytes.has_value())
        return Result<std::string>::failure(*bytes.try_error());
    const auto &data = *bytes.try_value();
    std::string text(data.empty() ? "" : reinterpret_cast<const char *>(data.data()), data.size());
    if (!is_valid_utf8_text(text))
        return Result<std::string>::failure(invalid("EditorIde.script_file_utf8"));
    return Result<std::string>::success(std::move(text));
}

/// @brief 型と登録 Symbol の対を確認し、既存 Source を解決してから新しい名前を予約する
Result<void> validate_catalog(IFileSystem &a_files, const Project &a_project, std::string_view a_text,
                              std::string_view a_name)
{
    if (a_text.starts_with("\xef\xbb\xbf"))
        a_text.remove_prefix(3);
    std::unordered_set<std::string> keys;
    std::unordered_map<std::string, std::string> scripts;
    std::unordered_map<std::string, std::string> registrations;
    std::unordered_set<std::string> sourceNames;
    std::unordered_set<std::string> symbols;
    std::unordered_set<std::string> typeNames;
    bool hasVersion = false;
    for (std::size_t offset = 0; offset < a_text.size();)
    {
        const auto end = a_text.find('\n', offset);
        const auto line = trim(a_text.substr(offset, end == std::string_view::npos ? end : end - offset));
        offset = end == std::string_view::npos ? a_text.size() : end + 1;
        if (line.empty() || line.starts_with('#'))
            continue;
        const auto separator = line.find('=');
        if (separator == std::string_view::npos)
            return Result<void>::failure(invalid("EditorIde.script_catalog_syntax"));
        const auto key = trim(line.substr(0, separator));
        const auto value = trim(line.substr(separator + 1));
        if (key.empty() || value.empty() || !keys.emplace(key).second || keys.size() > 1025)
            return Result<void>::failure(invalid("EditorIde.script_catalog_key"));
        if (key == "formatVersion")
        {
            if (value != "2")
                return Result<void>::failure(invalid("EditorIde.script_catalog_migration_required"));
            hasVersion = true;
        }
        else if (key.starts_with("script.") && valid_type_id(key.substr(7)))
        {
            if (!value.ends_with(".cpp") || value.find_first_of("\\;$<>\"|?*:") != std::string_view::npos ||
                !sourceNames.emplace(folded(value)).second || !typeNames.emplace(folded(key.substr(7))).second)
                return Result<void>::failure(invalid("EditorIde.script_catalog_source"));
            scripts.emplace(std::string(key.substr(7)), std::string(value));
        }
        else if (key.starts_with("registration.") && valid_type_id(key.substr(13)) && is_valid_script_name(value))
        {
            if (!symbols.emplace(folded(value)).second)
                return Result<void>::failure(invalid("EditorIde.script_catalog_symbol"));
            registrations.emplace(std::string(key.substr(13)), std::string(value));
        }
        else
            return Result<void>::failure(invalid("EditorIde.script_catalog_entry"));
    }
    if (!hasVersion || scripts.size() != registrations.size() || scripts.size() >= 256)
        return Result<void>::failure(invalid("EditorIde.script_catalog_pairs"));
    for (const auto &[typeId, source] : scripts)
    {
        if (!registrations.contains(typeId))
            return Result<void>::failure(invalid("EditorIde.script_catalog_pairs"));
        auto resolved = resolve_script_source(a_files, a_project, typeId);
        if (!resolved.has_value())
            return Result<void>::failure(*resolved.try_error());
        auto parent = validate_directory(a_files, resolved.try_value()->parent());
        if (!parent.has_value())
            return parent;
    }
    const auto name = folded(a_name);
    if (sourceNames.contains(name + ".cpp") || symbols.contains(name) || typeNames.contains("game." + name))
        return Result<void>::failure(invalid("EditorIde.script_exists"));
    return Result<void>::success();
}

/// @brief 他の Script から型を参照できるように Behaviour の宣言を生成する
std::string make_header(std::string_view a_name)
{
    const std::string name(a_name);
    return "#pragma once\n\n#include <Foundation/Result.h>\n#include <Script/Behaviour.h>\n\nnamespace game\n{\n"
           "/// @brief ゲームの更新処理を実装する Script\nclass " +
           name +
           " final : public ::cue::Behaviour\n{\n  public:\n"
           "    /// @brief 更新処理を World の Owner Thread で実行する\n"
           "    [[nodiscard]] ::cue::Result<void> update() override;\n};\n} // namespace game\n";
}

/// @brief 更新処理と型の登録を cpp に生成し、共通 C ABI 入口は Build 時に生成する
std::string make_source(std::string_view a_name)
{
    const std::string name(a_name);
    return "#include \"" + name + ".h\"\n\n#include <Script/ModuleEntry.h>\n\nnamespace game\n{\n"
           "::cue::Result<void> " +
           name +
           "::update()\n{\n    return ::cue::Result<void>::success();\n}\n} // namespace game\n\n"
           "// この cpp の型だけを登録し、接続入口は Build が一つに集約する\n"
           "CUE_DEFINE_SCRIPT(" +
           name + ", ::cue::make_script_registration<::game::" + name + ">({\"game." + name + "\", \"" + name +
           "\", 1, {}}))\n";
}
} // namespace

bool is_valid_script_name(std::string_view a_name) noexcept
{
    if (a_name.empty() || a_name.size() > 64 || a_name.find("__") != std::string_view::npos ||
        !((a_name.front() >= 'A' && a_name.front() <= 'Z') || (a_name.front() >= 'a' && a_name.front() <= 'z')) ||
        !std::all_of(a_name.begin(), a_name.end(),
                     [](char a_character)
                     {
                         return (a_character >= 'A' && a_character <= 'Z') ||
                                (a_character >= 'a' && a_character <= 'z') ||
                                (a_character >= '0' && a_character <= '9') || a_character == '_';
                     }))
        return false;
    constexpr std::string_view keywords =
        "alignas alignof and and_eq asm atomic_cancel atomic_commit atomic_noexcept auto bitand bitor bool break "
        "case catch char char8_t char16_t char32_t class compl concept const consteval constexpr constinit const_cast "
        "continue co_await co_return co_yield decltype default delete do double dynamic_cast else enum explicit "
        "export extern false float for friend goto if inline int long mutable namespace new noexcept not not_eq "
        "nullptr operator or or_eq private protected public reflexpr register reinterpret_cast requires return "
        "short signed sizeof static static_assert static_cast struct switch synchronized template this thread_local "
        "throw true try typedef typeid typename union unsigned using virtual void volatile wchar_t while xor xor_eq";
    for (std::size_t begin = 0; begin < keywords.size();)
    {
        const auto end = keywords.find(' ', begin);
        if (keywords.substr(begin, end == std::string_view::npos ? end : end - begin) == a_name)
            return false;
        begin = end == std::string_view::npos ? keywords.size() : end + 1;
    }
    // 大文字小文字を問わず Windows の予約 Device 名を File 名にしない
    if (a_name.size() == 3 || a_name.size() == 4)
    {
        std::array<char, 4> lower{};
        for (std::size_t index = 0; index < a_name.size(); ++index)
            lower[index] = a_name[index] >= 'A' && a_name[index] <= 'Z' ? a_name[index] + ('a' - 'A') : a_name[index];
        const std::string_view name(lower.data(), a_name.size());
        if (name == "con" || name == "prn" || name == "aux" || name == "nul" ||
            ((name.starts_with("com") || name.starts_with("lpt")) && name.size() == 4 && name[3] >= '1' &&
             name[3] <= '9'))
            return false;
    }
    return true;
}

Result<Path> create_script_source(IFileSystem &a_files, const Project &a_project, std::string_view a_className)
{
    struct ScriptFile
    {
        Path path;
        std::string text;
        std::size_t ownedBytes = 0;
        bool isCreated = false;
    };
    std::array<ScriptFile, 2> outputs;
    auto &[header, source] = outputs;
    bool isPublished = false;
    std::unique_ptr<IFile> lock;
    // 新規作成した内容と一致する File だけを回収し、競合する利用者の変更を削除しない
    const auto fail = [&](Error a_error) -> Result<Path>
    {
        if (isPublished)
            return Result<Path>::failure(std::move(a_error));
        for (const auto &output : outputs)
        {
            if (!output.isCreated)
                continue;
            auto parent = validate_directory(a_files, output.path.parent());
            auto info = a_files.stat(output.path);
            auto current = parent.has_value() && info.has_value() && *info.try_value() &&
                                   (**info.try_value()).type == FileType::Regular
                               ? a_files.read_all(output.path, output.text.size())
                               : Result<std::vector<std::byte>>::failure(invalid("EditorIde.script_cleanup_kind"));
            if (!current.has_value() || current.try_value()->size() != output.ownedBytes ||
                !std::equal(current.try_value()->begin(), current.try_value()->end(),
                            reinterpret_cast<const std::byte *>(output.text.data())))
                a_error.operation += ";cleanup:EditorIde.script_source_changed";
            else
            {
                auto removed = a_files.remove(output.path);
                if (!removed.has_value())
                    a_error.operation += ";cleanup:" + removed.try_error()->operation;
            }
        }
        return Result<Path>::failure(std::move(a_error));
    };
    try
    {
        if (!is_valid_script_name(a_className) || !a_project.descriptor().gameTarget)
            return fail(invalid("EditorIde.script_name"));
        auto sourceValid = validate_directory(a_files, a_project.source_directory());
        if (!sourceValid.has_value())
            return fail(*sourceValid.try_error());
        auto catalogPath = a_project.source_directory().join("ScriptSources.cueide");
        auto cmakePath = a_project.source_directory().join("CMakeLists.txt");
        auto headerPath = a_project.source_directory().join(std::string(a_className) + ".h");
        auto sourcePath = a_project.source_directory().join(std::string(a_className) + ".cpp");
        auto lockPath = a_project.root().join("Intermediate/Editor/ScriptCreation.lock");
        if (!catalogPath.has_value() || !cmakePath.has_value() || !headerPath.has_value() || !sourcePath.has_value() ||
            !lockPath.has_value())
            return fail(invalid("EditorIde.script_path"));
        header.path = headerPath.take_value();
        source.path = sourcePath.take_value();
        auto lockParent = validate_directory(a_files, lockPath.try_value()->parent(), true);
        if (!lockParent.has_value())
            return fail(*lockParent.try_error());
        auto directories = a_files.create_directories(lockPath.try_value()->parent());
        if (!directories.has_value())
            return fail(*directories.try_error());
        lockParent = validate_directory(a_files, lockPath.try_value()->parent());
        if (!lockParent.has_value())
            return fail(*lockParent.try_error());
        auto lockInfo = a_files.stat(*lockPath.try_value());
        if (!lockInfo.has_value())
            return fail(*lockInfo.try_error());
        if (*lockInfo.try_value() && (**lockInfo.try_value()).type != FileType::Regular)
            return fail(invalid("EditorIde.script_lock_kind"));
        auto locked =
            a_files.open(*lockPath.try_value(), {FileAccess::ReadWrite, FileCreation::OpenAlways, false, false, false});
        if (!locked.has_value())
            return fail(*locked.try_error());
        lock = locked.take_value();
        lockInfo = a_files.stat(*lockPath.try_value());
        if (!lockInfo.has_value() || !*lockInfo.try_value() || (**lockInfo.try_value()).type != FileType::Regular)
            return fail(lockInfo.has_value() ? invalid("EditorIde.script_lock_kind") : *lockInfo.try_error());
        auto cmake = read_text(a_files, *cmakePath.try_value());
        auto catalog = read_text(a_files, *catalogPath.try_value());
        if (!cmake.has_value() || !catalog.has_value())
            return fail(!cmake.has_value() ? *cmake.try_error() : *catalog.try_error());
        if (!has_catalog_command(*cmake.try_value()))
            return fail(invalid("EditorIde.script_catalog_migration_required"));
        auto valid = validate_catalog(a_files, a_project, *catalog.try_value(), a_className);
        if (!valid.has_value())
            return fail(*valid.try_error());
        auto children = a_files.list_directory(a_project.source_directory());
        if (!children.has_value())
            return fail(*children.try_error());
        // 両方の出力先を先に検証し、片方だけが既に存在するときも上書きしない
        for (const auto &output : outputs)
        {
            const auto name = folded(output.path.filename());
            for (const auto &child : *children.try_value())
                if (folded(child.filename()) == name)
                    return fail(invalid("EditorIde.script_exists"));
            auto info = a_files.stat(output.path);
            if (!info.has_value() || *info.try_value())
                return fail(info.has_value() ? invalid("EditorIde.script_exists") : *info.try_error());
        }
        header.text = make_header(a_className);
        source.text = make_source(a_className);
        auto updated = *catalog.try_value();
        if (!updated.empty() && !updated.ends_with('\n'))
            updated += '\n';
        updated += "script.game." + std::string(a_className) + '=' + std::string(a_className) +
                   ".cpp\nregistration.game." + std::string(a_className) + '=' + std::string(a_className) + '\n';
        if (updated.size() > k_maxCatalogBytes)
            return fail(invalid("EditorIde.script_catalog_size"));
        // Header と cpp の両方が書き込み済みになってから型の対応表を公開する
        for (auto &output : outputs)
        {
            auto created = a_files.open(output.path, {FileAccess::Write, FileCreation::CreateNew, false, false, false});
            if (!created.has_value())
                return fail(*created.try_error());
            auto file = created.take_value();
            output.isCreated = true;
            const auto bytes = std::as_bytes(std::span(output.text.data(), output.text.size()));
            std::optional<Error> writeError;
            while (output.ownedBytes < bytes.size())
            {
                auto written = file->write(bytes.subspan(output.ownedBytes));
                if (!written.has_value() || *written.try_value() == 0 ||
                    *written.try_value() > bytes.size() - output.ownedBytes)
                {
                    writeError = written.has_value() ? invalid("EditorIde.script_write") : *written.try_error();
                    break;
                }
                output.ownedBytes += *written.try_value();
            }
            if (!writeError)
            {
                auto flushed = file->flush();
                if (!flushed.has_value())
                    writeError = *flushed.try_error();
            }
            auto closed = file->close();
            file.reset();
            if (!writeError && !closed.has_value())
                writeError = *closed.try_error();
            if (writeError)
                return fail(*writeError);
        }
        // 排他を使わない外部 Editor の変更も、公開直前に検出して保持する
        auto latest = read_text(a_files, *catalogPath.try_value());
        if (!latest.has_value() || *latest.try_value() != *catalog.try_value())
            return fail(latest.has_value() ? invalid("EditorIde.script_catalog_changed") : *latest.try_error());
        auto saved =
            a_files.replace_file(*catalogPath.try_value(), std::as_bytes(std::span(updated.data(), updated.size())));
        if (!saved.has_value())
            return fail(*saved.try_error());
        isPublished = true;
        return Result<Path>::success(std::move(source.path));
    }
    catch (...)
    {
        return fail({ErrorCategory::PlatformFailure, "EditorIde.script_create_exception"});
    }
}
} // namespace cue
