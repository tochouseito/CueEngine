#include "PlayerValidation.h"

#include <array>
#include <charconv>
#include <limits>
#include <memory>
#include <span>
#include <string>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// Command Line の型は Win32 の型宣言後に Include する
#include <shellapi.h>

#include <Foundation/Windows/UtfConversion.h>
#include <Platform/FileSystem.h>

namespace cue
{
namespace
{
/// @brief 行列は Locale 非依存・round-trip 精度で出力し、Math の値を再解釈しない
void append_matrix(std::string &a_text, const char *a_name, const std::optional<math::float4x4> &a_matrix)
{
    a_text += a_name;
    a_text += '=';
    if (a_matrix)
    {
        for (std::size_t row = 0; row < 4; ++row)
            for (std::size_t column = 0; column < 4; ++column)
            {
                if (row || column)
                    a_text += ',';
                std::array<char, 64> value{};
                const auto written =
                    std::to_chars(value.data(), value.data() + value.size(), a_matrix->values[row][column],
                                  std::chars_format::general, std::numeric_limits<float>::max_digits10);
                a_text.append(value.data(), written.ptr);
            }
    }
    a_text += '\n';
}
} // namespace

Result<PlayerValidationOptions> read_player_validation_options()
{
    using OptionsResult = Result<PlayerValidationOptions>;
    try
    {
        int count = 0;
        auto **arguments = CommandLineToArgvW(GetCommandLineW(), &count);
        if (!arguments)
            return OptionsResult::failure({ErrorCategory::PlatformFailure, "GamePlayer.arguments", GetLastError()});
        const auto release = [](wchar_t **a_values) { LocalFree(a_values); };
        std::unique_ptr<wchar_t *, decltype(release)> owned(arguments, release);
        PlayerValidationOptions options;
        options.isRequested = count > 1;
        for (int index = 1; index < count; ++index)
        {
            auto argument = utf16_to_utf8(arguments[index]);
            if (!argument.has_value())
                return OptionsResult::failure(*argument.try_error());
            const auto &key = *argument.try_value();
            if (key == "--warp" && !options.isWarp)
                options.isWarp = true;
            else if (key == "--validate-only" && !options.isValidateOnly)
                options.isValidateOnly = true;
            else if ((key == "--frames" || key == "--validation-report") && ++index < count)
            {
                auto value = utf16_to_utf8(arguments[index]);
                if (!value.has_value())
                    return OptionsResult::failure(*value.try_error());
                const auto &text = *value.try_value();
                if (key == "--frames" && options.maxFrames == 0)
                {
                    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), options.maxFrames);
                    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || options.maxFrames == 0 ||
                        options.maxFrames > 1'000'000)
                        return OptionsResult::failure({ErrorCategory::InvalidArgument, "GamePlayer.frames"});
                }
                else if (key == "--validation-report" && !options.report)
                {
                    auto file = Path::create(text);
                    if (!file.has_value() || !file.try_value()->is_absolute())
                        return OptionsResult::failure({ErrorCategory::InvalidArgument, "GamePlayer.report_path"});
                    options.report = file.take_value();
                }
                else
                    return OptionsResult::failure({ErrorCategory::InvalidArgument, "GamePlayer.duplicate_argument"});
            }
            else
                return OptionsResult::failure({ErrorCategory::InvalidArgument, "GamePlayer.argument"});
        }
        return OptionsResult::success(std::move(options));
    }
    catch (...)
    {
        return OptionsResult::failure({ErrorCategory::Fatal, "GamePlayer.arguments.exception"});
    }
}

void observe_player_snapshot(PlayerValidationMetrics &a_metrics, const SceneRenderSnapshot &a_snapshot)
{
    a_metrics.meshCount = a_snapshot.objects.size();
    a_metrics.hasCamera = a_snapshot.camera.has_value();
    if (!a_snapshot.objects.empty())
    {
        if (!a_metrics.initialWorld)
            a_metrics.initialWorld = a_snapshot.objects.front().world;
        a_metrics.finalWorld = a_snapshot.objects.front().world;
    }
}

Result<void> write_player_validation_report(IFileSystem &a_files, const PlayerValidationOptions &a_options,
                                            const PlayerValidationMetrics &a_metrics)
{
    if (!a_options.report)
        return Result<void>::success();
    try
    {
        std::string text = "formatVersion=1\n";
        text += "staticRegistration=" + std::string(a_metrics.isStaticRegistration ? "true\n" : "false\n");
        text += "completedSimulationTicks=" + std::to_string(a_metrics.completedSimulationTicks) + '\n';
        text += "completedRenders=" + std::to_string(a_metrics.completedRenders) + '\n';
        text += "meshCount=" + std::to_string(a_metrics.meshCount) + '\n';
        text += "hasCamera=" + std::string(a_metrics.hasCamera ? "true\n" : "false\n");
        text += "normalShutdown=" + std::string(a_metrics.hasNormalShutdown ? "true\n" : "false\n");
        append_matrix(text, "initialWorld", a_metrics.initialWorld);
        append_matrix(text, "finalWorld", a_metrics.finalWorld);
        auto written = a_files.replace_file(*a_options.report, std::as_bytes(std::span(text.data(), text.size())));
        return written.has_value() ? Result<void>::success() : Result<void>::failure(*written.try_error());
    }
    catch (...)
    {
        return Result<void>::failure({ErrorCategory::PlatformFailure, "GamePlayer.report.exception"});
    }
}
} // namespace cue
