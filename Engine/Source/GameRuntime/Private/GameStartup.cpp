#include <GameRuntime/GameStartup.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <string>
#include <utility>

#include <Foundation/Utf8.h>
#include <Foundation/Uuid.h>
#include <Platform/FileSystem.h>

namespace cue
{
namespace
{
constexpr std::size_t k_maxConfigBytes = 64 * 1024;
constexpr std::array<std::string_view, 12> k_configKeys{
    "formatVersion", "projectId",    "gameTarget",  "applicationName", "platform",     "architecture",
    "configuration", "startupScene", "windowTitle", "windowWidth",     "windowHeight", "targetFramesPerSecond"};
constexpr std::array<std::string_view, 7> k_packageKeys{
    "formatVersion", "projectId", "gameTarget", "applicationName", "platform", "architecture", "configuration"};

/// @brief 構文と配布状態の失敗を呼出側が報告できる分類へ揃える
Error invalid(const char *a_operation)
{
    return {ErrorCategory::InvalidArgument, a_operation};
}
/// @brief 行末と区切りの空白だけを除去する。値内部の引用符や Escape は解釈しない
std::string_view trim(std::string_view a_value) noexcept
{
    const auto first = a_value.find_first_not_of(" \t\r");
    return first == std::string_view::npos ? std::string_view{}
                                           : a_value.substr(first, a_value.find_last_not_of(" \t\r") - first + 1);
}
/// @brief Runtime の識別子を Path / Command として扱わず、移植可能な単一 token として保持する
bool valid_identifier(std::string_view a_value) noexcept
{
    return !a_value.empty() && a_value.size() <= 128 &&
           std::all_of(a_value.begin(), a_value.end(),
                       [](unsigned char a_char)
                       {
                           return (a_char >= 'A' && a_char <= 'Z') || (a_char >= 'a' && a_char <= 'z') ||
                                  (a_char >= '0' && a_char <= '9') || a_char == '_' || a_char == '-';
                       });
}
/// @brief 必須 Key だけの Schema を所有値へ読む。部分的な設定を公開しない
template <std::size_t Count>
Result<std::array<std::string, Count>> parse_values(std::string_view a_text,
                                                    const std::array<std::string_view, Count> &a_keys)
{
    using ParseResult = Result<std::array<std::string, Count>>;
    if (a_text.empty() || a_text.size() > k_maxConfigBytes || !is_valid_utf8_text(a_text))
        return ParseResult::failure(invalid("GameStartup.text"));
    if (a_text.starts_with("\xef\xbb\xbf"))
        a_text.remove_prefix(3);
    std::array<std::string, Count> values{};
    for (std::size_t offset = 0; offset < a_text.size();)
    {
        const auto newline = a_text.find('\n', offset);
        const auto end = newline == std::string_view::npos ? a_text.size() : newline;
        const auto line = trim(a_text.substr(offset, end - offset));
        offset = end == a_text.size() ? end : end + 1;
        if (line.empty() || line.starts_with('#'))
            continue;
        const auto separator = line.find('=');
        if (separator == std::string_view::npos)
            return ParseResult::failure(invalid("GameStartup.syntax"));
        const auto key = trim(line.substr(0, separator));
        const auto value = trim(line.substr(separator + 1));
        const auto found = std::find(a_keys.begin(), a_keys.end(), key);
        if (found == a_keys.end() || value.empty())
            return ParseResult::failure(invalid("GameStartup.unknown_or_empty_key"));
        const auto index = static_cast<std::size_t>(found - a_keys.begin());
        if (!values[index].empty())
            return ParseResult::failure(invalid("GameStartup.duplicate_key"));
        values[index] = value;
    }
    if (std::any_of(values.begin(), values.end(), [](const auto &a_value) { return a_value.empty(); }))
        return ParseResult::failure(invalid("GameStartup.missing_key"));
    return ParseResult::success(std::move(values));
}
/// @brief 数値を Locale に依存せず読み、余剰文字と表現範囲外を拒否する
Result<std::uint32_t> parse_number(std::string_view a_text, std::uint32_t a_limit, bool a_allowZero)
{
    std::uint32_t value = 0;
    const auto parsed = std::from_chars(a_text.data(), a_text.data() + a_text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != a_text.data() + a_text.size() || value > a_limit ||
        (!a_allowZero && value == 0))
        return Result<std::uint32_t>::failure(invalid("GameStartup.number"));
    return Result<std::uint32_t>::success(value);
}
/// @brief Config / 完成 Marker と Scene の親も含め、Package 内の Link 参照を拒否する
Result<void> validate_file(IFileSystem &a_files, const Path &a_root, const Path &a_file)
{
    std::string prefix = a_root.utf8();
    if (!prefix.ends_with('/'))
        prefix += '/';
    if (!a_file.utf8().starts_with(prefix))
        return Result<void>::failure(invalid("GameStartup.path_outside_package"));
    const std::string_view text(a_file.utf8());
    for (std::size_t offset = prefix.size();;)
    {
        const auto separator = text.find('/', offset);
        auto path = Path::create(separator == std::string_view::npos ? text : text.substr(0, separator));
        if (!path.has_value())
            return Result<void>::failure(*path.try_error());
        auto info = a_files.stat(*path.try_value());
        if (!info.has_value())
            return Result<void>::failure(*info.try_error());
        const auto expected = separator == std::string_view::npos ? FileType::Regular : FileType::Directory;
        if (!*info.try_value() || (**info.try_value()).type != expected)
            return Result<void>::failure(invalid("GameStartup.incomplete_package"));
        if (separator == std::string_view::npos)
            return Result<void>::success();
        offset = separator + 1;
    }
}
/// @brief 検証済みの小さい File だけを読み、借用 Buffer を所有文字列へ複写する
Result<std::string> read_text(IFileSystem &a_files, const Path &a_root, const Path &a_file)
{
    auto valid = validate_file(a_files, a_root, a_file);
    if (!valid.has_value())
        return Result<std::string>::failure(*valid.try_error());
    auto bytes = a_files.read_all(a_file, k_maxConfigBytes);
    if (!bytes.has_value())
        return Result<std::string>::failure(*bytes.try_error());
    const auto &data = *bytes.try_value();
    return Result<std::string>::success(
        std::string(data.empty() ? "" : reinterpret_cast<const char *>(data.data()), data.size()));
}
} // namespace

Result<GameStartupConfig> parse_game_startup(std::string_view a_text)
{
    try
    {
        auto parsed = parse_values(a_text, k_configKeys);
        if (!parsed.has_value())
            return Result<GameStartupConfig>::failure(*parsed.try_error());
        const auto &values = *parsed.try_value();
        if (values[0] != "1" || !is_valid_uuid_text(values[1]))
            return Result<GameStartupConfig>::failure(invalid("GameStartup.version_or_identity"));
        for (const auto index : {2u, 3u, 4u, 5u})
            if (!valid_identifier(values[index]))
                return Result<GameStartupConfig>::failure(invalid("GameStartup.identifier"));
        if (values[6] != "Debug" && values[6] != "Development" && values[6] != "Release")
            return Result<GameStartupConfig>::failure(invalid("GameStartup.configuration"));
        auto scene = Path::create(values[7]);
        if (!scene.has_value() || scene.try_value()->is_absolute() ||
            !scene.try_value()->utf8().starts_with("Content/") || scene.try_value()->extension() != ".cuescene" ||
            values[7].find(':') != std::string::npos)
            return Result<GameStartupConfig>::failure(invalid("GameStartup.scene_path"));
        if (values[8].size() > 256 || std::any_of(values[8].begin(), values[8].end(),
                                                  [](unsigned char a_char) { return a_char < 32 || a_char == 127; }))
            return Result<GameStartupConfig>::failure(invalid("GameStartup.window_title"));
        auto width = parse_number(values[9], 16384, false);
        auto height = parse_number(values[10], 16384, false);
        auto fps = parse_number(values[11], 10000, true);
        if (!width.has_value() || !height.has_value() || !fps.has_value())
            return Result<GameStartupConfig>::failure(invalid("GameStartup.window_or_fps"));
        return Result<GameStartupConfig>::success({values[1], values[2], values[3], values[4], values[5], values[6],
                                                   scene.take_value(), values[8], *width.try_value(),
                                                   *height.try_value(), *fps.try_value()});
    }
    catch (...)
    {
        return Result<GameStartupConfig>::failure({ErrorCategory::PlatformFailure, "GameStartup.parse.exception"});
    }
}

Result<GameStartupConfig> load_game_startup(IFileSystem &a_files, const Path &a_executableDirectory)
{
    try
    {
        if (!a_executableDirectory.is_absolute())
            return Result<GameStartupConfig>::failure(invalid("GameStartup.absolute_directory_required"));
        auto marker = a_executableDirectory.join("GamePackage.cuepackage");
        auto startup = a_executableDirectory.join("GameStartup.cueapp");
        if (!marker.has_value() || !startup.has_value())
            return Result<GameStartupConfig>::failure(invalid("GameStartup.package_path"));
        // コピー中 / 不完全な配置は、起動設定を読む前に完成 Marker の欠損で拒否する
        auto packageText = read_text(a_files, a_executableDirectory, *marker.try_value());
        if (!packageText.has_value())
            return Result<GameStartupConfig>::failure(*packageText.try_error());
        auto package = parse_values(*packageText.try_value(), k_packageKeys);
        if (!package.has_value())
            return Result<GameStartupConfig>::failure(*package.try_error());
        auto configText = read_text(a_files, a_executableDirectory, *startup.try_value());
        if (!configText.has_value())
            return Result<GameStartupConfig>::failure(*configText.try_error());
        auto config = parse_game_startup(*configText.try_value());
        if (!config.has_value())
            return config;
        const auto &value = *config.try_value();
        const std::array<std::string_view, 7> expected{"1",
                                                       value.projectId,
                                                       value.gameTarget,
                                                       value.applicationName,
                                                       value.platform,
                                                       value.architecture,
                                                       value.configuration};
        for (std::size_t index = 0; index < expected.size(); ++index)
            if ((*package.try_value())[index] != expected[index])
                return Result<GameStartupConfig>::failure(invalid("GameStartup.package_identity_mismatch"));
        auto scene = a_executableDirectory.join(value.startupScene);
        if (!scene.has_value())
            return Result<GameStartupConfig>::failure(*scene.try_error());
        auto valid = validate_file(a_files, a_executableDirectory, *scene.try_value());
        if (!valid.has_value())
            return Result<GameStartupConfig>::failure(*valid.try_error());
        // 検証中の Marker 変化を拒否する。同一識別の実行中再配布は別途非対応とする
        auto latest = read_text(a_files, a_executableDirectory, *marker.try_value());
        if (!latest.has_value())
            return Result<GameStartupConfig>::failure(*latest.try_error());
        if (*latest.try_value() != *packageText.try_value())
            return Result<GameStartupConfig>::failure(invalid("GameStartup.package_changed"));
        return config;
    }
    catch (...)
    {
        return Result<GameStartupConfig>::failure({ErrorCategory::PlatformFailure, "GameStartup.load.exception"});
    }
}
} // namespace cue
