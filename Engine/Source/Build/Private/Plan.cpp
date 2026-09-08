#include <Cue/Build/Plan.h>

#include <Cue/Foundation/Assert.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <utility>

#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

namespace
{
constexpr std::uint32_t k_profileSchemaVersion = 1U;
constexpr std::size_t k_maximumProfileBytes = 4096U;

/// @brief Build Plan処理中の予期しない例外をFatal境界へ渡す
[[noreturn]] void terminate_plan_exception(const cue::AssertContext &a_assertContext) noexcept
{
    a_assertContext.fatal_handler().terminate("Build plan processing failed unexpectedly");
    std::abort();
}

/// @brief Build Plan固有の回復可能Errorを生成する
[[nodiscard]] cue::Error make_plan_error(const cue::AssertContext &a_assertContext, cue::BuildPlanError a_code,
                                         std::string_view a_summary) noexcept
{
    cue::ErrorCode code =
        cue::ErrorCode::create(a_assertContext.fatal_handler(), "Cue.Build.Plan", static_cast<std::int64_t>(a_code));
    return cue::Error::create(a_assertContext.fatal_handler(), std::move(code), a_summary);
}

/// @brief Build Configuration列挙値が公開契約内か判定する
[[nodiscard]] bool is_valid_configuration(cue::BuildConfiguration a_configuration) noexcept
{
    return a_configuration == cue::BuildConfiguration::Debug ||
           a_configuration == cue::BuildConfiguration::Development ||
           a_configuration == cue::BuildConfiguration::Release;
}

/// @brief Build Target列挙値が公開契約内か判定する
[[nodiscard]] bool is_valid_target(cue::BuildTarget a_target) noexcept
{
    return a_target == cue::BuildTarget::GameModule;
}

/// @brief Build Stage列挙値が公開契約内か判定する
[[nodiscard]] bool is_valid_stage(cue::BuildStage a_stage) noexcept
{
    return a_stage == cue::BuildStage::Configure || a_stage == cue::BuildStage::Build;
}

/// @brief Build Stage Outcome列挙値が公開契約内か判定する
[[nodiscard]] bool is_valid_stage_outcome(cue::BuildStageOutcome a_outcome) noexcept
{
    return a_outcome == cue::BuildStageOutcome::Succeeded || a_outcome == cue::BuildStageOutcome::Failed ||
           a_outcome == cue::BuildStageOutcome::Cancelled || a_outcome == cue::BuildStageOutcome::TimedOut;
}

/// @brief Configurationの永続名を返す
[[nodiscard]] std::string_view configuration_name(cue::BuildConfiguration a_configuration) noexcept
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
    return {};
}

/// @brief Configurationに対応するPreset名を返す
[[nodiscard]] std::string_view preset_name(cue::BuildConfiguration a_configuration) noexcept
{
    switch (a_configuration)
    {
    case cue::BuildConfiguration::Debug:
        return "windows-vs2026-debug";
    case cue::BuildConfiguration::Development:
        return "windows-vs2026-development";
    case cue::BuildConfiguration::Release:
        return "windows-vs2026-release";
    }
    return {};
}

/// @brief ConfigurationのWorkspace Key構成名を返す
[[nodiscard]] std::string_view configuration_key_name(cue::BuildConfiguration a_configuration) noexcept
{
    switch (a_configuration)
    {
    case cue::BuildConfiguration::Debug:
        return "debug";
    case cue::BuildConfiguration::Development:
        return "development";
    case cue::BuildConfiguration::Release:
        return "release";
    }
    return {};
}

/// @brief Workspace互換入力が初期Windows Build契約内か判定する
[[nodiscard]] bool is_valid_workspace_compatibility(const cue::BuildWorkspaceCompatibility &a_compatibility) noexcept
{
    const cue::BuildToolVersion &version = a_compatibility.toolsetVersion;
    return a_compatibility.generator == cue::BuildGenerator::VisualStudio2026 &&
           a_compatibility.architecture == cue::BuildArchitecture::X64 && version.major != 0U &&
           a_compatibility.engineBuildPolicyVersion != 0U;
}

