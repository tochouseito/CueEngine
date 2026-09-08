#include <Cue/Build/DiagnosticBundle.h>

#include <Cue/Foundation/Assert.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <utility>

namespace
{
constexpr std::size_t k_hardMaximumFileCount = 64U;
constexpr std::uint64_t k_hardMaximumFileBytes = 64U * 1024U * 1024U;
constexpr std::uint64_t k_hardMaximumTotalBytes = 256U * 1024U * 1024U;
constexpr std::array<std::string_view, 8U> k_bundlePaths = {"artifact.json", "environment.json", "manifest.json",
                                                            "plan.json",     "result.json",      "stages.json",
                                                            "stderr.log",    "stdout.log"};

/// @brief 回復不能なBundle内部例外をFatalHandlerへ通知してProcessを停止する
[[noreturn]] void terminate_bundle_exception(const cue::AssertContext &a_assertContext) noexcept
{
    a_assertContext.fatal_handler().terminate("Build diagnostic bundle failed unexpectedly");
    std::abort();
}

/// @brief Bundle固有の回復可能Errorを一貫したDomainで構築する
[[nodiscard]] cue::Error make_bundle_error(const cue::AssertContext &a_assertContext,
                                           cue::BuildDiagnosticBundleError a_code, std::string_view a_summary) noexcept
{
    cue::ErrorCode code = cue::ErrorCode::create(a_assertContext.fatal_handler(), "Cue.Build.DiagnosticBundle",
                                                 static_cast<std::int64_t>(a_code));
    return cue::Error::create(a_assertContext.fatal_handler(), std::move(code), a_summary);
}

/// @brief UUID検証で許可するlowercase hexadecimal文字か判定する
[[nodiscard]] bool is_lower_hex(char a_value) noexcept
{
    return (a_value >= '0' && a_value <= '9') || (a_value >= 'a' && a_value <= 'f');
}

/// @brief Operation IDがlowercase UUID v4のCanonical形式か検証する
[[nodiscard]] bool is_uuid_v4(std::string_view a_text) noexcept
{
    if (a_text.size() != 36U || a_text[8U] != '-' || a_text[13U] != '-' || a_text[18U] != '-' || a_text[23U] != '-' ||
        a_text[14U] != '4' || (a_text[19U] != '8' && a_text[19U] != '9' && a_text[19U] != 'a' && a_text[19U] != 'b'))
    {
        return false;
    }
    for (std::size_t index = 0U; index < a_text.size(); ++index)
    {
        if (index != 8U && index != 13U && index != 18U && index != 23U && !is_lower_hex(a_text[index]))
        {
            return false;
        }
    }
    return true;
}

/// @brief User指定上限が正数かつHard Limit内に収まるか検証する
[[nodiscard]] bool valid_limits(const cue::BuildDiagnosticBundleLimits &a_limits) noexcept
{
    return a_limits.maximumFileCount > 0U && a_limits.maximumFileCount <= k_hardMaximumFileCount &&
           a_limits.maximumFileBytes > 0U && a_limits.maximumFileBytes <= k_hardMaximumFileBytes &&
           a_limits.maximumTotalBytes >= a_limits.maximumFileBytes &&
           a_limits.maximumTotalBytes <= k_hardMaximumTotalBytes;
}

/// @brief Bundle Schemaが許可する固定File Pathか判定する
[[nodiscard]] bool known_bundle_path(std::string_view a_path) noexcept
{
    return std::find(k_bundlePaths.begin(), k_bundlePaths.end(), a_path) != k_bundlePaths.end();
}

/// @brief 公開境界のUTF-8文字列を埋め込みNULなしのNative Filesystem Pathへ変換する
[[nodiscard]] std::optional<std::filesystem::path> filesystem_path_from_utf8(std::string_view a_text)
{
    if (a_text.empty() || a_text.find('\0') != std::string_view::npos)
    {
        return std::nullopt;
    }
    std::u8string encoded;
    encoded.reserve(a_text.size());
    for (const unsigned char value : a_text)
    {
        encoded.push_back(static_cast<char8_t>(value));
    }
    try
    {
        return std::filesystem::path(encoded);
    }
    catch (const std::filesystem::filesystem_error &)
    {
        return std::nullopt;
    }
}

/// @brief 任意Byte列をControl文字を含め妥当なJSON StringへEscapeして追記する
void append_json_string(std::string &a_output, std::string_view a_value)
{
    constexpr char hexDigits[] = "0123456789ABCDEF";
    a_output.push_back('"');
    for (const unsigned char value : a_value)
    {
        switch (value)
        {
        case '"':
            a_output.append("\\\"");
            break;
        case '\\':
            a_output.append("\\\\");
            break;
        case '\n':
            a_output.append("\\n");
            break;
        case '\r':
            a_output.append("\\r");
            break;
        case '\t':
            a_output.append("\\t");
            break;
        default:
            if (value < 0x20U || value == 0x7FU)
            {
                a_output.append("\\u00");
                a_output.push_back(hexDigits[(value >> 4U) & 0x0FU]);
                a_output.push_back(hexDigits[value & 0x0FU]);
            }
            else
            {
                a_output.push_back(static_cast<char>(value));
            }
            break;
        }
    }
    a_output.push_back('"');
}

/// @brief ASCII英大文字だけを小文字へ正規化する
[[nodiscard]] unsigned char fold_ascii(unsigned char a_value) noexcept
{
    return a_value >= 'A' && a_value <= 'Z' ? static_cast<unsigned char>(a_value - 'A' + 'a') : a_value;
}

/// @brief ASCII大小文字とPath Separator表記を区別せず指定Offset以降のPath位置を検索する
[[nodiscard]] std::size_t find_path(std::string_view a_text, std::string_view a_pattern, std::size_t a_offset) noexcept
{
    if (a_pattern.empty() || a_pattern.size() > a_text.size())
    {
        return std::string_view::npos;
    }
    for (std::size_t begin = a_offset; begin + a_pattern.size() <= a_text.size(); ++begin)
    {
        bool equal = true;
        for (std::size_t index = 0U; index < a_pattern.size(); ++index)
        {
            const unsigned char textValue = static_cast<unsigned char>(a_text[begin + index]);
            const unsigned char patternValue = static_cast<unsigned char>(a_pattern[index]);
            const bool textSeparator = textValue == '/' || textValue == '\\';
            const bool patternSeparator = patternValue == '/' || patternValue == '\\';
            equal = equal && ((textSeparator && patternSeparator) || fold_ascii(textValue) == fold_ascii(patternValue));
        }
        if (equal)
        {
            return begin;
        }
    }
    return std::string_view::npos;
}

/// @brief Sensitive Path Prefixを大小文字とSeparator表現を保ったままTokenへ置換する
void replace_path(std::string &a_text, std::string_view a_prefix, std::string_view a_replacement)
{
    std::size_t offset = 0U;
    while (true)
    {
        const std::size_t found = find_path(a_text, a_prefix, offset);
        if (found == std::string::npos)
        {
            return;
        }
        a_text.replace(found, a_prefix.size(), a_replacement);
        offset = found + a_replacement.size();
    }
}

/// @brief 置換後も指定上限内に収まることを確認してSensitive PathをToken化する
[[nodiscard]] bool replace_path_bounded(std::string &a_text, std::string_view a_prefix, std::string_view a_replacement,
                                        std::size_t a_maximumBytes)
{
    std::size_t count = 0U;
    std::size_t offset = 0U;
    while (true)
    {
        const std::size_t found = find_path(a_text, a_prefix, offset);
        if (found == std::string_view::npos)
        {
            break;
        }
        ++count;
        offset = found + a_prefix.size();
    }
    if (a_replacement.size() > a_prefix.size())
    {
        const std::size_t growth = a_replacement.size() - a_prefix.size();
        if (a_text.size() > a_maximumBytes || count > (a_maximumBytes - a_text.size()) / growth)
        {
            return false;
        }
    }
    replace_path(a_text, a_prefix, a_replacement);
    return a_text.size() <= a_maximumBytes;
}

/// @brief Path Redaction規則がBoundedで安全なToken形式か検証する
[[nodiscard]] bool valid_mapping(const cue::BuildDiagnosticPathMapping &a_mapping) noexcept
{
    return !a_mapping.nativePrefix.empty() && a_mapping.nativePrefix.size() <= 4096U &&
           a_mapping.nativePrefix.find('\0') == std::string::npos && a_mapping.replacement.size() >= 3U &&
           a_mapping.replacement.size() <= 64U && a_mapping.replacement.front() == '<' &&
           a_mapping.replacement.back() == '>' &&
           a_mapping.replacement.find_first_of("/\\\r\n\t") == std::string::npos &&
           a_mapping.replacement.find('\0') == std::string::npos;
}

/// @brief 空Prefixと重複を除外してRedaction規則を追加する
void add_mapping(std::vector<cue::BuildDiagnosticPathMapping> &a_mappings, std::string_view a_prefix,
                 std::string_view a_replacement)
{
    if (a_prefix.empty())
    {
        return;
    }
    const auto found =
        std::find_if(a_mappings.begin(), a_mappings.end(),
                     /// @brief 同じNative Prefixを持つ既存Mappingを検出する
                     [a_prefix](const auto &a_mapping) noexcept { return a_mapping.nativePrefix == a_prefix; });
    if (found == a_mappings.end())
    {
        a_mappings.push_back({std::string(a_prefix), std::string(a_replacement)});
    }
}

/// @brief 全Mappingを用いて任意のSeparator表現を含むSensitive PathをToken化する
[[nodiscard]] std::string redact(std::string a_text, const std::vector<cue::BuildDiagnosticPathMapping> &a_mappings)
{
    for (const cue::BuildDiagnosticPathMapping &mapping : a_mappings)
    {
        replace_path(a_text, mapping.nativePrefix, mapping.replacement);
    }
    return a_text;
}

/// @brief 入力Copyと中間置換を含め指定上限を超えない場合だけSensitive PathをToken化する
[[nodiscard]] std::optional<std::string> redact_bounded(std::string_view a_text,
                                                        const std::vector<cue::BuildDiagnosticPathMapping> &a_mappings,
                                                        std::size_t a_maximumBytes)
{
    if (a_text.size() > a_maximumBytes)
    {
        return std::nullopt;
    }
    std::string redacted(a_text);
    for (const cue::BuildDiagnosticPathMapping &mapping : a_mappings)
    {
        if (!replace_path_bounded(redacted, mapping.nativePrefix, mapping.replacement, a_maximumBytes))
        {
            return std::nullopt;
        }
    }
    return redacted;
}

/// @brief Build Operation Stateを永続化用の安定文字列へ変換する
[[nodiscard]] const char *state_text(cue::GameBuildOperationState a_state) noexcept
{
    switch (a_state)
    {
    case cue::GameBuildOperationState::Idle:
        return "idle";
    case cue::GameBuildOperationState::Running:
        return "running";
    case cue::GameBuildOperationState::Succeeded:
        return "succeeded";
    case cue::GameBuildOperationState::Failed:
        return "failed";
    case cue::GameBuildOperationState::Cancelled:
        return "cancelled";
    case cue::GameBuildOperationState::TimedOut:
        return "timedOut";
    }
    return "unknown";
}

/// @brief 永続化済みState文字列をBuild Operation Stateへ検証変換する
[[nodiscard]] std::optional<cue::GameBuildOperationState> parse_state(std::string_view a_value) noexcept
{
    constexpr std::array states = {cue::GameBuildOperationState::Succeeded, cue::GameBuildOperationState::Failed,
                                   cue::GameBuildOperationState::Cancelled, cue::GameBuildOperationState::TimedOut};
    for (const cue::GameBuildOperationState state : states)
    {
        if (a_value == state_text(state))
        {
            return state;
        }
    }
    return std::nullopt;
}

/// @brief Build Stageを永続化用の安定文字列へ変換する
[[nodiscard]] const char *stage_text(cue::BuildStage a_stage) noexcept
{
    return a_stage == cue::BuildStage::Configure ? "configure" : "build";
}

/// @brief Build Stage Outcomeを永続化用の安定文字列へ変換する
[[nodiscard]] const char *outcome_text(cue::BuildStageOutcome a_outcome) noexcept
{
    switch (a_outcome)
    {
    case cue::BuildStageOutcome::Succeeded:
        return "succeeded";
    case cue::BuildStageOutcome::Failed:
        return "failed";
    case cue::BuildStageOutcome::Cancelled:
        return "cancelled";
    case cue::BuildStageOutcome::TimedOut:
        return "timedOut";
    }
    return "unknown";
}

/// @brief Build Configurationを永続化用の安定文字列へ変換する
[[nodiscard]] const char *configuration_text(cue::BuildConfiguration a_configuration) noexcept
{
    switch (a_configuration)
    {
    case cue::BuildConfiguration::Debug:
        return "Debug";
    case cue::BuildConfiguration::Development:
        return "Development";
    case cue::BuildConfiguration::Release:
        return "Release";
    }
    return "Unknown";
}

/// @brief 所有文字列を内容を変えずBundle用Byte列へ変換する
[[nodiscard]] std::vector<std::byte> to_bytes(std::string a_text)
{
    std::vector<std::byte> bytes(a_text.size());
    std::transform(a_text.begin(), a_text.end(), bytes.begin(),
                   /// @brief 一文字を同値のUnsigned Byteへ変換する
                   [](char a_value) noexcept { return static_cast<std::byte>(static_cast<unsigned char>(a_value)); });
    return bytes;
}

/// @brief Bundle Byte列を内容を変えず検証用文字列へ変換する
[[nodiscard]] std::string from_bytes(std::span<const std::byte> a_bytes)
{
    std::string text(a_bytes.size(), '\0');
    std::transform(a_bytes.begin(), a_bytes.end(), text.begin(),
                   /// @brief 一Byteを同値の文字表現へ変換する
                   [](std::byte a_value) noexcept
                   { return static_cast<char>(std::to_integer<unsigned char>(a_value)); });
    return text;
}

/// @brief Build Plan SnapshotをRedact済みJSONへSerializeする
[[nodiscard]] std::string serialize_plan(const cue::BuildDiagnosticPlanSnapshot &a_plan,
                                         const std::vector<cue::BuildDiagnosticPathMapping> &a_mappings)
{
    std::string output = "{\n\"schemaVersion\":1,\n";
    /// @brief Plan FieldをRedactしてJSON Memberとして追記する
    const auto add = [&output, &a_mappings](std::string_view a_name, const std::string &a_value, bool a_last)
    {
        append_json_string(output, a_name);
        output.push_back(':');
        append_json_string(output, redact(a_value, a_mappings));
        output.append(a_last ? "\n" : ",\n");
    };
    add("projectRoot", a_plan.projectRoot, false);
    add("presetName", a_plan.presetName, false);
    add("workspaceKey", a_plan.workspaceKey, false);
    add("binaryDirectory", a_plan.binaryDirectory, false);
    add("candidateDirectory", a_plan.candidateDirectory, false);
    add("operationDirectory", a_plan.operationDirectory, false);
    add("artifactStoreDirectory", a_plan.artifactStoreDirectory, false);
    add("targetName", a_plan.targetName, true);
    output.append("}\n");
    return output;
}

/// @brief Toolchain Environment ReportをRedact済みJSONへSerializeする
[[nodiscard]] std::string serialize_environment(const cue::BuildEnvironmentReport &a_environment,
                                                const std::vector<cue::BuildDiagnosticPathMapping> &a_mappings)
{
    std::string output = "{\n\"schemaVersion\":1,\n\"support\":";
    append_json_string(
        output, a_environment.support == cue::BuildEnvironmentSupport::Supported
                    ? "supported"
                    : (a_environment.support == cue::BuildEnvironmentSupport::Unsupported ? "unsupported" : "unknown"));
    output.append(",\n\"engineSourceRoot\":");
    append_json_string(output, redact(a_environment.engineSourceRoot, a_mappings));
    output.append(",\n\"engineBinaryRoot\":");
    append_json_string(output, redact(a_environment.engineBinaryRoot, a_mappings));
    output.append(",\n\"supportedConfigurations\":[");
    for (std::size_t index = 0U; index < a_environment.supportedConfigurations.size(); ++index)
    {
        if (index > 0U)
        {
            output.push_back(',');
        }
        append_json_string(output, configuration_text(a_environment.supportedConfigurations[index]));
    }
    output.append("],\n\"selectedTools\":[");
    for (std::size_t index = 0U; index < a_environment.selectedTools.size(); ++index)
    {
        const cue::BuildToolCandidate &tool = a_environment.selectedTools[index];
        if (index > 0U)
        {
            output.push_back(',');
        }
        output.append("{\"kind\":");
        output.append(std::to_string(static_cast<std::uint32_t>(tool.kind)));
        output.append(",\"path\":");
        append_json_string(output, redact(tool.nativePath, a_mappings));
        output.append(",\"root\":");
        append_json_string(output, redact(tool.installationRoot, a_mappings));
        output.append(",\"version\":");
        if (tool.version)
        {
            append_json_string(output, std::to_string(tool.version->major) + "." + std::to_string(tool.version->minor) +
                                           "." + std::to_string(tool.version->patch) + "." +
                                           std::to_string(tool.version->build));
        }
        else
        {
            output.append("null");
        }
        output.append(",\"architecture\":");
        append_json_string(output, tool.architecture == cue::BuildArchitecture::X64 ? "x64" : "unknown");
        output.append(",\"available\":");
        output.append(tool.available ? "true" : "false");
        output.push_back('}');
    }
    output.append("],\n\"diagnostics\":[");
    for (std::size_t index = 0U; index < a_environment.diagnostics.size(); ++index)
    {
        const cue::BuildEnvironmentDiagnostic &diagnostic = a_environment.diagnostics[index];
        if (index > 0U)
        {
            output.push_back(',');
        }
        output.append("{\"code\":");
        output.append(std::to_string(static_cast<std::uint32_t>(diagnostic.code)));
        output.append(",\"support\":");
        append_json_string(
            output,
            diagnostic.support == cue::BuildEnvironmentSupport::Supported
                ? "supported"
                : (diagnostic.support == cue::BuildEnvironmentSupport::Unsupported ? "unsupported" : "unknown"));
        output.append(",\"path\":");
        append_json_string(output, redact(diagnostic.nativePath, a_mappings));
        output.append(",\"summary\":");
        append_json_string(output, redact(diagnostic.summary, a_mappings));
        output.append(",\"repairHint\":");
        append_json_string(output, redact(diagnostic.repairHint, a_mappings));
        output.push_back('}');
    }
    output.append("]\n}\n");
    return output;
}

/// @brief 完了済みBuild Stage列をJSONへSerializeする
[[nodiscard]] std::string serialize_stages(const cue::BuildOperationSnapshot &a_operation)
{
    std::string output = "{\n\"schemaVersion\":1,\n\"stages\":[";
    for (std::size_t index = 0U; index < a_operation.stages.size(); ++index)
    {
        const cue::BuildStageSnapshot &stage = a_operation.stages[index];
        if (index > 0U)
        {
            output.push_back(',');
        }
        output.append("{\"stage\":");
        append_json_string(output, stage_text(stage.stage));
        output.append(",\"outcome\":");
        append_json_string(output, outcome_text(stage.outcome));
        output.append(",\"exitCode\":");
        output.append(stage.exitCode ? std::to_string(*stage.exitCode) : "null");
        output.push_back('}');
    }
    output.append("]\n}\n");
    return output;
}

/// @brief Build終端StateとError ChainをRedact済みJSONへSerializeする
[[nodiscard]] std::string serialize_result(const cue::BuildOperationSnapshot &a_operation,
                                           const std::vector<cue::BuildDiagnosticPathMapping> &a_mappings)
{
    std::string output = "{\n\"schemaVersion\":1,\n\"state\":";
    append_json_string(output, state_text(a_operation.state));
    output.append(",\n\"diagnostics\":[");
    for (std::size_t index = 0U; index < a_operation.diagnostics.size(); ++index)
    {
        const cue::BuildDiagnosticSnapshot &diagnostic = a_operation.diagnostics[index];
        if (index > 0U)
        {
            output.push_back(',');
        }
        output.append("{\"domain\":");
        append_json_string(output, diagnostic.domain);
        output.append(",\"code\":");
        output.append(std::to_string(diagnostic.code));
        output.append(",\"summary\":");
        append_json_string(output, redact(diagnostic.summary, a_mappings));
        output.append(",\"contexts\":[");
        for (std::size_t contextIndex = 0U; contextIndex < diagnostic.contexts.size(); ++contextIndex)
        {
            if (contextIndex > 0U)
            {
                output.push_back(',');
            }
            append_json_string(output, redact(diagnostic.contexts[contextIndex], a_mappings));
        }
        output.append("],\"nativeError\":");
        if (diagnostic.nativeError)
        {
            output.append("{\"domain\":");
            append_json_string(output, diagnostic.nativeError->domain);
            output.append(",\"code\":");
            output.append(std::to_string(diagnostic.nativeError->code));
            output.push_back('}');
        }
        else
        {
            output.append("null");
        }
        output.push_back('}');
    }
    output.append("]\n}\n");
    return output;
}

/// @brief 一つのArtifact Inventoryを指定JSON Memberへ追記する
void append_artifact(std::string &a_output, std::string_view a_name, const cue::BuildArtifactInventory &a_artifact)
{
    append_json_string(a_output, a_name);
    a_output.append(":{\"artifactId\":");
    append_json_string(a_output, a_artifact.artifact_id());
    a_output.append(",\"configuration\":");
    append_json_string(a_output, configuration_text(a_artifact.configuration()));
    a_output.append(",\"files\":[");
    for (std::size_t index = 0U; index < a_artifact.files().size(); ++index)
    {
        const cue::BuildArtifactFile &file = a_artifact.files()[index];
        if (index > 0U)
        {
            a_output.push_back(',');
        }
        a_output.append("{\"path\":");
        append_json_string(a_output, file.relativePath);
        a_output.append(",\"sizeBytes\":");
        a_output.append(std::to_string(file.byteSize));
        a_output.append(",\"contentHash\":");
        append_json_string(a_output, file.contentHash);
        a_output.push_back('}');
    }
    a_output.append("]}");
}

/// @brief CurrentとLatest Successful Artifact MetadataをJSONへSerializeする
[[nodiscard]] std::string serialize_artifacts(const cue::BuildOperationSnapshot &a_operation)
{
    std::string output = "{\n\"schemaVersion\":1,\n";
    bool needsComma = false;
    if (a_operation.artifact)
    {
        append_artifact(output, "operationArtifact", *a_operation.artifact);
        needsComma = true;
    }
    if (a_operation.latestSuccessfulArtifact)
    {
        if (needsComma)
        {
            output.append(",\n");
        }
        append_artifact(output, "latestSuccessfulArtifact", *a_operation.latestSuccessfulArtifact);
    }
    output.append("\n}\n");
    return output;
}

/// @brief 指定Streamかつ現在Operationに属するLogをRedactして連結する
[[nodiscard]] std::optional<std::string> serialize_log(const cue::BuildOperationSnapshot &a_operation,
                                                       cue::ChildProcessStream a_stream,
                                                       const std::vector<cue::BuildDiagnosticPathMapping> &a_mappings,
                                                       std::size_t a_maximumBytes)
{
    std::string output;
    for (const cue::BuildLogSnapshot &log : a_operation.logs)
    {
        if (log.operationId == a_operation.operationId && log.stream == a_stream)
        {
            const std::size_t remaining = a_maximumBytes - output.size();
            std::optional<std::string> redacted = redact_bounded(log.bytes, a_mappings, remaining);
            if (!redacted)
            {
                return std::nullopt;
            }
            output.append(*redacted);
        }
    }
    return output;
}

/// @brief Size Policyを適用しながら一FileをBundle候補へ追加する
[[nodiscard]] std::optional<cue::BuildDiagnosticBundleError> add_file(
    std::vector<cue::BuildDiagnosticBundleFile> &a_files, std::string a_path, std::string a_text,
    const cue::BuildDiagnosticBundleLimits &a_limits, std::uint64_t &a_totalBytes)
{
    const std::uint64_t size = static_cast<std::uint64_t>(a_text.size());
    if (a_files.size() >= a_limits.maximumFileCount)
    {
        return cue::BuildDiagnosticBundleError::FileCountLimitExceeded;
    }
    if (size > a_limits.maximumFileBytes)
    {
        return cue::BuildDiagnosticBundleError::FileSizeLimitExceeded;
    }
    if (size > a_limits.maximumTotalBytes - a_totalBytes)
    {
        return cue::BuildDiagnosticBundleError::TotalSizeLimitExceeded;
    }
    a_totalBytes += size;
    a_files.push_back({std::move(a_path), to_bytes(std::move(a_text))});
    return std::nullopt;
}

/// @brief 収集項目と欠損理由を再読込可能なManifest JSONへSerializeする
[[nodiscard]] std::string serialize_manifest(std::string_view a_operationId, cue::GameBuildOperationState a_state,
                                             std::span<const cue::BuildDiagnosticManifestEntry> a_entries)
{
    std::string output = "{\n\"schemaVersion\":1,\n\"operationId\":";
    append_json_string(output, a_operationId);
    output.append(",\n\"state\":");
    append_json_string(output, state_text(a_state));
    output.append(",\n\"entries\":[\n");
    for (std::size_t index = 0U; index < a_entries.size(); ++index)
    {
        const cue::BuildDiagnosticManifestEntry &entry = a_entries[index];
        output.append("{\"path\":");
        append_json_string(output, entry.relativePath);
        output.append(",\"collected\":");
        output.append(entry.collected ? "true" : "false");
        output.append(",\"sizeBytes\":");
        output.append(std::to_string(entry.byteSize));
        output.append(",\"missingReason\":");
        append_json_string(output, entry.missingReason);
        output.append(index + 1U == a_entries.size() ? "}\n" : "},\n");
    }
    output.append("]\n}\n");
    return output;
}

/// @brief 固定Prefix直後の単純なQuoted JSON値を検証用に抽出する
[[nodiscard]] std::optional<std::string_view> extract_quoted_value(std::string_view a_text,
                                                                   std::string_view a_prefix) noexcept
{
    const std::size_t begin = a_text.find(a_prefix);
    if (begin == std::string_view::npos)
    {
        return std::nullopt;
    }
    const std::size_t valueBegin = begin + a_prefix.size();
    const std::size_t end = a_text.find('"', valueBegin);
    if (end == std::string_view::npos)
    {
        return std::nullopt;
    }
    return a_text.substr(valueBegin, end - valueBegin);
}

/// @brief 一行Manifest Entryを既知Pathと整合条件付きでParseする
[[nodiscard]] bool parse_manifest_entry(std::string_view a_line, cue::BuildDiagnosticManifestEntry &a_entry)
{
    const auto path = extract_quoted_value(a_line, "{\"path\":\"");
    const std::size_t collectedPosition = a_line.find("\",\"collected\":");
    const std::size_t sizePosition = a_line.find(",\"sizeBytes\":");
    const auto missingReason = extract_quoted_value(a_line, ",\"missingReason\":\"");
    if (!path || !missingReason || collectedPosition == std::string_view::npos ||
        sizePosition == std::string_view::npos || !known_bundle_path(*path) || *path == "manifest.json")
    {
        return false;
    }
    const std::size_t collectedBegin = collectedPosition + 14U;
    const std::string_view collectedText = a_line.substr(collectedBegin, sizePosition - collectedBegin);
    const std::size_t sizeBegin = sizePosition + 13U;
    const std::size_t sizeEnd = a_line.find(',', sizeBegin);
    if ((collectedText != "true" && collectedText != "false") || sizeEnd == std::string_view::npos)
    {
        return false;
    }
    std::uint64_t byteSize = 0U;
    const auto parsed = std::from_chars(a_line.data() + sizeBegin, a_line.data() + sizeEnd, byteSize);
    if (parsed.ec != std::errc{} || parsed.ptr != a_line.data() + sizeEnd ||
        (collectedText == "false" && (byteSize != 0U || missingReason->empty())) ||
        (collectedText == "true" && !missingReason->empty()))
    {
        return false;
    }
    a_entry = {std::string(*path), collectedText == "true", byteSize, std::string(*missingReason)};
    return true;
}

/// @brief Manifestの収集宣言と実際のBundle File集合が一致するか検証する
[[nodiscard]] bool files_match_manifest(std::span<const cue::BuildDiagnosticBundleFile> a_files,
                                        std::span<const cue::BuildDiagnosticManifestEntry> a_entries) noexcept
{
    for (const cue::BuildDiagnosticManifestEntry &entry : a_entries)
    {
        const auto file = std::find_if(a_files.begin(), a_files.end(),
                                       /// @brief Manifest Entryと同じ相対PathのFileを検出する
                                       [&entry](const auto &a_candidate) noexcept
                                       { return a_candidate.relativePath == entry.relativePath; });
        if (entry.collected != (file != a_files.end()) ||
            (file != a_files.end() && file->bytes.size() != entry.byteSize))
        {
            return false;
        }
    }
    for (const cue::BuildDiagnosticBundleFile &file : a_files)
    {
        if (file.relativePath == "manifest.json")
        {
            continue;
        }
        const auto entry =
            std::find_if(a_entries.begin(), a_entries.end(),
                         /// @brief Fileと同じPathを収集済みとするManifest Entryを検出する
                         [&file](const auto &a_candidate) noexcept
                         { return a_candidate.collected && a_candidate.relativePath == file.relativePath; });
        if (entry == a_entries.end())
        {
            return false;
        }
    }
    return true;
}

/// @brief Manifestが全既知項目を重複なく列挙し必須Fileを収集済みか検証する
[[nodiscard]] bool valid_manifest_entries(std::span<const cue::BuildDiagnosticManifestEntry> a_entries) noexcept
{
    if (a_entries.size() + 1U != k_bundlePaths.size())
    {
        return false;
    }
    for (const std::string_view path : k_bundlePaths)
    {
        if (path == "manifest.json")
        {
            continue;
        }
        const std::size_t count = static_cast<std::size_t>(
            std::count_if(a_entries.begin(), a_entries.end(),
                          /// @brief 現在の既知Pathと一致するManifest Entryを数える
                          [path](const auto &a_entry) noexcept { return a_entry.relativePath == path; }));
        if (count != 1U)
        {
            return false;
        }
        const auto entry =
            std::find_if(a_entries.begin(), a_entries.end(),
                         /// @brief 現在の既知Pathに対応するManifest Entryを取得する
                         [path](const auto &a_candidate) noexcept { return a_candidate.relativePath == path; });
        const bool optional = path == "artifact.json" || path == "environment.json";
        if (!optional && !entry->collected)
        {
            return false;
        }
    }
    return true;
}
} // namespace

