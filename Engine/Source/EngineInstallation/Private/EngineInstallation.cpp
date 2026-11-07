#include <EngineInstallation/EngineInstallation.h>

#include <algorithm>
#include <array>
#include <new>
#include <optional>
#include <utility>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <Foundation/Windows/UtfConversion.h>
#endif

#include <EngineInstallation/EngineVersion.h>
#include <Foundation/Utf8.h>
#include <Platform/FileSystem.h>

namespace cue
{
namespace
{
constexpr std::array<std::string_view, 13> k_keys{
    "formatVersion", "product",          "version",          "buildId", "platform",    "architecture", "configuration",
    "sdkAbiVersion", "projectFormatMin", "projectFormatMax", "editor",  "projectTool", "useLeaseId"};

/// @brief 読取境界の失敗を配置操作の診断として返す
Error invalid(std::string a_name)
{
    return {ErrorCategory::InvalidArgument, "EngineInstallation." + std::move(a_name)};
}

/// @brief Key / Value の外側だけを取り除き、引用や Escape を解釈しない
std::string_view trim(std::string_view a_text) noexcept
{
    const auto first = a_text.find_first_not_of(" \t\r");
    return first == std::string_view::npos ? std::string_view{}
                                           : a_text.substr(first, a_text.find_last_not_of(" \t\r") - first + 1);
}

/// @brief SemVer の識別子で使える ASCII だけを許可する
bool identifier_character(char a_value) noexcept
{
    return (a_value >= 'a' && a_value <= 'z') || (a_value >= 'A' && a_value <= 'Z') ||
           (a_value >= '0' && a_value <= '9') || a_value == '-';
}

/// @brief 数値 Version の先頭 Zero と空要素を拒否する
bool numeric_identifier(std::string_view a_value) noexcept
{
    return !a_value.empty() && (a_value.size() == 1 || a_value.front() != '0') &&
           std::all_of(a_value.begin(), a_value.end(), [](char a_digit) { return a_digit >= '0' && a_digit <= '9'; });
}

/// @brief Dot 区切りの識別子を検証し、Pre-release の数値だけ先頭 Zero を拒否する
bool identifier_list(std::string_view a_text, bool a_isPrerelease) noexcept
{
    if (a_text.empty())
        return false;
    while (!a_text.empty())
    {
        const auto dot = a_text.find('.');
        const auto part = a_text.substr(0, dot);
        if (part.empty() || !std::all_of(part.begin(), part.end(), identifier_character))
            return false;
        const bool isNumeric =
            std::all_of(part.begin(), part.end(), [](char a_value) { return a_value >= '0' && a_value <= '9'; });
        if (a_isPrerelease && isNumeric && !numeric_identifier(part))
            return false;
        if (dot == std::string_view::npos)
            return true;
        a_text.remove_prefix(dot + 1);
    }
    return false;
}

/// @brief File と全親の種類を確認し、Reparse Point の別名で配布 Root を越えない
Result<void> require_type(IFileSystem &a_files, const Path &a_path, FileType a_type)
{
    if (!a_path.is_absolute())
        return Result<void>::failure(invalid("absolute_path"));
    auto current = a_path;
    for (;;)
    {
        auto info = a_files.stat(current);
        if (!info.has_value())
            return Result<void>::failure(*info.try_error());
        if (!*info.try_value() || (**info.try_value()).type != a_type)
            return Result<void>::failure(invalid("path_kind:" + current.utf8()));
        const auto parent = current.parent();
        if (parent.utf8() == current.utf8())
            return Result<void>::success();
        current = parent;
        a_type = FileType::Directory;
    }
}

/// @brief 保存用の配置を Engine Root 内の通常 File へ解決する
Result<Path> contained_file(IFileSystem &a_files, const Path &a_root, std::string_view a_relative)
{
    auto relative = Path::create(a_relative);
    if (!relative.has_value() || relative.try_value()->is_absolute() ||
        a_relative.find(':') != std::string_view::npos || relative.try_value()->utf8() == ".." ||
        relative.try_value()->utf8().starts_with("../"))
        return Result<Path>::failure(invalid("relative_file"));
    auto file = a_root.join(relative.try_value()->utf8());
    if (!file.has_value())
        return Result<Path>::failure(*file.try_error());
    auto type = require_type(a_files, *file.try_value(), FileType::Regular);
    return type.has_value() ? Result<Path>::success(file.take_value()) : Result<Path>::failure(*type.try_error());
}

/// @brief 配布 Descriptor の既知 Key を一度だけ読み、余剰情報を成功扱いしない
Result<std::array<std::string, k_keys.size()>> parse(std::string_view a_text)
{
    using ParseResult = Result<std::array<std::string, k_keys.size()>>;
    if (!is_valid_utf8_text(a_text))
        return ParseResult::failure(invalid("utf8"));
    if (a_text.starts_with("\xef\xbb\xbf"))
        a_text.remove_prefix(3);
    std::array<std::string, k_keys.size()> values{};
    std::array<bool, k_keys.size()> seen{};
    while (!a_text.empty())
    {
        const auto newline = a_text.find('\n');
        const auto line = trim(a_text.substr(0, newline));
        a_text = newline == std::string_view::npos ? std::string_view{} : a_text.substr(newline + 1);
        if (line.empty() || line.front() == '#')
            continue;
        const auto equals = line.find('=');
        if (equals == std::string_view::npos)
            return ParseResult::failure(invalid("line"));
        const auto key = trim(line.substr(0, equals));
        const auto value = trim(line.substr(equals + 1));
        const auto found = std::find(k_keys.begin(), k_keys.end(), key);
        if (found == k_keys.end() || value.empty())
            return ParseResult::failure(invalid("key"));
        const auto index = static_cast<std::size_t>(found - k_keys.begin());
        if (seen[index] || std::any_of(value.begin(), value.end(), [](char a_value)
                                       { return static_cast<unsigned char>(a_value) < 32 || a_value == 127; }))
            return ParseResult::failure(invalid("duplicate_or_control"));
        seen[index] = true;
        values[index] = value;
    }
    if (!std::all_of(seen.begin(), seen.end(), [](bool a_seen) { return a_seen; }))
        return ParseResult::failure(invalid("missing_key"));
    return ParseResult::success(std::move(values));
}

/// @brief 既存親も検証し、ユーザー領域の専用保存先だけを生成する
Result<void> prepare_directory(IFileSystem &a_files, const Path &a_path)
{
    if (!a_path.is_absolute())
        return Result<void>::failure(invalid("lease_absolute"));
    std::vector<Path> missing;
    auto current = a_path;
    for (;;)
    {
        auto info = a_files.stat(current);
        if (!info.has_value())
            return Result<void>::failure(*info.try_error());
        if (*info.try_value())
        {
            auto valid = require_type(a_files, current, FileType::Directory);
            if (!valid.has_value())
                return valid;
            break;
        }
        missing.push_back(current);
        auto parent = current.parent();
        if (parent.utf8() == current.utf8())
            return Result<void>::failure(invalid("lease_parent"));
        current = std::move(parent);
    }
    for (auto entry = missing.rbegin(); entry != missing.rend(); ++entry)
    {
        auto created = a_files.create_directories(*entry);
        if (!created.has_value())
            return created;
        auto valid = require_type(a_files, *entry, FileType::Directory);
        if (!valid.has_value())
            return valid;
    }
    return Result<void>::success();
}

#if defined(_WIN32)
/// @brief 明示設定と Windows の標準導入先を UTF-8 の所有値へ変換する
Result<std::optional<Path>> environment_path(const wchar_t *a_name)
{
    SetLastError(ERROR_SUCCESS);
    const auto required = GetEnvironmentVariableW(a_name, nullptr, 0);
    if (!required)
    {
        const auto error = GetLastError();
        return error == ERROR_ENVVAR_NOT_FOUND || error == ERROR_SUCCESS
                   ? Result<std::optional<Path>>::success(std::nullopt)
                   : Result<std::optional<Path>>::failure(
                         {ErrorCategory::PlatformFailure, "EngineInstallation.environment", error});
    }
    std::wstring value(required, L'\0');
    const auto length = GetEnvironmentVariableW(a_name, value.data(), required);
    if (!length || length >= required)
        return Result<std::optional<Path>>::failure(invalid("environment_changed"));
    value.resize(length);
    auto text = utf16_to_utf8(value);
    if (!text.has_value())
        return Result<std::optional<Path>>::failure(*text.try_error());
    auto path = Path::create(*text.try_value());
    if (!path.has_value() || !path.try_value()->is_absolute())
        return Result<std::optional<Path>>::failure(invalid("environment_absolute"));
    return Result<std::optional<Path>>::success(path.take_value());
}
#endif
} // namespace

bool is_valid_engine_version(std::string_view a_version) noexcept
{
    if (a_version.empty() || a_version.size() > 128)
        return false;
    const auto plus = a_version.find('+');
    if (plus != std::string_view::npos && !identifier_list(a_version.substr(plus + 1), false))
        return false;
    auto main = a_version.substr(0, plus);
    const auto dash = main.find('-');
    if (dash != std::string_view::npos && !identifier_list(main.substr(dash + 1), true))
        return false;
    auto core = main.substr(0, dash);
    for (int index = 0; index < 3; ++index)
    {
        const auto dot = core.find('.');
        if (!numeric_identifier(core.substr(0, dot)) || (index == 2) != (dot == std::string_view::npos))
            return false;
        if (dot != std::string_view::npos)
            core.remove_prefix(dot + 1);
    }
    return true;
}

Result<EngineInstallation> load_engine_installation(IFileSystem &a_files, const Path &a_root)
{
    try
    {
        auto root = require_type(a_files, a_root, FileType::Directory);
        if (!root.has_value())
            return Result<EngineInstallation>::failure(*root.try_error());
        auto descriptor = contained_file(a_files, a_root, "engine.cueengine");
        if (!descriptor.has_value())
            return Result<EngineInstallation>::failure(*descriptor.try_error());
        auto bytes = a_files.read_all(*descriptor.try_value(), 32 * 1024);
        if (!bytes.has_value())
            return Result<EngineInstallation>::failure(*bytes.try_error());
        auto parsed = parse({reinterpret_cast<const char *>(bytes.try_value()->data()), bytes.try_value()->size()});
        if (!parsed.has_value())
            return Result<EngineInstallation>::failure(*parsed.try_error());
        const auto &value = *parsed.try_value();
        if (value[0] != "1" || value[1] != "engine" || !is_valid_engine_version(value[2]) || value[3].size() > 256 ||
            value[4] != "Windows" || value[5] != "x64" || value[6] != "Development" || value[7] != "1" ||
            value[8] != "1" || value[9] != "2" || value[12] != "engine-" + value[2] + "-windows-x64")
            return Result<EngineInstallation>::failure(invalid("contract"));
        auto editor = contained_file(a_files, a_root, value[10]);
        auto tool = contained_file(a_files, a_root, value[11]);
        if (!editor.has_value() || !tool.has_value())
            return Result<EngineInstallation>::failure(!editor.has_value() ? *editor.try_error() : *tool.try_error());
        if (value[10] != "Bin/Editor/CueEditorHost.exe" || value[11] != "Bin/Tools/CueProjectTool.exe")
            return Result<EngineInstallation>::failure(invalid("host_paths"));
        for (const auto *name : {"CMakeLists.txt", "CMakePresets.json"})
        {
            auto source = contained_file(a_files, a_root, name);
            if (!source.has_value())
                return Result<EngineInstallation>::failure(*source.try_error());
        }
        return Result<EngineInstallation>::success(
            {a_root, editor.take_value(), tool.take_value(), value[2], value[3], value[12], true});
    }
    catch (const std::bad_alloc &)
    {
        return Result<EngineInstallation>::failure({ErrorCategory::PlatformFailure, "EngineInstallation.allocation"});
    }
}

Result<EngineInstallation> resolve_engine_installation(IFileSystem &a_files, const Path &a_developmentRoot)
{
    try
    {
        auto executable = a_files.executable_directory();
        if (!executable.has_value())
            return Result<EngineInstallation>::failure(*executable.try_error());
        const auto expectedPackageRoot = executable.try_value()->parent().parent();
        const bool hasPackageLayout =
            executable.try_value()->parent().filename() == "Bin" &&
            (executable.try_value()->filename() == "Editor" || executable.try_value()->filename() == "Tools");
        auto current = *executable.try_value();
        for (int depth = 0; depth < 8; ++depth)
        {
            auto descriptor = current.join("engine.cueengine");
            if (!descriptor.has_value())
                return Result<EngineInstallation>::failure(*descriptor.try_error());
            auto info = a_files.stat(*descriptor.try_value());
            if (!info.has_value())
                return Result<EngineInstallation>::failure(*info.try_error());
            if (*info.try_value())
            {
                auto installation = load_engine_installation(a_files, current);
                if (!installation.has_value())
                    return installation;
                // 実行 Binary と同じ識別の配布物だけを使用し、別 SDK の組合せを拒否する
                if (installation.try_value()->version != CUE_ENGINE_VERSION ||
                    installation.try_value()->buildId != CUE_ENGINE_BUILD_ID)
                    return Result<EngineInstallation>::failure(invalid("binary_version_mismatch"));
                return installation;
            }
            if (hasPackageLayout && current.utf8() == expectedPackageRoot.utf8())
                return Result<EngineInstallation>::failure(invalid("manifest_missing"));
            auto parent = current.parent();
            if (parent.utf8() == current.utf8())
                break;
            current = std::move(parent);
        }
#if CUE_DISTRIBUTION_BUILD
        return Result<EngineInstallation>::failure(invalid("distribution_manifest_missing"));
#else
        auto root = require_type(a_files, a_developmentRoot, FileType::Directory);
        if (!root.has_value())
            return Result<EngineInstallation>::failure(*root.try_error());
        auto editor = executable.try_value()->join("CueEditorHost.exe");
        auto tool = executable.try_value()->join("CueProjectTool.exe");
        if (!editor.has_value() || !tool.has_value())
            return Result<EngineInstallation>::failure(invalid("development_paths"));
        return Result<EngineInstallation>::success({a_developmentRoot,
                                                    editor.take_value(),
                                                    tool.take_value(),
                                                    CUE_ENGINE_VERSION,
                                                    CUE_ENGINE_BUILD_ID,
                                                    {},
                                                    false});
#endif
    }
    catch (const std::bad_alloc &)
    {
        return Result<EngineInstallation>::failure({ErrorCategory::PlatformFailure, "EngineInstallation.allocation"});
    }
}

Result<std::unique_ptr<IFile>> acquire_engine_use_lease(IFileSystem &a_files, const EngineInstallation &a_installation)
{
    if (!a_installation.isPackaged)
        return Result<std::unique_ptr<IFile>>::success({});
    try
    {
        if (!is_valid_engine_version(a_installation.version) ||
            a_installation.useLeaseId != "engine-" + a_installation.version + "-windows-x64")
            return Result<std::unique_ptr<IFile>>::failure(invalid("lease_id"));
        auto local = a_files.local_data_directory();
        if (!local.has_value())
            return Result<std::unique_ptr<IFile>>::failure(*local.try_error());
        auto directory = local.try_value()->join("CueEngine/Locks");
        if (!directory.has_value())
            return Result<std::unique_ptr<IFile>>::failure(*directory.try_error());
        auto prepared = prepare_directory(a_files, *directory.try_value());
        if (!prepared.has_value())
            return Result<std::unique_ptr<IFile>>::failure(*prepared.try_error());
        auto lock = directory.try_value()->join(a_installation.useLeaseId + ".lock");
        if (!lock.has_value())
            return Result<std::unique_ptr<IFile>>::failure(*lock.try_error());
        auto info = a_files.stat(*lock.try_value());
        if (!info.has_value())
            return Result<std::unique_ptr<IFile>>::failure(*info.try_error());
        if (*info.try_value() && (**info.try_value()).type != FileType::Regular)
            return Result<std::unique_ptr<IFile>>::failure(invalid("lease_file_kind"));
        return a_files.open(*lock.try_value(), {FileAccess::ReadWrite, FileCreation::OpenAlways, true, true, false});
    }
    catch (const std::bad_alloc &)
    {
        return Result<std::unique_ptr<IFile>>::failure(
            {ErrorCategory::PlatformFailure, "EngineInstallation.lease_allocation"});
    }
}

Result<Path> locate_engine_cmake(IFileSystem &a_files, const EngineInstallation &a_installation)
{
    try
    {
        std::vector<Path> candidates;
#if defined(_WIN32)
        auto explicitPath = environment_path(L"CUE_CMAKE_EXECUTABLE");
        if (!explicitPath.has_value())
            return Result<Path>::failure(*explicitPath.try_error());
        if (*explicitPath.try_value())
        {
            auto valid = require_type(a_files, **explicitPath.try_value(), FileType::Regular);
            return valid.has_value() ? Result<Path>::success(**explicitPath.try_value())
                                     : Result<Path>::failure(*valid.try_error());
        }
#endif
        auto bundled = a_installation.root.join("Tools/CMake/bin/cmake.exe");
        if (bundled.has_value())
            candidates.push_back(bundled.take_value());
#if defined(_WIN32)
        for (const auto *name : {L"ProgramFiles", L"ProgramFiles(x86)"})
        {
            auto programFiles = environment_path(name);
            if (!programFiles.has_value())
                return Result<Path>::failure(*programFiles.try_error());
            if (!*programFiles.try_value())
                continue;
            auto ordinary = (**programFiles.try_value()).join("CMake/bin/cmake.exe");
            if (ordinary.has_value())
                candidates.push_back(ordinary.take_value());
            for (const auto *edition : {"Community", "Professional", "Enterprise", "BuildTools"})
            {
                auto visualStudio = (**programFiles.try_value())
                                        .join(std::string("Microsoft Visual Studio/18/") + edition +
                                              "/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe");
                if (visualStudio.has_value())
                    candidates.push_back(visualStudio.take_value());
            }
        }
#endif
        for (const auto &candidate : candidates)
        {
            auto info = a_files.stat(candidate);
            if (!info.has_value())
                return Result<Path>::failure(*info.try_error());
            if (!*info.try_value())
                continue;
            auto valid = require_type(a_files, candidate, FileType::Regular);
            if (valid.has_value())
                return Result<Path>::success(candidate);
        }
        return Result<Path>::failure({ErrorCategory::InvalidState, "EngineInstallation.cmake_not_installed"});
    }
    catch (const std::bad_alloc &)
    {
        return Result<Path>::failure({ErrorCategory::PlatformFailure, "EngineInstallation.tools_allocation"});
    }
}
} // namespace cue