/// @brief Generator、Architecture、Toolset、Engine PolicyからFilesystem安全な決定的Keyを作る
[[nodiscard]] std::string make_workspace_key(cue::BuildConfiguration a_configuration,
                                             const cue::BuildWorkspaceCompatibility &a_compatibility)
{
    const cue::BuildToolVersion &version = a_compatibility.toolsetVersion;
    std::string key = "windows-vs2026-x64-msvc-";
    key.append(std::to_string(version.major));
    key.push_back('.');
    key.append(std::to_string(version.minor));
    key.push_back('.');
    key.append(std::to_string(version.patch));
    key.push_back('.');
    key.append(std::to_string(version.build));
    key.append("-policy-");
    key.append(std::to_string(a_compatibility.engineBuildPolicyVersion));
    key.push_back('-');
    key.append(configuration_key_name(a_configuration));
    return key;
}

/// @brief ASCII Hexadecimal文字か判定する
[[nodiscard]] bool is_lower_hex(char a_value) noexcept
{
    return (a_value >= '0' && a_value <= '9') || (a_value >= 'a' && a_value <= 'f');
}

/// @brief lowercase canonical UUID Version 4を検証する
[[nodiscard]] bool is_operation_id(std::string_view a_text) noexcept
{
    if (a_text.size() != 36U || a_text[8] != '-' || a_text[13] != '-' || a_text[18] != '-' || a_text[23] != '-' ||
        a_text[14] != '4' || (a_text[19] != '8' && a_text[19] != '9' && a_text[19] != 'a' && a_text[19] != 'b'))
    {
        return false;
    }
    for (std::size_t index = 0U; index < a_text.size(); ++index)
    {
        if (index == 8U || index == 13U || index == 18U || index == 23U)
        {
            continue;
        }
        if (!is_lower_hex(a_text[index]))
        {
            return false;
        }
    }
    return true;
}

/// @brief Native PathをCMakeへ渡せるUTF-8 generic Pathへ変換する
[[nodiscard]] std::string generic_utf8_path(const std::filesystem::path &a_path)
{
    const std::u8string encoded = a_path.generic_u8string();
    return std::string(reinterpret_cast<const char *>(encoded.data()), encoded.size());
}

/// @brief CandidateがRoot自身ではなくRoot配下か字句的に判定する
[[nodiscard]] bool is_descendant(const std::filesystem::path &a_root, const std::filesystem::path &a_candidate)
{
    auto rootPart = a_root.begin();
    auto candidatePart = a_candidate.begin();
    while (rootPart != a_root.end() && candidatePart != a_candidate.end())
    {
        if (*rootPart != *candidatePart)
        {
            return false;
        }
        ++rootPart;
        ++candidatePart;
    }
    return rootPart == a_root.end() && candidatePart != a_candidate.end();
}

/// @brief Windows Native検査用にAbsolute PathをExtended-length形式へ変換する
[[nodiscard]] std::filesystem::path native_inspection_path(const std::filesystem::path &a_path)
{
#if defined(_WIN32)
    std::filesystem::path preferred = a_path;
    preferred.make_preferred();
    const std::wstring &native = preferred.native();
    if (native.starts_with(L"\\\\?\\"))
    {
        return preferred;
    }
    if (native.starts_with(L"\\\\"))
    {
        std::wstring extended = L"\\\\?\\UNC\\";
        extended.append(native.substr(2U));
        return std::filesystem::path(std::move(extended));
    }
    std::wstring extended = L"\\\\?\\";
    extended.append(native);
    return std::filesystem::path(std::move(extended));
#else
    return a_path;
#endif
}

/// @brief Windows Reparse PointまたはPortable Symbolic Linkか判定する
[[nodiscard]] bool is_reparse_point(const std::filesystem::path &a_path, std::error_code &a_error)
{
#if defined(_WIN32)
    const std::filesystem::path inspectionPath = native_inspection_path(a_path);
    const DWORD attributes = GetFileAttributesW(inspectionPath.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES)
    {
        const DWORD nativeError = GetLastError();
        if (nativeError == ERROR_FILE_NOT_FOUND || nativeError == ERROR_PATH_NOT_FOUND)
        {
            a_error.clear();
            return false;
        }
        a_error = std::error_code(static_cast<int>(nativeError), std::system_category());
        return false;
    }
    a_error.clear();
    return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U;
#else
    const std::filesystem::file_status status =
        std::filesystem::symlink_status(native_inspection_path(a_path), a_error);
    return !a_error && std::filesystem::is_symlink(status);
#endif
}

