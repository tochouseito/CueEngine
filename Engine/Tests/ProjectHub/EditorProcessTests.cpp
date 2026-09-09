#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Fatal.h>
#include <Cue/Foundation/Log.h>
#include <Cue/Foundation/Windows/UtfConversion.h>
#include <Cue/IO/Windows/WindowsFilesystem.h>
#include <Cue/Project/Compatibility.h>
#include <Cue/ProjectHub/Windows/WindowsProjectHubPlatform.h>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <Windows.h>

namespace
{
/// @brief Test中の回復不能状態を固定Exit Codeへ変換する
class TestFatalHandler final : public cue::FatalHandler
{
  public:
    /// @brief MessageなしFatalを固定Exit Codeへ変換する
    [[noreturn]] void terminate() noexcept override
    {
        std::_Exit(75);
    }

    /// @brief Message付きFatalを固定Exit Codeへ変換する
    [[noreturn]] void terminate(std::string_view) noexcept override
    {
        std::_Exit(76);
    }
};

/// @brief Editor Process Test専用の一時WorkspaceとProject親Folderを所有する
class TestDirectory final
{
  public:
    /// @brief ProcessとTick値から一意な一時Directory Treeを作成する
    TestDirectory()
    {
        std::array<wchar_t, MAX_PATH> temporary{};
        const DWORD length = GetTempPathW(static_cast<DWORD>(temporary.size()), temporary.data());
        if (length == 0 || length >= temporary.size())
        {
            return;
        }
        m_root = temporary.data();
        m_root += L"CueProjectHubEditorProcessTests-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                  std::to_wstring(GetTickCount64());
        m_workspace = m_root + L"\\Workspace";
        m_projects = m_root + L"\\Projects With Space";
        m_marker = m_projects + L"\\LaunchGame\\EditorProcessProbe.ok";
        m_isCreated = CreateDirectoryW(m_root.c_str(), nullptr) != FALSE &&
                      CreateDirectoryW(m_workspace.c_str(), nullptr) != FALSE &&
                      CreateDirectoryW(m_projects.c_str(), nullptr) != FALSE;
    }

    /// @brief 一時Directory所有権の複製を禁止する
    TestDirectory(const TestDirectory &) = delete;
    /// @brief 一時Directory所有権の複製を禁止する
    TestDirectory &operator=(const TestDirectory &) = delete;

    /// @brief Testが作成した一意な一時Directory Treeだけを再帰削除する
    ~TestDirectory()
    {
        if (m_isCreated)
        {
            std::error_code error;
            static_cast<void>(std::filesystem::remove_all(m_root, error));
        }
    }

    /// @brief 一時Directory Treeの作成結果を返す
    [[nodiscard]] bool is_created() const noexcept
    {
        return m_isCreated;
    }

    /// @brief Recent Registry用Workspace Locatorを返す
    [[nodiscard]] std::wstring_view workspace() const noexcept
    {
        return m_workspace;
    }

    /// @brief Blank Project生成先の親Locatorを返す
    [[nodiscard]] std::wstring_view projects() const noexcept
    {
        return m_projects;
    }

    /// @brief Child Processが作成する成功Marker Locatorを返す
    [[nodiscard]] const std::wstring &marker() const noexcept
    {
        return m_marker;
    }

