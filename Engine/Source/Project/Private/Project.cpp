#include <Project/Project.h>

#include <algorithm>
#include <array>
#include <new>
#include <string_view>
#include <utility>

#include <EngineInstallation/EngineInstallation.h>
#include <Foundation/Utf8.h>
#include <Foundation/Uuid.h>
#include <Platform/FileSystem.h>

namespace cue
{
namespace
{
constexpr std::size_t k_maxProjectBytes = 64 * 1024;
constexpr std::array<std::string_view, 9> k_keys{"formatVersion",   "projectId",  "name",
                                                 "applicationName", "sourceRoot", "contentRoot",
                                                 "startupScene",    "gameTarget", "engineVersion"};
constexpr std::size_t k_requiredKeys = 6;

/// @brief 設定の区切りに使う Space / Tab と CRLF の CR だけを取り除く
std::string_view trim(std::string_view a_text)
{
    const auto first = a_text.find_first_not_of(" \t\r");
    return first == std::string_view::npos ? std::string_view{}
                                           : a_text.substr(first, a_text.find_last_not_of(" \t\r") - first + 1);
}

/// @brief 固定長の UUID 表記を検証し、未指定を表す全零 ID は拒否する
bool is_project_id(std::string_view a_text)
{
    return is_valid_uuid_text(a_text);
}

/// @brief 表示名は Path 構文から独立させ、UTF-8 と NUL・制御文字・長さを検証する
bool is_display_name(std::string_view a_text)
{
    if (a_text.empty() || a_text.size() > 256 || !is_valid_utf8_text(a_text))
    {
        return false;
    }
    for (const auto character : a_text)
    {
        if (static_cast<unsigned char>(character) < 32 || character == 127)
        {
            return false;
        }
    }
    return true;
}

/// @brief 成果物の名前は単一の移植可能な要素に限定する
bool is_application_name(std::string_view a_text)
{
    if (a_text.empty() || a_text.size() > 128)
    {
        return false;
    }
    for (const auto character : a_text)
    {
        if (!((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
              (character >= '0' && character <= '9') || character == '_' || character == '-'))
        {
            return false;
        }
    }
    return true;
}

/// @brief Project 内の相対配置だけを受け付け、字句的に Root 外へ出る参照を拒否する
Result<Path> relative_directory(std::string_view a_text, std::string_view a_key)
{
    auto path = Path::create(a_text);
    if (!path.has_value() || path.try_value()->is_absolute() || path.try_value()->utf8() == ".." ||
        path.try_value()->utf8().starts_with("../") || a_text.find(':') != std::string_view::npos)
    {
        return Result<Path>::failure({ErrorCategory::InvalidArgument, "Project." + std::string(a_key)});
    }
    return path;
}

/// @brief 既知 Key を重複なく読み、v1 互換と v2 の Engine 指定を検証して所有値を公開する
Result<ProjectDescriptor> parse_descriptor(std::string_view a_text)
{
    // Comment 行も含めて検証し、後から編集・保存する際に不正な文字列を持ち越さない
    if (!is_valid_utf8_text(a_text))
    {
        return Result<ProjectDescriptor>::failure({ErrorCategory::InvalidArgument, "Project.utf8"});
    }
    // UTF-8 BOM は先頭だけで許可する。値の引用符や Escape は v1 では解釈しない
    if (a_text.starts_with("\xef\xbb\xbf"))
    {
        a_text.remove_prefix(3);
    }
    std::array<std::string_view, k_keys.size()> values{};
    for (std::size_t offset = 0; offset < a_text.size();)
    {
        const auto newline = a_text.find('\n', offset);
        const auto end = newline == std::string_view::npos ? a_text.size() : newline;
        auto line = trim(a_text.substr(offset, end - offset));
        offset = end == a_text.size() ? end : end + 1;
        if (line.empty() || line.starts_with('#'))
        {
            continue;
        }
        const auto separator = line.find('=');
        if (separator == std::string_view::npos)
        {
            return Result<ProjectDescriptor>::failure({ErrorCategory::InvalidArgument, "Project.syntax"});
        }
        const auto key = trim(line.substr(0, separator));
        const auto value = trim(line.substr(separator + 1));
        std::size_t index = 0;
        while (index < k_keys.size() && key != k_keys[index])
        {
            ++index;
        }
        if (index == k_keys.size() || value.empty() || !values[index].empty())
        {
            return Result<ProjectDescriptor>::failure(
                {ErrorCategory::InvalidArgument, "Project.key:" + std::string(key)});
        }
        values[index] = value;
    }
    // 未知 Version と欠損を区別し、将来の形式を v1 として誤読しない
    for (std::size_t index = 0; index < k_requiredKeys; ++index)
    {
        if (values[index].empty())
        {
            return Result<ProjectDescriptor>::failure(
                {ErrorCategory::InvalidArgument, "Project.missing:" + std::string(k_keys[index])});
        }
    }
    if (values[0] != "1" && values[0] != "2")
    {
        return Result<ProjectDescriptor>::failure({ErrorCategory::InvalidArgument, "Project.formatVersion"});
    }
    if ((values[0] == "1" && !values[8].empty()) || (values[0] == "2" && !is_valid_engine_version(values[8])))
    {
        return Result<ProjectDescriptor>::failure({ErrorCategory::InvalidArgument, "Project.engineVersion"});
    }
    if (!is_project_id(values[1]) || !is_display_name(values[2]) || !is_application_name(values[3]))
    {
        return Result<ProjectDescriptor>::failure({ErrorCategory::InvalidArgument, "Project.identity"});
    }
    auto source = relative_directory(values[4], k_keys[4]);
    auto content = relative_directory(values[5], k_keys[5]);
    if (!source.has_value() || !content.has_value())
    {
        return Result<ProjectDescriptor>::failure(!source.has_value() ? *source.try_error() : *content.try_error());
    }
    std::optional<Path> startupScene;
    if (!values[6].empty())
    {
        auto path = relative_directory(values[6], k_keys[6]);
        if (!path.has_value())
            return Result<ProjectDescriptor>::failure(*path.try_error());
        if (path.try_value()->extension() != ".cuescene")
            return Result<ProjectDescriptor>::failure(
                {ErrorCategory::InvalidArgument, "Project.startupScene.extension"});
        startupScene = path.take_value();
    }
    std::optional<std::string> gameTarget;
    if (!values[7].empty())
    {
        if (!is_valid_game_target(values[7]))
            return Result<ProjectDescriptor>::failure({ErrorCategory::InvalidArgument, "Project.gameTarget"});
        gameTarget = values[7];
    }
    return Result<ProjectDescriptor>::success(
        {values[0] == "2" ? 2U : 1U, std::string(values[1]), std::string(values[2]), std::string(values[3]),
         source.take_value(), content.take_value(), std::move(startupScene), std::move(gameTarget),
         values[8].empty() ? std::nullopt : std::optional<std::string>(values[8])});
}

/// @brief 設定で指定した配置が存在する Directory か確認する
Result<void> validate_directory(IFileSystem &a_files, const Path &a_path, std::string_view a_key)
{
    auto info = a_files.stat(a_path);
    if (!info.has_value())
    {
        return Result<void>::failure(*info.try_error());
    }
    if (!*info.try_value() || (**info.try_value()).type != FileType::Directory)
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "Project.directory:" + std::string(a_key)});
    }
    return Result<void>::success();
}