/// @brief Candidate内の既存Reparse PointまたはDirectory以外のComponentを検出する
[[nodiscard]] bool contains_unsafe_component(const std::filesystem::path &a_root,
                                             const std::filesystem::path &a_candidate)
{
    const std::filesystem::path relative = a_candidate.lexically_relative(a_root);
    if (relative.empty() || relative.is_absolute())
    {
        return true;
    }

    std::filesystem::path current = a_root;
    for (const std::filesystem::path &component : relative)
    {
        current /= component;
        std::error_code statusError;
        if (is_reparse_point(current, statusError))
        {
            return true;
        }
        if (statusError)
        {
            return true;
        }
        const std::filesystem::file_status linkStatus =
            std::filesystem::symlink_status(native_inspection_path(current), statusError);
        if (statusError)
        {
            return statusError != std::errc::no_such_file_or_directory;
        }
        if (!std::filesystem::exists(linkStatus))
        {
            return false;
        }
        if (!std::filesystem::is_directory(linkStatus))
        {
            return true;
        }
    }
    return false;
}

/// @brief Candidateの既存ComponentがDirectoryだけで構成されるか判定する
[[nodiscard]] bool resolves_inside(const std::filesystem::path &a_root, const std::filesystem::path &a_candidate)
{
    return !contains_unsafe_component(a_root, a_candidate);
}

/// @brief JSON Readerの最小Cursor
class ProfileReader final
{
  public:
    explicit ProfileReader(std::string_view a_input) noexcept : m_input(a_input)
    {
    }

    /// @brief JSON空白を読み飛ばす
    void skip_space() noexcept
    {
        while (m_cursor < m_input.size() && (m_input[m_cursor] == ' ' || m_input[m_cursor] == '\t' ||
                                             m_input[m_cursor] == '\r' || m_input[m_cursor] == '\n'))
        {
            ++m_cursor;
        }
    }

    /// @brief 一つの固定記号を読む
    [[nodiscard]] bool read(char a_expected) noexcept
    {
        skip_space();
        if (m_cursor >= m_input.size() || m_input[m_cursor] != a_expected)
        {
            return false;
        }
        ++m_cursor;
        return true;
    }

    /// @brief Escape不要なASCII JSON Stringを読む
    [[nodiscard]] bool read_string(std::string_view &a_output) noexcept
    {
        skip_space();
        if (m_cursor >= m_input.size() || m_input[m_cursor] != '"')
        {
            return false;
        }
        const std::size_t begin = ++m_cursor;
        while (m_cursor < m_input.size() && m_input[m_cursor] != '"')
        {
            const unsigned char value = static_cast<unsigned char>(m_input[m_cursor]);
            if (value < 0x20U || value >= 0x80U || value == '\\')
            {
                return false;
            }
            ++m_cursor;
        }
        if (m_cursor >= m_input.size())
        {
            return false;
        }
        a_output = m_input.substr(begin, m_cursor - begin);
        ++m_cursor;
        return true;
    }

    /// @brief schemaVersionの固定整数1を読む
    [[nodiscard]] bool read_schema_version() noexcept
    {
        skip_space();
        if (m_cursor >= m_input.size() || m_input[m_cursor] != '1')
        {
            return false;
        }
        ++m_cursor;
        if (m_cursor < m_input.size())
        {
            const char next = m_input[m_cursor];
            if ((next >= '0' && next <= '9') || next == '.' || next == 'e' || next == 'E')
            {
                return false;
            }
        }
        return true;
    }

    /// @brief Input末尾へ到達したか判定する
    [[nodiscard]] bool at_end() noexcept
    {
        skip_space();
        return m_cursor == m_input.size();
    }

  private:
    std::string_view m_input;
    std::size_t m_cursor = 0U;
};
} // namespace

