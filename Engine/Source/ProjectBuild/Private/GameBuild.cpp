#include <ProjectBuild/GameBuild.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

// CNG の型は Windows の宣言後に読み、公開 Header へ Native 型を漏らさない
#include <bcrypt.h>
#endif

#include <Foundation/Utf8.h>
#include <Platform/FileSystem.h>
#include <Project/Project.h>

namespace cue
{
namespace
{
constexpr std::array<std::string_view, 11> k_keys{
    "formatVersion", "projectId", "gameTarget",    "configuration", "platform",           "architecture",
    "abiVersion",    "module",    "staticLibrary", "moduleSha256",  "staticLibrarySha256"};

/// @brief Manifest の区切りだけを除去し、値へ引用符や Escape を持ち込まない
std::string_view trim(std::string_view a_text) noexcept
{
    const auto first = a_text.find_first_not_of(" \t\r");
    return first == std::string_view::npos ? std::string_view{}
                                           : a_text.substr(first, a_text.find_last_not_of(" \t\r") - first + 1);
}
/// @brief Manifest / File の不一致を Editor が表示・再試行できる Error にする
Error invalid(std::string a_operation)
{
    return {ErrorCategory::InvalidArgument, "GameBuild." + std::move(a_operation)};
}
/// @brief 固定の成果物 Root 以下を歩き、Link の先にある別 Project 成果物を選ばない
Result<void> validate_file(IFileSystem &a_files, const Path &a_root, const Path &a_file)
{
    auto prefix = a_root.utf8();
    if (!prefix.ends_with('/'))
        prefix += '/';
    if (!a_file.utf8().starts_with(prefix))
        return Result<void>::failure(invalid("artifact_outside_project"));
    const std::string_view text = a_file.utf8();
    for (std::size_t offset = prefix.size();;)
    {
        const auto separator = text.find('/', offset);
        const auto part = separator == std::string_view::npos ? text : text.substr(0, separator);
        auto path = Path::create(part);
        if (!path.has_value())
            return Result<void>::failure(*path.try_error());
        auto info = a_files.stat(*path.try_value());
        if (!info.has_value())
            return Result<void>::failure(*info.try_error());
        if (!*info.try_value())
            return Result<void>::failure({ErrorCategory::InvalidState, "GameBuild.not_built:" + a_file.utf8()});
        const auto expected = separator == std::string_view::npos ? FileType::Regular : FileType::Directory;
        if ((**info.try_value()).type != expected)
            return Result<void>::failure(invalid("artifact_kind:" + path.try_value()->utf8()));
        if (separator == std::string_view::npos)
            return Result<void>::success();
        offset = separator + 1;
    }
}

/// @brief 両方のビルド完了後に公開された内容だけを厳密に読む
Result<std::array<std::string, k_keys.size()>> parse_manifest(std::string_view a_text)
{
    using ParseResult = Result<std::array<std::string, k_keys.size()>>;
    if (a_text.empty() || a_text.size() > 4096 || !is_valid_utf8_text(a_text))
        return ParseResult::failure(invalid("manifest_text"));
    std::array<std::string, k_keys.size()> values{};
    for (std::size_t offset = 0; offset < a_text.size();)
    {
        const auto newline = a_text.find('\n', offset);
        const auto end = newline == std::string_view::npos ? a_text.size() : newline;
        const auto line = trim(a_text.substr(offset, end - offset));
        offset = end == a_text.size() ? end : end + 1;
        if (line.empty())
            continue;
        const auto separator = line.find('=');
        if (separator == std::string_view::npos)
            return ParseResult::failure(invalid("manifest_syntax"));
        const auto key = trim(line.substr(0, separator));
        const auto value = trim(line.substr(separator + 1));
        const auto found = std::find(k_keys.begin(), k_keys.end(), key);
        if (found == k_keys.end() || value.empty())
            return ParseResult::failure(invalid("manifest_key:" + std::string(key)));
        const auto index = static_cast<std::size_t>(found - k_keys.begin());
        if (!values[index].empty())
            return ParseResult::failure(invalid("manifest_duplicate:" + std::string(key)));
        values[index] = value;
    }
    if (std::any_of(values.begin(), values.end(), [](const auto &a_value) { return a_value.empty(); }))
        return ParseResult::failure(invalid("manifest_missing_key"));
    for (const auto index : {9u, 10u})
        if (values[index].size() != 64 ||
            !std::all_of(values[index].begin(), values[index].end(), [](unsigned char a_char)
                         { return (a_char >= '0' && a_char <= '9') || (a_char >= 'a' && a_char <= 'f'); }))
            return ParseResult::failure(invalid("manifest_hash"));
    return ParseResult::success(std::move(values));
}

#if defined(_WIN32)
/// @brief Platform 内部の CNG 所有値。Hash と Provider を File より先に正しい順で回収する
struct HashOwner final
{
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::vector<unsigned char> workspace;
    /// @brief 部分初期化でも Hash を先に破棄する
    ~HashOwner()
    {
        if (hash)
            BCryptDestroyHash(hash);
        if (algorithm)
            BCryptCloseAlgorithmProvider(algorithm, 0);
    }
};
#endif

/// @brief 大容量の成果物を全量確保せず読み、CMake の SHA-256 と照合する
Result<std::string> hash_file(IFileSystem &a_files, const Path &a_file)
{
#if defined(_WIN32)
    using HashResult = Result<std::string>;
    auto opened = a_files.open(a_file);
    if (!opened.has_value())
        return HashResult::failure(*opened.try_error());
    auto file = opened.take_value();
    auto size = file->size();
    if (!size.has_value())
        return HashResult::failure(*size.try_error());
    if (*size.try_value() == 0 || *size.try_value() > 2ull * 1024 * 1024 * 1024)
        return HashResult::failure(invalid("artifact_size:" + a_file.utf8()));
    HashOwner owner;
    auto native = BCryptOpenAlgorithmProvider(&owner.algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    ULONG workspaceBytes = 0;
    ULONG returned = 0;
    if (native >= 0)
        native = BCryptGetProperty(owner.algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&workspaceBytes),
                                   sizeof(workspaceBytes), &returned, 0);
    if (native >= 0)
    {
        owner.workspace.resize(workspaceBytes);
        native = BCryptCreateHash(owner.algorithm, &owner.hash, owner.workspace.data(), workspaceBytes, nullptr, 0, 0);
    }
    if (native < 0)
        return HashResult::failure({ErrorCategory::PlatformFailure, "GameBuild.sha256_initialize", native});
    std::array<std::byte, 64 * 1024> buffer{};
    std::uint64_t total = 0;
    while (true)
    {
        auto read = file->read(buffer);
        if (!read.has_value())
            return HashResult::failure(*read.try_error());
        if (*read.try_value() == 0)
            break;
        total += *read.try_value();
        if (total > *size.try_value())
            return HashResult::failure(invalid("artifact_changed:" + a_file.utf8()));
        native = BCryptHashData(owner.hash, reinterpret_cast<PUCHAR>(buffer.data()),
                                static_cast<ULONG>(*read.try_value()), 0);
        if (native < 0)
            return HashResult::failure({ErrorCategory::PlatformFailure, "GameBuild.sha256_update", native});
    }
    auto finalSize = file->size();
    if (!finalSize.has_value())
        return HashResult::failure(*finalSize.try_error());
    if (total != *size.try_value() || *finalSize.try_value() != *size.try_value())
        return HashResult::failure(invalid("artifact_changed:" + a_file.utf8()));
    std::array<unsigned char, 32> digest{};
    native = BCryptFinishHash(owner.hash, digest.data(), static_cast<ULONG>(digest.size()), 0);
    if (native < 0)
        return HashResult::failure({ErrorCategory::PlatformFailure, "GameBuild.sha256_finish", native});
    auto closed = file->close();
    if (!closed.has_value())
        return HashResult::failure(*closed.try_error());
    std::string value;
    value.reserve(digest.size() * 2);
    constexpr char digits[] = "0123456789abcdef";
    for (auto byte : digest)
    {
        value += digits[byte >> 4];
        value += digits[byte & 15];
    }
    return HashResult::success(std::move(value));
#else
    (void)a_files;
    (void)a_file;
    return Result<std::string>::failure({ErrorCategory::InvalidState, "GameBuild.hash_platform_unsupported"});
#endif
}
} // namespace

Result<Path> game_build_directory(const Project &a_project, const GameBuildTarget &a_target)
{
    auto valid = validate_game_build_target(a_target);
    if (!valid.has_value())
        return Result<Path>::failure(*valid.try_error());
    if (!a_project.descriptor().gameTarget)
        return Result<Path>::failure(invalid("configuration_or_target"));
    return a_project.root().join("Build/" + a_target.platform + '/' + a_target.architecture + '/' +
                                 std::string(game_build_configuration_name(a_target.configuration)));
}

Result<Path> game_build_directory(const Project &a_project, GameBuildConfiguration a_configuration)
{
    auto native = current_native_game_build_target();
    if (!native.has_value())
        return Result<Path>::failure(*native.try_error());
    native.try_value()->configuration = a_configuration;
    return game_build_directory(a_project, *native.try_value());
}

Result<GameBuildArtifacts> load_game_build_artifacts(IFileSystem &a_files, const Project &a_project,
                                                     const GameBuildTarget &a_target)
{
    using ArtifactResult = Result<GameBuildArtifacts>;
    try
    {
        auto directory = game_build_directory(a_project, a_target);
        if (!directory.has_value())
            return ArtifactResult::failure(*directory.try_error());
        auto manifest = directory.try_value()->join("GameModule.cuebuild");
        if (!manifest.has_value())
            return ArtifactResult::failure(*manifest.try_error());
        auto valid = validate_file(a_files, a_project.root(), *manifest.try_value());
        if (!valid.has_value())
            return ArtifactResult::failure(*valid.try_error());
        auto bytes = a_files.read_all(*manifest.try_value(), 4096);
        if (!bytes.has_value())
            return ArtifactResult::failure(*bytes.try_error());
        const auto &data = *bytes.try_value();
        auto parsed = parse_manifest(
            std::string_view(data.empty() ? "" : reinterpret_cast<const char *>(data.data()), data.size()));
        if (!parsed.has_value())
            return ArtifactResult::failure(*parsed.try_error());
        const auto &values = *parsed.try_value();
        const auto &target = *a_project.descriptor().gameTarget;
        const auto &platform = *find_game_build_platform(a_target.platform);
        if (values[0] != "1" || values[1] != a_project.descriptor().projectId || values[2] != target ||
            values[3] != game_build_configuration_name(a_target.configuration) || values[4] != a_target.platform ||
            values[5] != a_target.architecture || values[6] != "1")
            return ArtifactResult::failure(invalid("manifest_identity"));
        if (values[7] != "Runtime/" + target + std::string(platform.moduleSuffix) ||
            values[8] != "Libraries/" + target + "Static" + std::string(platform.staticLibrarySuffix))
            return ArtifactResult::failure(invalid("manifest_artifact_path"));
        auto module = directory.try_value()->join(values[7]);
        auto library = directory.try_value()->join(values[8]);
        if (!module.has_value() || !library.has_value())
            return ArtifactResult::failure(invalid("manifest_artifact_path"));
        for (const auto *file : {module.try_value(), library.try_value()})
        {
            valid = validate_file(a_files, a_project.root(), *file);
            if (!valid.has_value())
                return ArtifactResult::failure(*valid.try_error());
        }
        auto moduleHash = hash_file(a_files, *module.try_value());
        auto libraryHash = hash_file(a_files, *library.try_value());
        if (!moduleHash.has_value() || !libraryHash.has_value())
            return ArtifactResult::failure(!moduleHash.has_value() ? *moduleHash.try_error()
                                                                   : *libraryHash.try_error());
        if (*moduleHash.try_value() != values[9] || *libraryHash.try_value() != values[10])
            return ArtifactResult::failure(invalid("artifact_hash_mismatch"));
        // 検証中の再 Publish とも照合する。返却後の Build は Play 起動前の再検証で扱う
        auto latest = a_files.read_all(*manifest.try_value(), 4096);
        if (!latest.has_value())
            return ArtifactResult::failure(*latest.try_error());
        if (*latest.try_value() != data)
            return ArtifactResult::failure(invalid("manifest_changed"));
        return ArtifactResult::success({directory.take_value(), manifest.take_value(), module.take_value(),
                                        library.take_value(), target, a_target.configuration, a_target});
    }
    catch (...)
    {
        return ArtifactResult::failure({ErrorCategory::PlatformFailure, "GameBuild.load.exception"});
    }
}

Result<GameBuildArtifacts> load_game_build_artifacts(IFileSystem &a_files, const Project &a_project,
                                                     GameBuildConfiguration a_configuration)
{
    auto native = current_native_game_build_target();
    if (!native.has_value())
        return Result<GameBuildArtifacts>::failure(*native.try_error());
    native.try_value()->configuration = a_configuration;
    return load_game_build_artifacts(a_files, a_project, *native.try_value());
}
} // namespace cue