/// @brief CMake の List / Generator Expression と衝突する配置を Build 指定時だけ拒否する
bool unsupported_cmake_path(const Path &a_path) noexcept
{
    return a_path.utf8().find(';') != std::string::npos || a_path.utf8().find("$<") != std::string::npos;
}

/// @brief 生成物の固定 Root と Source / Content を重ねない。Windows の末尾 Dot / Space Alias も除外する
bool overlaps_generated_root(const Path &a_relative) noexcept
{
    std::string_view first(a_relative.utf8());
    first = first.substr(0, first.find('/'));
    const auto last = first.find_last_not_of(" .");
    if (last == std::string_view::npos)
        return true;
    first = first.substr(0, last + 1);
    if (first.size() != 5 && first.size() != 12)
        return false;
    std::array<char, 12> lower{};
    for (std::size_t index = 0; index < first.size(); ++index)
    {
        const auto character = first[index];
        lower[index] = character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a') : character;
    }
    const std::string_view name(lower.data(), first.size());
    return name == "build" || name == "intermediate";
}
} // namespace

bool is_valid_game_target(std::string_view a_name) noexcept
{
    const auto isLetter = [](char a_character)
    { return (a_character >= 'a' && a_character <= 'z') || (a_character >= 'A' && a_character <= 'Z'); };
    if (a_name.empty() || a_name.size() > 128 || (!isLetter(a_name.front()) && a_name.front() != '_'))
        return false;
    std::array<char, 128> lower{};
    for (std::size_t index = 0; index < a_name.size(); ++index)
    {
        const auto character = a_name[index];
        if (!isLetter(character) && !(character >= '0' && character <= '9') && character != '_' && character != '-')
            return false;
        lower[index] = character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a') : character;
    }
    const std::string_view name(lower.data(), a_name.size());
    // Generator が持つ Target と、成果物名に使えない Device を大小文字を区別せず拒否する
    constexpr std::array<std::string_view, 17> reserved{
        "all",     "all_build",  "zero_check",    "install",   "clean", "test", "help", "package", "package_source",
        "rebuild", "edit_cache", "rebuild_cache", "run_tests", "con",   "prn",  "aux",  "nul"};
    if (std::find(reserved.begin(), reserved.end(), name) != reserved.end())
        return false;
    return !(name.size() == 4 && (name.starts_with("com") || name.starts_with("lpt")) && name[3] >= '1' &&
             name[3] <= '9');
}

Project::Project(ProjectDescriptor a_descriptor, Path a_file, Path a_source, Path a_content,
                 std::optional<Path> a_startupScene, std::optional<Path> a_gameCmake)
    : m_descriptor(std::move(a_descriptor)), m_file(std::move(a_file)), m_root(m_file.parent()),
      m_source(std::move(a_source)), m_content(std::move(a_content)), m_startupScene(std::move(a_startupScene)),
      m_gameCmake(std::move(a_gameCmake))
{
}