  private:
    std::wstring m_root;
    std::wstring m_workspace;
    std::wstring m_projects;
    std::wstring m_marker;
    bool m_isCreated = false;
};

/// @brief Test用Loggerを追加Sinkなしで生成する
[[nodiscard]] std::unique_ptr<cue::Logger> create_logger(TestFatalHandler &a_handler)
{
    std::vector<std::unique_ptr<cue::LogSink>> sinks;
    return std::make_unique<cue::Logger>(a_handler, std::move(sinks));
}

/// @brief UTF-16 Test LocatorをProject Hub入力用UTF-8へ変換する
[[nodiscard]] std::string to_utf8(std::wstring_view a_text, cue::AssertContext &a_context)
{
    std::string converted;
    const cue::WindowsUtfConversionResult result =
        cue::convert_windows_utf16_to_utf8(a_text, converted, a_context.fatal_handler());
    return result.status == cue::WindowsUtfConversionStatus::Success ? std::move(converted) : std::string();
}

/// @brief Test実行Fileと同じDirectoryにあるEditor Process Probe Locatorを返す
[[nodiscard]] std::string locate_probe(cue::AssertContext &a_context)
{
    std::array<wchar_t, 32768> module{};
    const DWORD length = GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
    if (length == 0 || length >= module.size())
    {
        return {};
    }
    std::wstring path(module.data(), length);
    const std::size_t separator = path.find_last_of(L"\\/");
    if (separator == std::wstring::npos)
    {
        return {};
    }
    path.resize(separator + 1);
    path.append(L"CueProjectHubEditorProcessProbe.exe");
    return to_utf8(path, a_context);
}

/// @brief Editor Process Test用の最小Project Hub設定を生成する
[[nodiscard]] cue::Result<cue::project_hub::ProjectHubConfiguration> make_configuration(
    const cue::AssertContext &a_context) noexcept
{
    cue::Result<cue::ProjectCapabilityProfile> profile = cue::ProjectCapabilityProfile::create({}, a_context);
    cue::Result<cue::ProjectCapabilitySnapshot> snapshot = cue::ProjectCapabilitySnapshot::create({}, a_context);
    if (!profile)
    {
        return cue::Result<cue::project_hub::ProjectHubConfiguration>::failure(std::move(*profile.try_error()));
    }
    if (!snapshot)
    {
        return cue::Result<cue::project_hub::ProjectHubConfiguration>::failure(std::move(*snapshot.try_error()));
    }
    cue::project_hub::ProjectHubConfiguration configuration{
        cue::k_currentProjectDescriptorSchemaVersion, cue::EngineVersion{1U, 0U, 0U}, std::move(*profile.try_value()),
        std::move(*snapshot.try_value()),
        cue::EngineCompatibility{cue::EngineVersion{1U, 0U, 0U}, cue::EngineVersion{2U, 0U, 0U}}};
    return cue::Result<cue::project_hub::ProjectHubConfiguration>::success(std::move(configuration));
}

/// @brief Launch Requestの全必須引数が独立Processへ渡り成功Markerを生成することを検証する
[[nodiscard]] bool test_editor_process(cue::AssertContext &a_context)
{
    TestDirectory directory;
    const std::string workspaceLocator = to_utf8(directory.workspace(), a_context);
    const std::string projectsLocator = to_utf8(directory.projects(), a_context);
    const std::string probeLocator = locate_probe(a_context);
    cue::Result<std::unique_ptr<cue::FilesystemRoot>> workspace =
        cue::create_windows_filesystem_root(workspaceLocator, a_context);
    cue::Result<std::unique_ptr<cue::project_hub::ProjectHubPlatform>> platform =
        cue::project_hub::create_windows_project_hub_platform(a_context);
    cue::Result<cue::project_hub::ProjectHubConfiguration> configuration = make_configuration(a_context);
    if (!directory.is_created() || workspaceLocator.empty() || projectsLocator.empty() || probeLocator.empty() ||
        !workspace || !platform || !configuration)
    {
        return false;
    }
    cue::Result<std::unique_ptr<cue::project_hub::ProjectHubService>> service =
        cue::project_hub::ProjectHubService::create(**workspace.try_value(), **platform.try_value(),
                                                    std::move(*configuration.try_value()), a_context);
    if (!service)
    {
        return false;
    }
    cue::Result<cue::project_hub::ProjectCreationOutcome> created = service.try_value()->get()->create_blank_project(
        projectsLocator, "LaunchGame", "Launch Game", cue::project_hub::k_blank3dTemplateId, 1U);
    if (!created || service.try_value()->get()->projects().size() != 1U)
    {
        return false;
    }
    const std::string projectId = service.try_value()->get()->projects()[0].projectId;
    cue::Result<cue::project_hub::EditorLaunchRequest> request =
        service.try_value()->get()->open_project(projectId, 2U);
    if (!request)
    {
        return false;
    }
    cue::Result<std::unique_ptr<cue::project_hub::WindowsEditorProcess>> launched =
        cue::project_hub::launch_windows_editor_process(probeLocator, *request.try_value(), a_context);
    if (!launched)
    {
        return false;
    }
    bool didExitNormally = false;
    for (std::uint32_t attempt = 0; attempt < 500U; ++attempt)
    {
        cue::Result<bool> processState = launched.try_value()->get()->poll();
        if (!processState)
        {
            return false;
        }
        if (!*processState.try_value())
        {
            didExitNormally = true;
            break;
        }
        Sleep(10);
    }
    if (!didExitNormally || GetFileAttributesW(directory.marker().c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        return false;
    }

    if (!SetEnvironmentVariableW(L"CUE_EDITOR_PROCESS_PROBE_EXIT_CODE", L"23"))
    {
        return false;
    }
    cue::Result<std::unique_ptr<cue::project_hub::WindowsEditorProcess>> abnormal =
        cue::project_hub::launch_windows_editor_process(probeLocator, *request.try_value(), a_context);
    static_cast<void>(SetEnvironmentVariableW(L"CUE_EDITOR_PROCESS_PROBE_EXIT_CODE", nullptr));
    if (!abnormal)
    {
        return false;
    }
    for (std::uint32_t attempt = 0; attempt < 500U; ++attempt)
    {
        cue::Result<bool> processState = abnormal.try_value()->get()->poll();
        if (!processState)
        {
            return processState.try_error()->code().domain() == "Cue.ProjectHub" &&
                   processState.try_error()->root_code().domain() == "Editor.ExitCode" &&
                   processState.try_error()->root_code().value() == 23;
        }
        if (!*processState.try_value())
        {
            return false;
        }
        Sleep(10);
    }
    return false;
}
} // namespace

/// @brief Editor Process起動とLaunch Request Command Line契約をProcess単位で検証する
int main()
{
    TestFatalHandler handler;
    std::unique_ptr<cue::Logger> logger = create_logger(handler);
    cue::AssertContext context(*logger, handler);
    return test_editor_process(context) ? 0 : 1;
}