namespace cue
{
BuildProfile::BuildProfile(BuildConfiguration a_configuration, BuildTarget a_target) noexcept
    : m_configuration(a_configuration), m_target(a_target)
{
}

Result<BuildProfile> BuildProfile::create(BuildConfiguration a_configuration, BuildTarget a_target,
                                          const AssertContext &a_assertContext) noexcept
{
    if (!is_valid_configuration(a_configuration) || !is_valid_target(a_target))
    {
        return Result<BuildProfile>::failure(
            make_plan_error(a_assertContext, BuildPlanError::InvalidProfile, "Build profile is invalid"));
    }
    return Result<BuildProfile>::success(BuildProfile(a_configuration, a_target));
}

std::uint32_t BuildProfile::schema_version() const noexcept
{
    return k_profileSchemaVersion;
}

BuildConfiguration BuildProfile::configuration() const noexcept
{
    return m_configuration;
}

BuildTarget BuildProfile::target() const noexcept
{
    return m_target;
}

BuildPlan::BuildPlan(std::string a_projectRoot, BuildProfile a_profile, std::string a_operationId,
                     std::string a_presetName, std::string a_workspaceKey,
                     BuildWorkspaceCompatibility a_workspaceCompatibility, std::string a_binaryDirectory,
                     std::string a_candidateDirectory, std::string a_operationDirectory,
                     std::string a_artifactStoreDirectory) noexcept
    : m_projectRoot(std::move(a_projectRoot)), m_profile(a_profile), m_operationId(std::move(a_operationId)),
      m_presetName(std::move(a_presetName)), m_workspaceKey(std::move(a_workspaceKey)),
      m_workspaceCompatibility(a_workspaceCompatibility), m_binaryDirectory(std::move(a_binaryDirectory)),
      m_candidateDirectory(std::move(a_candidateDirectory)), m_operationDirectory(std::move(a_operationDirectory)),
      m_artifactStoreDirectory(std::move(a_artifactStoreDirectory))
{
}

std::string_view BuildPlan::project_root() const noexcept
{
    return m_projectRoot;
}

const BuildProfile &BuildPlan::profile() const noexcept
{
    return m_profile;
}

std::string_view BuildPlan::operation_id() const noexcept
{
    return m_operationId;
}

std::string_view BuildPlan::preset_name() const noexcept
{
    return m_presetName;
}

std::string_view BuildPlan::workspace_key() const noexcept
{
    return m_workspaceKey;
}

const BuildWorkspaceCompatibility &BuildPlan::workspace_compatibility() const noexcept
{
    return m_workspaceCompatibility;
}

std::string_view BuildPlan::binary_directory() const noexcept
{
    return m_binaryDirectory;
}

std::string_view BuildPlan::candidate_directory() const noexcept
{
    return m_candidateDirectory;
}

std::string_view BuildPlan::operation_directory() const noexcept
{
    return m_operationDirectory;
}

std::string_view BuildPlan::artifact_store_directory() const noexcept
{
    return m_artifactStoreDirectory;
}

std::string_view BuildPlan::cmake_target_name() const noexcept
{
    return "CueGameModule";
}

bool BuildPlan::equivalent_to(const BuildPlan &a_other) const noexcept
{
    return m_projectRoot == a_other.m_projectRoot && m_profile == a_other.m_profile &&
           m_operationId == a_other.m_operationId && m_presetName == a_other.m_presetName &&
           m_workspaceKey == a_other.m_workspaceKey && m_workspaceCompatibility == a_other.m_workspaceCompatibility &&
           m_binaryDirectory == a_other.m_binaryDirectory && m_candidateDirectory == a_other.m_candidateDirectory &&
           m_operationDirectory == a_other.m_operationDirectory &&
           m_artifactStoreDirectory == a_other.m_artifactStoreDirectory;
}

BuildStageResult::BuildStageResult(BuildStage a_stage, BuildStageOutcome a_outcome,
                                   std::optional<std::uint32_t> a_exitCode) noexcept
    : m_stage(a_stage), m_outcome(a_outcome), m_exitCode(a_exitCode)
{
}

Result<BuildStageResult> BuildStageResult::create(BuildStage a_stage, BuildStageOutcome a_outcome,
                                                  std::optional<std::uint32_t> a_exitCode,
                                                  const AssertContext &a_assertContext) noexcept
{
    const bool exited = a_outcome == BuildStageOutcome::Succeeded || a_outcome == BuildStageOutcome::Failed;
    const bool exitCodeMatches = exited == a_exitCode.has_value() &&
                                 (a_outcome != BuildStageOutcome::Succeeded || a_exitCode == 0U) &&
                                 (a_outcome != BuildStageOutcome::Failed || a_exitCode != 0U);
    if (!is_valid_stage(a_stage) || !is_valid_stage_outcome(a_outcome) || !exitCodeMatches)
    {
        return Result<BuildStageResult>::failure(
            make_plan_error(a_assertContext, BuildPlanError::InvalidStageResult, "Build stage result is inconsistent"));
    }
    return Result<BuildStageResult>::success(BuildStageResult(a_stage, a_outcome, a_exitCode));
}

BuildStage BuildStageResult::stage() const noexcept
{
    return m_stage;
}

BuildStageOutcome BuildStageResult::outcome() const noexcept
{
    return m_outcome;
}

std::optional<std::uint32_t> BuildStageResult::exit_code() const noexcept
{
    return m_exitCode;
}

Result<std::string> serialize_build_profile(const BuildProfile &a_profile,
                                            const AssertContext &a_assertContext) noexcept
{
    try
    {
        if (!is_valid_configuration(a_profile.configuration()) || !is_valid_target(a_profile.target()))
        {
            return Result<std::string>::failure(
                make_plan_error(a_assertContext, BuildPlanError::InvalidProfile, "Build profile is invalid"));
        }
        std::string json;
        json.reserve(96U);
        json.append("{\n    \"schemaVersion\": 1,\n    \"configuration\": \"");
        json.append(configuration_name(a_profile.configuration()));
        json.append("\",\n    \"target\": \"GameModule\"\n}\n");
        return Result<std::string>::success(std::move(json));
    }
    catch (...)
    {
        terminate_plan_exception(a_assertContext);
    }
}

Result<BuildProfile> parse_build_profile(std::string_view a_json, const AssertContext &a_assertContext) noexcept
{
    try
    {
        if (a_json.size() > k_maximumProfileBytes)
        {
            return Result<BuildProfile>::failure(
                make_plan_error(a_assertContext, BuildPlanError::InvalidProfile, "Build profile is too large"));
        }
        ProfileReader reader(a_json);
        if (!reader.read('{'))
        {
            return Result<BuildProfile>::failure(
                make_plan_error(a_assertContext, BuildPlanError::InvalidProfile, "Build profile JSON is invalid"));
        }

        bool foundSchema = false;
        bool foundConfiguration = false;
        bool foundTarget = false;
        BuildConfiguration configuration = BuildConfiguration::Debug;
        for (std::size_t memberIndex = 0U; memberIndex < 3U; ++memberIndex)
        {
            std::string_view name;
            if (!reader.read_string(name) || !reader.read(':'))
            {
                return Result<BuildProfile>::failure(make_plan_error(a_assertContext, BuildPlanError::InvalidProfile,
                                                                     "Build profile member is invalid"));
            }
            if (name == "schemaVersion" && !foundSchema)
            {
                foundSchema = reader.read_schema_version();
            }
            else if (name == "configuration" && !foundConfiguration)
            {
                std::string_view value;
                if (!reader.read_string(value))
                {
                    foundConfiguration = false;
                }
                else if (value == "Debug")
                {
                    configuration = BuildConfiguration::Debug;
                    foundConfiguration = true;
                }
                else if (value == "Development")
                {
                    configuration = BuildConfiguration::Development;
                    foundConfiguration = true;
                }
                else if (value == "Release")
                {
                    configuration = BuildConfiguration::Release;
                    foundConfiguration = true;
                }
            }
            else if (name == "target" && !foundTarget)
            {
                std::string_view value;
                foundTarget = reader.read_string(value) && value == "GameModule";
            }
            else
            {
                return Result<BuildProfile>::failure(make_plan_error(a_assertContext, BuildPlanError::InvalidProfile,
                                                                     "Build profile has unknown or duplicate members"));
            }
            if ((!foundSchema && name == "schemaVersion") || (!foundConfiguration && name == "configuration") ||
                (!foundTarget && name == "target") || (memberIndex < 2U && !reader.read(',')))
            {
                return Result<BuildProfile>::failure(
                    make_plan_error(a_assertContext, BuildPlanError::InvalidProfile, "Build profile value is invalid"));
            }
        }
        if (!reader.read('}') || !reader.at_end() || !foundSchema || !foundConfiguration || !foundTarget)
        {
            return Result<BuildProfile>::failure(
                make_plan_error(a_assertContext, BuildPlanError::InvalidProfile, "Build profile JSON is invalid"));
        }
        return BuildProfile::create(configuration, BuildTarget::GameModule, a_assertContext);
    }
    catch (...)
    {
        terminate_plan_exception(a_assertContext);
    }
}

Result<BuildPlan> create_build_plan(const BuildRequest &a_request, const AssertContext &a_assertContext) noexcept
{
    try
    {
        if (!is_valid_configuration(a_request.profile.configuration()) || !is_valid_target(a_request.profile.target()))
        {
            return Result<BuildPlan>::failure(
                make_plan_error(a_assertContext, BuildPlanError::InvalidProfile, "Build request profile is invalid"));
        }
        if (!is_valid_workspace_compatibility(a_request.workspaceCompatibility))
        {
            return Result<BuildPlan>::failure(make_plan_error(a_assertContext, BuildPlanError::InvalidProfile,
                                                              "Build workspace compatibility is invalid"));
        }
        if (!is_operation_id(a_request.operationId))
        {
            return Result<BuildPlan>::failure(make_plan_error(a_assertContext, BuildPlanError::InvalidOperationId,
                                                              "Build operation ID is not a canonical UUID v4"));
        }
        if (a_request.projectRoot.find('\0') != std::string_view::npos)
        {
            return Result<BuildPlan>::failure(make_plan_error(a_assertContext, BuildPlanError::InvalidProjectRoot,
                                                              "Build project root contains an embedded null"));
        }

        std::u8string encodedRoot;
        encodedRoot.reserve(a_request.projectRoot.size());
        for (const unsigned char value : a_request.projectRoot)
        {
            encodedRoot.push_back(static_cast<char8_t>(value));
        }
        std::filesystem::path root;
        try
        {
            root = std::filesystem::path(encodedRoot).lexically_normal();
            while (!root.empty() && root != root.root_path() && root.filename().empty())
            {
                root = root.parent_path();
            }
        }
        catch (const std::filesystem::filesystem_error &)
        {
            return Result<BuildPlan>::failure(make_plan_error(a_assertContext, BuildPlanError::InvalidProjectRoot,
                                                              "Build project root encoding is invalid"));
        }
        if (!root.is_absolute() || root.empty() || root == root.root_path())
        {
            return Result<BuildPlan>::failure(make_plan_error(a_assertContext, BuildPlanError::InvalidProjectRoot,
                                                              "Build project root must be an absolute directory"));
        }
        std::error_code rootLinkError;
        if (is_reparse_point(root, rootLinkError) || rootLinkError)
        {
            return Result<BuildPlan>::failure(make_plan_error(a_assertContext, BuildPlanError::InvalidProjectRoot,
                                                              "Build project root must not be a reparse point"));
        }
        std::error_code rootStatusError;
        const std::filesystem::file_status rootStatus =
            std::filesystem::status(native_inspection_path(root), rootStatusError);
        if (rootStatusError || !std::filesystem::is_directory(rootStatus))
        {
            return Result<BuildPlan>::failure(make_plan_error(a_assertContext, BuildPlanError::InvalidProjectRoot,
                                                              "Build project root is not an existing directory"));
        }

        const std::string_view preset = preset_name(a_request.profile.configuration());
        const std::string workspaceKey =
            make_workspace_key(a_request.profile.configuration(), a_request.workspaceCompatibility);
        const std::filesystem::path binary = (root / "Generated" / "Build" / workspaceKey).lexically_normal();
        const std::filesystem::path candidate =
            (root / "Generated" / "Build" / "Candidates" / a_request.operationId).lexically_normal();
        const std::filesystem::path operation =
            (root / "Saved" / "Build" / "Operations" / a_request.operationId).lexically_normal();
        const std::filesystem::path artifact =
            (root / "Generated" / "Artifacts" / configuration_name(a_request.profile.configuration()))
                .lexically_normal();
        if (!is_descendant(root, binary) || !is_descendant(root, candidate) || !is_descendant(root, operation) ||
            !is_descendant(root, artifact) || !resolves_inside(root, binary) || !resolves_inside(root, candidate) ||
            !resolves_inside(root, operation) || !resolves_inside(root, artifact))
        {
            return Result<BuildPlan>::failure(make_plan_error(a_assertContext, BuildPlanError::UnsafeOutputPath,
                                                              "Build output escaped the project root"));
        }

        return Result<BuildPlan>::success(BuildPlan(generic_utf8_path(root), a_request.profile, a_request.operationId,
                                                    std::string(preset), workspaceKey, a_request.workspaceCompatibility,
                                                    generic_utf8_path(binary), generic_utf8_path(candidate),
                                                    generic_utf8_path(operation), generic_utf8_path(artifact)));
    }
    catch (...)
    {
        terminate_plan_exception(a_assertContext);
    }
}
} // namespace cue