namespace cue
{
BuildDiagnosticBundle::BuildDiagnosticBundle(std::string a_operationId, GameBuildOperationState a_state,
                                             std::vector<BuildDiagnosticManifestEntry> a_entries,
                                             std::vector<BuildDiagnosticBundleFile> a_files) noexcept
    : m_operationId(std::move(a_operationId)), m_state(a_state), m_entries(std::move(a_entries)),
      m_files(std::move(a_files))
{
}

std::string_view BuildDiagnosticBundle::operation_id() const noexcept
{
    return m_operationId;
}

GameBuildOperationState BuildDiagnosticBundle::state() const noexcept
{
    return m_state;
}

std::span<const BuildDiagnosticManifestEntry> BuildDiagnosticBundle::manifest_entries() const noexcept
{
    return m_entries;
}

std::span<const BuildDiagnosticBundleFile> BuildDiagnosticBundle::files() const noexcept
{
    return m_files;
}

BuildDiagnosticPlanSnapshot make_build_diagnostic_plan_snapshot(const BuildPlan &a_plan,
                                                                const AssertContext &a_assertContext) noexcept
{
    try
    {
        return {std::string(a_plan.project_root()),
                std::string(a_plan.preset_name()),
                std::string(a_plan.workspace_key()),
                std::string(a_plan.binary_directory()),
                std::string(a_plan.candidate_directory()),
                std::string(a_plan.operation_directory()),
                std::string(a_plan.artifact_store_directory()),
                std::string(a_plan.cmake_target_name())};
    }
    catch (...)
    {
        terminate_bundle_exception(a_assertContext);
    }
}

Result<BuildDiagnosticBundle> create_build_diagnostic_bundle(const BuildDiagnosticBundleInput &a_input,
                                                             const BuildDiagnosticBundleLimits &a_limits,
                                                             const AssertContext &a_assertContext) noexcept
{
    try
    {
        if (!valid_limits(a_limits))
        {
            return Result<BuildDiagnosticBundle>::failure(make_bundle_error(
                a_assertContext, BuildDiagnosticBundleError::InvalidLimits, "Diagnostic bundle limits are invalid"));
        }
        if (!is_uuid_v4(a_input.operation.operationId) || a_input.operation.state == GameBuildOperationState::Idle ||
            a_input.operation.state == GameBuildOperationState::Running || a_input.plan.projectRoot.empty() ||
            a_input.plan.targetName.empty())
        {
            return Result<BuildDiagnosticBundle>::failure(make_bundle_error(
                a_assertContext, BuildDiagnosticBundleError::InvalidInput, "Diagnostic bundle input is invalid"));
        }
        std::vector<BuildDiagnosticPathMapping> mappings = a_input.pathMappings;
        add_mapping(mappings, a_input.plan.projectRoot, "<PROJECT_ROOT>");
        if (a_input.environment)
        {
            add_mapping(mappings, a_input.environment->engineSourceRoot, "<ENGINE_SOURCE_ROOT>");
            add_mapping(mappings, a_input.environment->engineBinaryRoot, "<ENGINE_BINARY_ROOT>");
            for (std::size_t index = 0U; index < a_input.environment->selectedTools.size(); ++index)
            {
                add_mapping(mappings, a_input.environment->selectedTools[index].installationRoot,
                            "<TOOL_ROOT_" + std::to_string(index) + ">");
                add_mapping(mappings, a_input.environment->selectedTools[index].nativePath,
                            "<TOOL_PATH_" + std::to_string(index) + ">");
            }
            for (std::size_t index = 0U; index < a_input.environment->diagnostics.size(); ++index)
            {
                add_mapping(mappings, a_input.environment->diagnostics[index].nativePath,
                            "<DIAGNOSTIC_PATH_" + std::to_string(index) + ">");
            }
        }
        if (!std::all_of(mappings.begin(), mappings.end(), valid_mapping))
        {
            return Result<BuildDiagnosticBundle>::failure(make_bundle_error(
                a_assertContext, BuildDiagnosticBundleError::InvalidInput, "Diagnostic path mapping is invalid"));
        }
        std::sort(mappings.begin(), mappings.end(),
                  /// @brief Longer Prefixを先に置いて包含Pathの部分置換を防ぐ
                  [](const auto &a_left, const auto &a_right) noexcept
                  { return a_left.nativePrefix.size() > a_right.nativePrefix.size(); });

        std::vector<BuildDiagnosticBundleFile> files;
        std::vector<BuildDiagnosticManifestEntry> entries;
        std::uint64_t totalBytes = 0U;
        /// @brief 一つの生成Fileへ上限を適用しManifest収集Entryも同時に記録する
        const auto collect = [&](std::string a_path, std::string a_text) -> std::optional<BuildDiagnosticBundleError>
        {
            const std::uint64_t size = static_cast<std::uint64_t>(a_text.size());
            if (auto failure = add_file(files, a_path, std::move(a_text), a_limits, totalBytes))
            {
                return failure;
            }
            entries.push_back({std::move(a_path), true, size, {}});
            return std::nullopt;
        };

        std::optional<BuildDiagnosticBundleError> failure =
            collect("plan.json", serialize_plan(a_input.plan, mappings));
        if (!failure && a_input.environment)
        {
            failure = collect("environment.json", serialize_environment(*a_input.environment, mappings));
        }
        else if (!a_input.environment)
        {
            entries.push_back({"environment.json", false, 0U, "Environment report was not captured"});
        }
        if (!failure)
        {
            failure = collect("stages.json", serialize_stages(a_input.operation));
        }
        if (!failure)
        {
            failure = collect("result.json", serialize_result(a_input.operation, mappings));
        }
        if (!failure)
        {
            const std::uint64_t maximumBytes =
                std::min(a_limits.maximumFileBytes, a_limits.maximumTotalBytes - totalBytes);
            std::optional<std::string> output = serialize_log(a_input.operation, ChildProcessStream::StandardOutput,
                                                              mappings, static_cast<std::size_t>(maximumBytes));
            failure = output ? collect("stdout.log", std::move(*output))
                             : std::optional<BuildDiagnosticBundleError>(
                                   maximumBytes < a_limits.maximumFileBytes
                                       ? BuildDiagnosticBundleError::TotalSizeLimitExceeded
                                       : BuildDiagnosticBundleError::FileSizeLimitExceeded);
        }
        if (!failure)
        {
            const std::uint64_t maximumBytes =
                std::min(a_limits.maximumFileBytes, a_limits.maximumTotalBytes - totalBytes);
            std::optional<std::string> output = serialize_log(a_input.operation, ChildProcessStream::StandardError,
                                                              mappings, static_cast<std::size_t>(maximumBytes));
            failure = output ? collect("stderr.log", std::move(*output))
                             : std::optional<BuildDiagnosticBundleError>(
                                   maximumBytes < a_limits.maximumFileBytes
                                       ? BuildDiagnosticBundleError::TotalSizeLimitExceeded
                                       : BuildDiagnosticBundleError::FileSizeLimitExceeded);
        }
        if (!failure && (a_input.operation.artifact || a_input.operation.latestSuccessfulArtifact))
        {
            failure = collect("artifact.json", serialize_artifacts(a_input.operation));
        }
        else if (!a_input.operation.artifact && !a_input.operation.latestSuccessfulArtifact)
        {
            entries.push_back({"artifact.json", false, 0U, "No published artifact was available"});
        }
        if (failure)
        {
            return Result<BuildDiagnosticBundle>::failure(
                make_bundle_error(a_assertContext, *failure, "Diagnostic bundle exceeded its configured limits"));
        }
        const std::string manifest =
            serialize_manifest(a_input.operation.operationId, a_input.operation.state, entries);
        failure = add_file(files, "manifest.json", manifest, a_limits, totalBytes);
        if (failure)
        {
            return Result<BuildDiagnosticBundle>::failure(make_bundle_error(
                a_assertContext, *failure, "Diagnostic bundle manifest exceeded its configured limits"));
        }
        std::sort(files.begin(), files.end(),
                  /// @brief Bundle Fileを決定的な相対Path順へ整列する
                  [](const auto &a_left, const auto &a_right) noexcept
                  { return a_left.relativePath < a_right.relativePath; });
        return Result<BuildDiagnosticBundle>::success(BuildDiagnosticBundle(
            a_input.operation.operationId, a_input.operation.state, std::move(entries), std::move(files)));
    }
    catch (...)
    {
        terminate_bundle_exception(a_assertContext);
    }
}

Result<void> write_build_diagnostic_bundle_directory(const BuildDiagnosticBundle &a_bundle,
                                                     std::string_view a_destination,
                                                     const AssertContext &a_assertContext) noexcept
{
    try
    {
        const auto destination = filesystem_path_from_utf8(a_destination);
        if (!destination || !destination->is_absolute())
        {
            return Result<void>::failure(make_bundle_error(a_assertContext, BuildDiagnosticBundleError::InvalidInput,
                                                           "Diagnostic destination must be absolute"));
        }
        std::error_code error;
        if (std::filesystem::exists(*destination, error) || error)
        {
            return Result<void>::failure(make_bundle_error(a_assertContext,
                                                           error ? BuildDiagnosticBundleError::FilesystemFailure
                                                                 : BuildDiagnosticBundleError::DestinationAlreadyExists,
                                                           "Diagnostic destination is unavailable"));
        }
        const std::filesystem::path parent = destination->parent_path();
        const std::filesystem::path name = destination->filename();
        if (parent.empty() || name.empty())
        {
            return Result<void>::failure(make_bundle_error(a_assertContext, BuildDiagnosticBundleError::InvalidInput,
                                                           "Diagnostic destination has no parent or name"));
        }
        std::filesystem::create_directories(parent, error);
        if (error)
        {
            return Result<void>::failure(make_bundle_error(a_assertContext,
                                                           BuildDiagnosticBundleError::FilesystemFailure,
                                                           "Diagnostic destination parent could not be created"));
        }
        const auto stagingSuffix = filesystem_path_from_utf8(".staging-" + std::string(a_bundle.operation_id()));
        if (!stagingSuffix)
        {
            return Result<void>::failure(make_bundle_error(a_assertContext, BuildDiagnosticBundleError::InvalidBundle,
                                                           "Diagnostic bundle operation ID is invalid"));
        }
        std::filesystem::path staging = parent / name;
        staging += *stagingSuffix;
        if (std::filesystem::exists(staging, error) || error || !std::filesystem::create_directory(staging, error) ||
            error)
        {
            return Result<void>::failure(make_bundle_error(a_assertContext,
                                                           BuildDiagnosticBundleError::FilesystemFailure,
                                                           "Diagnostic staging destination is unavailable"));
        }
        bool stagingOwned = true;
        /// @brief この呼出しが所有するStagingだけをRollbackして失敗を返す
        const auto fail = [&](BuildDiagnosticBundleError a_code, std::string_view a_summary)
        {
            std::error_code cleanupError;
            if (stagingOwned)
            {
                std::filesystem::remove_all(staging, cleanupError);
            }
            return Result<void>::failure(make_bundle_error(a_assertContext, a_code, a_summary));
        };
        for (const BuildDiagnosticBundleFile &file : a_bundle.files())
        {
            if (!known_bundle_path(file.relativePath))
            {
                return fail(BuildDiagnosticBundleError::InvalidBundle, "Diagnostic bundle path is invalid");
            }
            std::ofstream stream(staging / file.relativePath, std::ios::binary | std::ios::trunc);
            stream.write(reinterpret_cast<const char *>(file.bytes.data()),
                         static_cast<std::streamsize>(file.bytes.size()));
            stream.flush();
            if (!stream)
            {
                return fail(BuildDiagnosticBundleError::FilesystemFailure, "Diagnostic file write failed");
            }
        }
        std::filesystem::rename(staging, *destination, error);
        if (error)
        {
            std::error_code destinationError;
            const bool destinationExists = std::filesystem::exists(*destination, destinationError);
            return fail(destinationExists && !destinationError ? BuildDiagnosticBundleError::DestinationAlreadyExists
                                                               : BuildDiagnosticBundleError::FilesystemFailure,
                        "Diagnostic destination could not be published");
        }
        stagingOwned = false;
        return Result<void>::success();
    }
    catch (...)
    {
        terminate_bundle_exception(a_assertContext);
    }
}

Result<BuildDiagnosticBundle> read_build_diagnostic_bundle_directory(std::string_view a_source,
                                                                     const BuildDiagnosticBundleLimits &a_limits,
                                                                     const AssertContext &a_assertContext) noexcept
{
    try
    {
        if (!valid_limits(a_limits))
        {
            return Result<BuildDiagnosticBundle>::failure(make_bundle_error(
                a_assertContext, BuildDiagnosticBundleError::InvalidLimits, "Diagnostic bundle limits are invalid"));
        }
        const auto source = filesystem_path_from_utf8(a_source);
        std::error_code error;
        if (!source || !source->is_absolute() || !std::filesystem::is_directory(*source, error) || error)
        {
            return Result<BuildDiagnosticBundle>::failure(make_bundle_error(
                a_assertContext, BuildDiagnosticBundleError::FilesystemFailure, "Diagnostic source is unavailable"));
        }
        std::vector<BuildDiagnosticBundleFile> files;
        std::uint64_t totalBytes = 0U;
        std::optional<BuildDiagnosticBundleError> readFailure;
        for (std::filesystem::directory_iterator iterator(*source, error), end; iterator != end && !error;
             iterator.increment(error))
        {
            const std::filesystem::file_status status = iterator->symlink_status(error);
            if (error)
            {
                break;
            }
            if (!std::filesystem::is_regular_file(status))
            {
                readFailure = BuildDiagnosticBundleError::InvalidBundle;
                break;
            }
            const std::string path = iterator->path().filename().generic_string();
            if (!known_bundle_path(path))
            {
                readFailure = BuildDiagnosticBundleError::InvalidBundle;
                break;
            }
            if (files.size() >= a_limits.maximumFileCount)
            {
                readFailure = BuildDiagnosticBundleError::FileCountLimitExceeded;
                break;
            }
            const std::uint64_t size = iterator->file_size(error);
            if (error)
            {
                break;
            }
            if (size > a_limits.maximumFileBytes ||
                size > static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max()))
            {
                readFailure = BuildDiagnosticBundleError::FileSizeLimitExceeded;
                break;
            }
            if (size > a_limits.maximumTotalBytes - totalBytes)
            {
                readFailure = BuildDiagnosticBundleError::TotalSizeLimitExceeded;
                break;
            }
            std::ifstream stream(iterator->path(), std::ios::binary);
            if (!stream.is_open())
            {
                error = std::make_error_code(std::errc::io_error);
                break;
            }
            std::vector<std::byte> bytes(static_cast<std::size_t>(size));
            stream.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            if (!stream && !bytes.empty())
            {
                error = std::make_error_code(std::errc::io_error);
                break;
            }
            totalBytes += size;
            files.push_back({path, std::move(bytes)});
        }
        if (error)
        {
            return Result<BuildDiagnosticBundle>::failure(make_bundle_error(
                a_assertContext, BuildDiagnosticBundleError::FilesystemFailure, "Diagnostic directory read failed"));
        }
        if (readFailure)
        {
            return Result<BuildDiagnosticBundle>::failure(
                make_bundle_error(a_assertContext, *readFailure, "Diagnostic directory violates the bundle policy"));
        }
        std::sort(files.begin(), files.end(),
                  /// @brief 再読込Fileを決定的な相対Path順へ整列する
                  [](const auto &a_left, const auto &a_right) noexcept
                  { return a_left.relativePath < a_right.relativePath; });
        const auto manifestFile =
            std::find_if(files.begin(), files.end(),
                         /// @brief Bundle SchemaのManifest Fileを検出する
                         [](const auto &a_file) noexcept { return a_file.relativePath == "manifest.json"; });
        if (manifestFile == files.end())
        {
            return Result<BuildDiagnosticBundle>::failure(make_bundle_error(
                a_assertContext, BuildDiagnosticBundleError::InvalidBundle, "Diagnostic manifest is missing"));
        }
        const std::string manifest = from_bytes(manifestFile->bytes);
        const auto operationId = extract_quoted_value(manifest, "\"operationId\":\"");
        const auto stateValue = extract_quoted_value(manifest, "\"state\":\"");
        const auto state = stateValue ? parse_state(*stateValue) : std::nullopt;
        if (manifest.find("\"schemaVersion\":1") == std::string::npos || !operationId || !is_uuid_v4(*operationId) ||
            !state)
        {
            return Result<BuildDiagnosticBundle>::failure(make_bundle_error(
                a_assertContext, BuildDiagnosticBundleError::InvalidBundle, "Diagnostic manifest is invalid"));
        }
        std::vector<BuildDiagnosticManifestEntry> entries;
        std::size_t begin = 0U;
        while (begin < manifest.size())
        {
            const std::size_t end = manifest.find('\n', begin);
            const std::string_view line(manifest.data() + begin,
                                        (end == std::string::npos ? manifest.size() : end) - begin);
            if (line.starts_with("{\"path\":"))
            {
                BuildDiagnosticManifestEntry entry;
                if (!parse_manifest_entry(line, entry))
                {
                    return Result<BuildDiagnosticBundle>::failure(
                        make_bundle_error(a_assertContext, BuildDiagnosticBundleError::InvalidBundle,
                                          "Diagnostic manifest entry is invalid"));
                }
                entries.push_back(std::move(entry));
            }
            if (end == std::string::npos)
            {
                break;
            }
            begin = end + 1U;
        }
        if (!valid_manifest_entries(entries) || manifest != serialize_manifest(*operationId, *state, entries) ||
            !files_match_manifest(files, entries))
        {
            return Result<BuildDiagnosticBundle>::failure(
                make_bundle_error(a_assertContext, BuildDiagnosticBundleError::InvalidBundle,
                                  "Diagnostic manifest does not match directory files"));
        }
        return Result<BuildDiagnosticBundle>::success(
            BuildDiagnosticBundle(std::string(*operationId), *state, std::move(entries), std::move(files)));
    }
    catch (...)
    {
        terminate_bundle_exception(a_assertContext);
    }
}
} // namespace cue