Result<Project> Project::load(IFileSystem &a_files, const Path &a_file)
{
    try
    {
        // 起動引数の相対 Path はここで一度だけ解決し、以後の File 操作へ絶対 Path を渡す
        auto file = a_files.absolute_path(a_file);
        if (!file.has_value())
        {
            return Result<Project>::failure(*file.try_error());
        }
        if (!file.try_value()->is_absolute() || file.try_value()->extension() != ".cueproj")
        {
            return Result<Project>::failure({ErrorCategory::InvalidArgument, "Project.extension"});
        }
        auto bytes = a_files.read_all(*file.try_value(), k_maxProjectBytes);
        if (!bytes.has_value())
        {
            return Result<Project>::failure(*bytes.try_error());
        }
        const auto &data = *bytes.try_value();
        const std::string_view text(data.empty() ? "" : reinterpret_cast<const char *>(data.data()), data.size());
        auto descriptor = parse_descriptor(text);
        if (!descriptor.has_value())
        {
            return Result<Project>::failure(*descriptor.try_error());
        }
        // 全配置を同じ Project Root へ固定する。検証中に Directory を作成することはしない
        const auto root = file.try_value()->parent();
        auto source = root.join(descriptor.try_value()->sourceRoot);
        auto content = root.join(descriptor.try_value()->contentRoot);
        if (!source.has_value() || !content.has_value())
        {
            return Result<Project>::failure(!source.has_value() ? *source.try_error() : *content.try_error());
        }
        if (descriptor.try_value()->gameTarget)
        {
            // 既存の編集専用 Project の Path 契約は保ち、Build 接続時だけ生成物の安全性を検証する
            if (unsupported_cmake_path(root) || unsupported_cmake_path(*source.try_value()) ||
                unsupported_cmake_path(*content.try_value()))
                return Result<Project>::failure({ErrorCategory::InvalidArgument, "Project.gameTarget.cmake_path"});
            if (overlaps_generated_root(descriptor.try_value()->sourceRoot) ||
                overlaps_generated_root(descriptor.try_value()->contentRoot))
                return Result<Project>::failure(
                    {ErrorCategory::InvalidArgument, "Project.gameTarget.generated_overlap"});
        }
        auto sourceValid = validate_directory(a_files, *source.try_value(), "sourceRoot");
        if (!sourceValid.has_value())
        {
            return Result<Project>::failure(*sourceValid.try_error());
        }
        auto contentValid = validate_directory(a_files, *content.try_value(), "contentRoot");
        if (!contentValid.has_value())
        {
            return Result<Project>::failure(*contentValid.try_error());
        }
        std::optional<Path> startupScene;
        if (descriptor.try_value()->startupScene)
        {
            // 起動 Scene は Content から解決し、Scene Parser を呼ぶ前に配置だけを検証する
            auto path = content.try_value()->join(*descriptor.try_value()->startupScene);
            if (!path.has_value())
                return Result<Project>::failure(*path.try_error());
            auto info = a_files.stat(*path.try_value());
            if (!info.has_value())
                return Result<Project>::failure(*info.try_error());
            if (!*info.try_value() || (**info.try_value()).type != FileType::Regular)
                return Result<Project>::failure({ErrorCategory::InvalidArgument, "Project.startupScene.file"});
            startupScene = path.take_value();
        }
        std::optional<Path> gameCmake;
        if (descriptor.try_value()->gameTarget)
        {
            // Game Target の登録は Source 側だけが定義する。Load 中に CMake を実行しない
            auto path = source.try_value()->join("CMakeLists.txt");
            if (!path.has_value())
                return Result<Project>::failure(*path.try_error());
            auto info = a_files.stat(*path.try_value());
            if (!info.has_value())
                return Result<Project>::failure(*info.try_error());
            if (!*info.try_value() || (**info.try_value()).type != FileType::Regular)
                return Result<Project>::failure({ErrorCategory::InvalidArgument, "Project.gameTarget.cmake_file"});
            gameCmake = path.take_value();
        }
        return Result<Project>::success(Project(descriptor.take_value(), file.take_value(), source.take_value(),
                                                content.take_value(), std::move(startupScene), std::move(gameCmake)));
    }
    catch (const std::bad_alloc &)
    {
        return Result<Project>::failure({ErrorCategory::PlatformFailure, "Project.load.allocation"});
    }
}

const ProjectDescriptor &Project::descriptor() const noexcept
{
    return m_descriptor;
}

const Path &Project::file() const noexcept
{
    return m_file;
}

const Path &Project::root() const noexcept
{
    return m_root;
}

const Path &Project::source_directory() const noexcept
{
    return m_source;
}

const Path &Project::content_directory() const noexcept
{
    return m_content;
}

const Path *Project::startup_scene_file() const noexcept
{
    return m_startupScene ? &*m_startupScene : nullptr;
}

const Path *Project::game_cmake_file() const noexcept
{
    return m_gameCmake ? &*m_gameCmake : nullptr;
}
} // namespace cue
