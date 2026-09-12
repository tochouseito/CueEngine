#include <Cue/Editor/Windows/EditorSession.h>

#include <Cue/EditorCore/EditorIntent.h>
#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Fatal.h>
#include <Cue/Foundation/Log.h>
#include <Cue/Foundation/Windows/UtfConversion.h>
#include <Cue/IO/Windows/WindowsFilesystem.h>
#include <Cue/Project/Compatibility.h>
#include <Cue/Project/Generator.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <Windows.h>

namespace
{
#if CUE_TEST_BUILD_CONFIGURATION == 1
constexpr std::string_view k_buildWorkflowAction = "build-workflow-debug";
constexpr std::string_view k_packageWorkflowAction = "package-workflow-debug";
constexpr std::wstring_view k_packageConfiguration = L"Debug";
#elif CUE_TEST_BUILD_CONFIGURATION == 2
constexpr std::string_view k_buildWorkflowAction = "build-workflow-development";
constexpr std::string_view k_packageWorkflowAction = "package-workflow-development";
constexpr std::wstring_view k_packageConfiguration = L"Development";
#elif CUE_TEST_BUILD_CONFIGURATION == 3
constexpr std::string_view k_buildWorkflowAction = "build-workflow-release";
constexpr std::string_view k_packageWorkflowAction = "package-workflow-release";
constexpr std::wstring_view k_packageConfiguration = L"Release";
#else
#error CUE_TEST_BUILD_CONFIGURATION must identify a supported configuration
#endif
constexpr std::string_view k_shippingWorkflowAction = "shipping-workflow-release";

/// @brief Test内のFatalを固定Exit Codeへ変換する
class TestFatalHandler final : public cue::FatalHandler
{
  public:
    /// @brief MessageなしFatalを固定Exit Codeへ変換する
    [[noreturn]] void terminate() noexcept override
    {
        std::_Exit(76);
    }

    /// @brief Message付きFatalを固定Exit Codeへ変換する
    [[noreturn]] void terminate(std::string_view) noexcept override
    {
        std::_Exit(76);
    }
};

/// @brief Process固有Temporary Project Directoryを一意所有する
class TestDirectory final
{
  public:
    /// @brief Test Workspace下へProcess固有Directoryを作成する
    explicit TestDirectory(const std::filesystem::path &a_workspaceRoot)
    {
        m_path = a_workspaceRoot /
                 (L"CEW-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        std::filesystem::create_directories(m_path);
    }

    TestDirectory(const TestDirectory &) = delete;
    TestDirectory &operator=(const TestDirectory &) = delete;

    /// @brief Test所有Directoryだけを終了時に除去する
    ~TestDirectory()
    {
        const std::filesystem::path cleanupPath(L"\\\\?\\" + m_path.native());
        for (std::size_t attempt = 0U; attempt < 50U; ++attempt)
        {
            std::error_code error;
            std::filesystem::remove_all(cleanupPath, error);
            if (!std::filesystem::exists(cleanupPath, error) && !error)
            {
                return;
            }
            Sleep(100U);
        }
        std::_Exit(77);
    }

    /// @brief Temporary RootのNative Pathを返す
    [[nodiscard]] const std::filesystem::path &path() const noexcept
    {
        return m_path;
    }

  private:
    std::filesystem::path m_path;
};

/// @brief Windows PathをStrict UTF-8へ変換する
[[nodiscard]] std::string to_utf8(const std::filesystem::path &a_path, cue::FatalHandler &a_handler)
{
    std::string converted;
    const cue::WindowsUtfConversionResult result =
        cue::convert_windows_utf16_to_utf8(a_path.native(), converted, a_handler);
    if (result.status != cue::WindowsUtfConversionStatus::Success)
    {
        std::_Exit(2);
    }
    return converted;
}

/// @brief M12 Editorが使用する現在Engine互換性入力を毎回生成する
[[nodiscard]] cue::editor::WindowsEditorEngineConfiguration make_configuration(const cue::AssertContext &a_context)
{
    auto profile = cue::ProjectCapabilityProfile::create({}, a_context);
    auto snapshot = cue::ProjectCapabilitySnapshot::create({}, a_context);
    if (!profile || !snapshot)
    {
        std::_Exit(3);
    }
    return {cue::k_currentProjectDescriptorSchemaVersion, cue::EngineVersion{1U, 0U, 0U},
            std::move(*profile.try_value()), std::move(*snapshot.try_value())};
}

/// @brief 生成Projectへ対応するEditor起動値を作る
[[nodiscard]] cue::editor::WindowsEditorLaunchParameters make_parameters(
    const std::filesystem::path &a_projectPath, std::string_view a_projectId,
    const cue::EngineCompatibility &a_compatibility, const cue::AssertContext &a_context,
    std::optional<std::string> a_initialScene = std::nullopt,
    std::optional<std::string> a_expectedInitialSceneAssetId = std::nullopt)
{
    return {cue::k_editorLaunchProtocolVersion, to_utf8(a_projectPath / L"CueProject.json", a_context.fatal_handler()),
            std::string(a_projectId),           cue::make_engine_compatibility_id(a_compatibility, a_context),
            std::move(a_initialScene),          std::move(a_expectedInitialSceneAssetId)};
}

/// @brief File全体を既存Destination保護の比較用Byte列として読む
[[nodiscard]] std::string read_file(const std::filesystem::path &a_path)
{
    std::ifstream stream(a_path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

/// @brief Test FixtureのProject Descriptor内Identityを一度だけ置換する
[[nodiscard]] bool replace_file_text(const std::filesystem::path &a_path, std::string_view a_from,
                                     std::string_view a_to)
{
    std::string text = read_file(a_path);
    const std::size_t position = text.find(a_from);
    if (position == std::string::npos || text.find(a_from, position + a_from.size()) != std::string::npos)
    {
        return false;
    }
    text.replace(position, a_from.size(), a_to);
    std::ofstream stream(a_path, std::ios::binary | std::ios::trunc);
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    return stream.good();
}

/// @brief 実CueEditorToolをVersion付き引数で有限Frame起動して正常終了を待つ
[[nodiscard]] bool run_editor_process(const std::filesystem::path &a_editorExecutable,
                                      const std::filesystem::path &a_projectPath,
                                      std::optional<std::string_view> a_processTestAction = std::nullopt)
{
    const std::filesystem::path descriptorPath = a_projectPath / L"CueProject.json";
    std::wstring commandLine = L"\"" + a_editorExecutable.native() + L"\" --protocol-version " +
                               std::to_wstring(cue::k_editorLaunchProtocolVersion) + L" --project-descriptor \"" +
                               descriptorPath.native() +
                               L"\" --expected-project-id 00000000-0000-4000-8000-000000000901" +
                               L" --engine-compatibility-id \"cue-engine:[1.0.0,2.0.0)\"" +
                               L" --initial-scene Scenes/Main.cuescene --maximum-frame-count 1";
    if (a_processTestAction.has_value())
    {
        if (*a_processTestAction == "autosave-recovery")
        {
            commandLine.append(L" --process-test-action autosave-recovery");
        }
        else if (*a_processTestAction == "autosave-new-scene")
        {
            commandLine.append(L" --process-test-action autosave-new-scene");
        }
        else if (*a_processTestAction == "files-workflow")
        {
            commandLine.append(L" --process-test-action files-workflow");
        }
        else if (*a_processTestAction == "play-repeated-workflow")
        {
            commandLine.append(L" --process-test-action play-repeated-workflow");
        }
        else if (*a_processTestAction == "build-workflow-debug")
        {
            commandLine.append(L" --process-test-action build-workflow-debug");
        }
        else if (*a_processTestAction == "build-workflow-development")
        {
            commandLine.append(L" --process-test-action build-workflow-development");
        }
        else if (*a_processTestAction == "build-workflow-release")
        {
            commandLine.append(L" --process-test-action build-workflow-release");
        }
        else if (*a_processTestAction == "package-workflow-debug")
        {
            commandLine.append(L" --process-test-action package-workflow-debug");
        }
        else if (*a_processTestAction == "package-workflow-development")
        {
            commandLine.append(L" --process-test-action package-workflow-development");
        }
        else if (*a_processTestAction == "package-workflow-release")
        {
            commandLine.append(L" --process-test-action package-workflow-release");
        }
        else if (*a_processTestAction == k_shippingWorkflowAction)
        {
            commandLine.append(L" --process-test-action shipping-workflow-release");
        }
        else
        {
            commandLine.append(L" --process-test-action edit-close-save");
        }
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (CreateProcessW(a_editorExecutable.c_str(), commandLine.data(), nullptr, nullptr, FALSE, 0U, nullptr, nullptr,
                       &startup, &process) == FALSE)
    {
        return false;
    }
    CloseHandle(process.hThread);
    const DWORD timeout = a_processTestAction.has_value() && (a_processTestAction->starts_with("build-workflow-") ||
                                                              a_processTestAction->starts_with("package-workflow-") ||
                                                              *a_processTestAction == k_shippingWorkflowAction)
                              ? 600000U
                              : 30000U;
    const DWORD wait = WaitForSingleObject(process.hProcess, timeout);
    DWORD exitCode = 1U;
    const bool completed = wait == WAIT_OBJECT_0 && GetExitCodeProcess(process.hProcess, &exitCode) != FALSE;
    if (!completed)
    {
        TerminateProcess(process.hProcess, 20U);
        static_cast<void>(WaitForSingleObject(process.hProcess, 5000U));
    }
    CloseHandle(process.hProcess);
    return completed && exitCode == 0U;
}

/// @brief 公開済みPackage Directoryを安定順で列挙する
[[nodiscard]] std::vector<std::filesystem::path> list_package_directories(
    const std::filesystem::path &a_projectPath)
{
    const std::filesystem::path parent =
        a_projectPath / L"Generated" / L"Packages" / std::filesystem::path(k_packageConfiguration);
    std::vector<std::filesystem::path> packages;
    for (const std::filesystem::directory_entry &entry : std::filesystem::directory_iterator(parent))
    {
        if (entry.is_directory())
        {
            packages.push_back(entry.path());
        }
    }
    std::ranges::sort(packages);
    return packages;
}

/// @brief Release Shipping Package Directoryを安定順で列挙する
[[nodiscard]] std::vector<std::filesystem::path> list_shipping_package_directories(
    const std::filesystem::path &a_projectPath)
{
    const std::filesystem::path parent = a_projectPath / L"Generated" / L"Packages" / L"Shipping" / L"Release";
    std::vector<std::filesystem::path> packages;
    for (const std::filesystem::directory_entry &entry : std::filesystem::directory_iterator(parent))
    {
        if (entry.is_directory())
        {
            packages.push_back(entry.path());
        }
    }
    std::ranges::sort(packages);
    return packages;
}

/// @brief Package Tree Entryの種類、相対Path、完全Byte列を保持する
struct PackageTreeEntry final
{
    char kind = 'O';
    std::string relativePath;
    std::string bytes;

    /// @brief Package Treeの種類、Path、完全Byte列を比較する
    bool operator==(const PackageTreeEntry &) const = default;
};

/// @brief Package Treeを相対Path、種類、完全Byte列の安定Snapshotへ変換する
[[nodiscard]] std::vector<PackageTreeEntry> capture_package_tree(const std::filesystem::path &a_packageRoot)
{
    std::vector<PackageTreeEntry> entries;
    for (const std::filesystem::directory_entry &entry : std::filesystem::recursive_directory_iterator(a_packageRoot))
    {
        const std::string relative = entry.path().lexically_relative(a_packageRoot).generic_string();
        if (entry.is_regular_file())
        {
            entries.push_back({'F', relative, read_file(entry.path())});
        }
        else if (entry.is_directory())
        {
            entries.push_back({'D', relative, {}});
        }
        else
        {
            entries.push_back({'O', relative, {}});
        }
    }
    std::ranges::sort(entries, {}, &PackageTreeEntry::relativePath);
    return entries;
}

/// @brief ASCII Byte列をWindows Path照合用のlowercaseへ変換する
[[nodiscard]] std::string ascii_lowercase(std::string a_value)
{
    std::ranges::transform(a_value, a_value.begin(), [](unsigned char a_character) noexcept
                           { return static_cast<char>(a_character >= 'A' && a_character <= 'Z'
                                                          ? a_character - 'A' + 'a'
                                                          : a_character); });
    return a_value;
}

/// @brief Windows UTF-16 PathをBinary検索用のlittle-endian Byte列へ変換する
[[nodiscard]] std::string utf16_bytes(std::wstring_view a_value)
{
    static_assert(sizeof(wchar_t) == 2U);
    return {reinterpret_cast<const char *>(a_value.data()), a_value.size() * sizeof(wchar_t)};
}

/// @brief Package内Fileが指定Windows PathをNativeまたはGeneric表現で参照するか返す
[[nodiscard]] bool package_contains_path_reference(const std::filesystem::path &a_packageRoot,
                                                   const std::filesystem::path &a_reference,
                                                   cue::FatalHandler &a_handler)
{
    const std::u8string genericReference = a_reference.generic_u8string();
    const std::wstring nativeReference = a_reference.native();
    const std::wstring extendedReference = L"\\\\?\\" + nativeReference;
    const std::array references = {
        ascii_lowercase(to_utf8(a_reference, a_handler)),
        ascii_lowercase(std::string(reinterpret_cast<const char *>(genericReference.data()), genericReference.size())),
        ascii_lowercase(utf16_bytes(nativeReference)), ascii_lowercase(utf16_bytes(extendedReference))};
    for (const std::filesystem::directory_entry &entry :
         std::filesystem::recursive_directory_iterator(a_packageRoot))
    {
        if (!entry.is_regular_file())
        {
            continue;
        }
        const std::string bytes = ascii_lowercase(read_file(entry.path()));
        if (std::ranges::any_of(references,
                                [&bytes](const std::string &a_referenceBytes)
                                {
                                    return !a_referenceBytes.empty() &&
                                           bytes.find(a_referenceBytes) != std::string::npos;
                                }))
        {
            return true;
        }
    }
    return false;
}

/// @brief Shipping Packageが実行に必要な四File以外を含まないか返す
[[nodiscard]] bool has_minimal_shipping_inventory(const std::filesystem::path &a_packageRoot,
                                                  const std::filesystem::path &a_runtimeScene)
{
    std::vector<std::string> files;
    std::vector<std::string> directories;
    for (const std::filesystem::directory_entry &entry : std::filesystem::recursive_directory_iterator(a_packageRoot))
    {
        const std::string relative = entry.path().lexically_relative(a_packageRoot).generic_string();
        if (entry.is_regular_file())
        {
            files.push_back(relative);
        }
        else if (entry.is_directory())
        {
            directories.push_back(relative);
        }
        else
        {
            return false;
        }
    }
    std::ranges::sort(files);
    std::ranges::sort(directories);
    std::vector<std::string> expectedFiles{"CueGameProduct.exe", "CuePackage.json", "Data/CueProject.runtime.json",
                                           a_runtimeScene.generic_string()};
    std::ranges::sort(expectedFiles);
    const std::vector<std::string> expectedDirectories{"Data", "Data/Scenes"};
    return files == expectedFiles && directories == expectedDirectories;
}

/// @brief JSON TextにDriveまたはslash／backslash形式のWindows Rooted Pathが含まれるか返す
[[nodiscard]] bool json_text_contains_windows_absolute_path(std::string_view a_bytes) noexcept
{
    if (a_bytes.find("\\\\") != std::string_view::npos ||
        a_bytes.find("\"/") != std::string_view::npos)
    {
        return true;
    }
    for (std::size_t index = 0U; index + 2U < a_bytes.size(); ++index)
    {
        const char drive = a_bytes[index];
        const bool startsJsonString = index > 0U && a_bytes[index - 1U] == '"';
        if (startsJsonString && ((drive >= 'A' && drive <= 'Z') || (drive >= 'a' && drive <= 'z')) &&
            a_bytes[index + 1U] == ':' && (a_bytes[index + 2U] == '/' || a_bytes[index + 2U] == '\\'))
        {
            return true;
        }
    }
    return false;
}

/// @brief PackageのJSON DataにWindows Absolute Path表現が含まれるか返す
[[nodiscard]] bool package_json_contains_absolute_path(const std::filesystem::path &a_packageRoot)
{
    for (const std::filesystem::directory_entry &entry :
         std::filesystem::recursive_directory_iterator(a_packageRoot))
    {
        if (!entry.is_regular_file() || entry.path().extension() != L".json")
        {
            continue;
        }
        const std::string bytes = read_file(entry.path());
        if (json_text_contains_windows_absolute_path(bytes))
        {
            return true;
        }
    }
    return false;
}

/// @brief Project Treeに未回収のPackage／Workspace Staging Entryが残っているか返す
[[nodiscard]] bool has_staging_entry(const std::filesystem::path &a_projectRoot)
{
    for (const std::filesystem::directory_entry &entry :
         std::filesystem::recursive_directory_iterator(a_projectRoot))
    {
        const std::wstring name = entry.path().filename().native();
        if (name.starts_with(L"CueStaging-") || name.ends_with(L".cuedir-staging") ||
            name.ends_with(L".cuefile-staging") || name.ends_with(L".cuefile-replace-staging") ||
            name.ends_with(L"-dir-staging") || name.ends_with(L"-file-staging"))
        {
            return true;
        }
    }
    return false;
}

/// @brief Relocated Packageの指定Executableを無関係なCurrent DirectoryからSmoke起動する
[[nodiscard]] std::optional<DWORD> run_relocated_package(const std::filesystem::path &a_packageRoot,
                                                         const std::filesystem::path &a_workingDirectory,
                                                         std::wstring_view a_executableName)
{
    const std::filesystem::path executable = a_packageRoot / a_executableName;
    std::wstring commandLine = L"\"" + executable.native() + L"\" --package-smoke-test";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (CreateProcessW(executable.c_str(), commandLine.data(), nullptr, nullptr, FALSE, 0U, nullptr,
                       a_workingDirectory.c_str(), &startup, &process) == FALSE)
    {
        return std::nullopt;
    }
    CloseHandle(process.hThread);
    const DWORD wait = WaitForSingleObject(process.hProcess, 30000U);
    DWORD exitCode = 1U;
    const bool completed = wait == WAIT_OBJECT_0 && GetExitCodeProcess(process.hProcess, &exitCode) != FALSE;
    if (!completed)
    {
        TerminateProcess(process.hProcess, 20U);
        static_cast<void>(WaitForSingleObject(process.hProcess, 5000U));
    }
    CloseHandle(process.hProcess);
    return completed ? std::optional<DWORD>(exitCode) : std::nullopt;
}

/// @brief Project生成からScene保存、実Editor再起動、Stable ID再Openまでを検証する
void test_process_round_trip(const std::filesystem::path &a_editorExecutable,
                             const std::filesystem::path &a_workspaceRoot, const cue::AssertContext &a_context)
{
    TestDirectory directory(a_workspaceRoot);
    auto parent = cue::create_windows_filesystem_root(to_utf8(directory.path(), a_context.fatal_handler()), a_context);
    auto projectId = cue::ProjectId::parse("00000000-0000-4000-8000-000000000901", a_context);
    const cue::EngineCompatibility engineCompatibility{cue::EngineVersion{1U, 0U, 0U}, cue::EngineVersion{2U, 0U, 0U}};
    if (!parent || !projectId)
    {
        std::_Exit(4);
    }
    auto generated =
        cue::generate_blank_project(**parent.try_value(), "Project", "Workflow Project", *projectId.try_value(),
                                    "00000000-0000-4000-8000-000000000099", {engineCompatibility}, a_context);
    if (!generated)
    {
        std::_Exit(5);
    }

    const std::filesystem::path projectPath = directory.path() / L"Project";
    auto session = cue::editor::WindowsEditorSession::create(
        make_parameters(projectPath, projectId.try_value()->text(), engineCompatibility, a_context),
        make_configuration(a_context), a_context);
    if (!session || (*session.try_value())->active_document_id().has_value())
    {
        std::_Exit(6);
    }
    auto sceneLocator = cue::RelativePath::parse("Scenes/Main.cuescene", a_context);
    if (!sceneLocator)
    {
        std::_Exit(7);
    }
    auto documentId = (*session.try_value())->create_scene(std::move(*sceneLocator.try_value()));
    if (!documentId)
    {
        std::_Exit(8);
    }
    cue::editor_core::AddObjectIntent addObject{std::nullopt, "Persistent Root"};
    auto edited = (*session.try_value())
                      ->controller()
                      .execute_intent(*documentId.try_value(), std::move(addObject),
                                      (*session.try_value())->identity_source(), {});
    if (!edited)
    {
        std::_Exit(9);
    }
    const cue::editor_core::EditorDocument *document =
        (*session.try_value())->controller().session().find_document(*documentId.try_value());
    if (document == nullptr || document->scene_document().object_count() != 1U)
    {
        std::_Exit(10);
    }
    const auto sceneText = document->scene_document().scene_asset_id().canonical_text();
    const auto objectText = document->scene_document().objects().front().id().canonical_text();
    auto saved = (*session.try_value())->save_active_scene();
    if (!saved || saved.try_value()->status() != cue::scene::SceneSaveStatus::Committed)
    {
        std::_Exit(11);
    }
    const std::filesystem::path sourceAssetsPath = projectPath / L"Assets" / L"Source";
    const std::filesystem::path savedScenePath = sourceAssetsPath / L"Scenes" / L"Main.cuescene";
    const std::string savedSceneBytes = read_file(savedScenePath);
    if (!replace_file_text(projectPath / L"CueProject.json", "00000000-0000-4000-8000-000000000099",
                           std::string_view(sceneText.data(), sceneText.size())) ||
        !replace_file_text(projectPath / L"CueProject.json", "Scenes/Default.cuescene", "Scenes/Main.cuescene"))
    {
        std::_Exit(54);
    }
    auto existingLocator = cue::RelativePath::parse("Scenes/Main.cuescene", a_context);
    auto existingCreate = (*session.try_value())->prepare_new_scene(std::move(*existingLocator.try_value()));
    if (existingCreate || read_file(savedScenePath) != savedSceneBytes ||
        (*session.try_value())->active_document_id() != std::optional(*documentId.try_value()))
    {
        std::_Exit(20);
    }
    auto missingLocator = cue::RelativePath::parse("Scenes/Missing.cuescene", a_context);
    auto missingOpen = (*session.try_value())->prepare_open_scene(std::move(*missingLocator.try_value()));
    if (missingOpen || (*session.try_value())->active_document_id() != std::optional(*documentId.try_value()) ||
        (*session.try_value())->controller().session().documents().size() != 1U)
    {
        std::_Exit(21);
    }
    cue::editor_core::RenameObjectIntent dirtyBeforeSwitch{document->scene_document().objects().front().id(),
                                                           "Dirty Before Switch"};
    auto dirtied = (*session.try_value())
                       ->controller()
                       .execute_intent(*documentId.try_value(), std::move(dirtyBeforeSwitch),
                                       (*session.try_value())->identity_source(), {});
    auto newLocator = cue::RelativePath::parse("Scenes/New.cuescene", a_context);
    auto prepared = (*session.try_value())->prepare_new_scene(std::move(*newLocator.try_value()));
    auto awaitingSwitch = (*session.try_value())->request_activate_prepared_scene();
    if (!dirtied || !prepared || !awaitingSwitch ||
        *awaitingSwitch.try_value() != cue::editor_core::DocumentCloseState::AwaitingDecision)
    {
        std::_Exit(22);
    }
    auto cancelledSwitch = (*session.try_value())->respond_to_close(cue::editor_core::CloseDecision::Cancel);
    auto discardedPrepared = (*session.try_value())->discard_prepared_scene();
    document = (*session.try_value())->controller().session().find_document(*documentId.try_value());
    if (!cancelledSwitch || *cancelledSwitch.try_value() != cue::editor_core::DocumentCloseState::Open ||
        !discardedPrepared || (*session.try_value())->has_prepared_scene() || document == nullptr ||
        document->scene_document().objects().front().name() != "Dirty Before Switch" ||
        (*session.try_value())->controller().session().documents().size() != 1U)
    {
        std::_Exit(23);
    }
    newLocator = cue::RelativePath::parse("Scenes/New.cuescene", a_context);
    auto discardCandidate = (*session.try_value())->prepare_new_scene(std::move(*newLocator.try_value()));
    awaitingSwitch = (*session.try_value())->request_activate_prepared_scene();
    auto discardedActive = (*session.try_value())->respond_to_close(cue::editor_core::CloseDecision::Discard);
    if (!discardCandidate || !awaitingSwitch || !discardedActive ||
        *discardedActive.try_value() != cue::editor_core::DocumentCloseState::Closed ||
        (*session.try_value())->has_prepared_scene() ||
        (*session.try_value())->active_document_id() != std::optional(*discardCandidate.try_value()))
    {
        std::_Exit(25);
    }
    auto candidateClose = (*session.try_value())->request_close();
    auto candidateDiscard = (*session.try_value())->respond_to_close(cue::editor_core::CloseDecision::Discard);
    auto originalLocator = cue::RelativePath::parse("Scenes/Main.cuescene", a_context);
    auto originalReopened = (*session.try_value())->open_scene(std::move(*originalLocator.try_value()));
    if (!candidateClose || !candidateDiscard || !originalReopened)
    {
        std::_Exit(26);
    }
    auto cleanLocator = cue::RelativePath::parse("Scenes/CleanSwitch.cuescene", a_context);
    auto cleanCandidate = (*session.try_value())->prepare_new_scene(std::move(*cleanLocator.try_value()));
    auto cleanSwitch = (*session.try_value())->request_activate_prepared_scene();
    if (!cleanCandidate || !cleanSwitch || *cleanSwitch.try_value() != cue::editor_core::DocumentCloseState::Closed ||
        (*session.try_value())->active_document_id() != std::optional(*cleanCandidate.try_value()))
    {
        std::_Exit(27);
    }
    candidateClose = (*session.try_value())->request_close();
    candidateDiscard = (*session.try_value())->respond_to_close(cue::editor_core::CloseDecision::Discard);
    originalLocator = cue::RelativePath::parse("Scenes/Main.cuescene", a_context);
    originalReopened = (*session.try_value())->open_scene(std::move(*originalLocator.try_value()));
    if (!candidateClose || !candidateDiscard || !originalReopened)
    {
        std::_Exit(28);
    }
    session.try_value()->reset();

    if (!run_editor_process(a_editorExecutable, projectPath, "autosave-new-scene"))
    {
        std::_Exit(45);
    }
    auto cleanRecoverySession = cue::editor::WindowsEditorSession::create(
        make_parameters(projectPath, projectId.try_value()->text(), engineCompatibility, a_context),
        make_configuration(a_context), a_context);
    if (!cleanRecoverySession)
    {
        std::_Exit(46);
    }
    auto cleanRecoveryCandidates = (*cleanRecoverySession.try_value())->list_recovery_candidates();
    const cue::editor_core::RecoveryCandidateInspection *cleanRecoveryCandidate = nullptr;
    if (cleanRecoveryCandidates)
    {
        for (const cue::editor_core::RecoveryCandidateInspection &candidate : *cleanRecoveryCandidates.try_value())
        {
            if (candidate.try_metadata() != nullptr &&
                candidate.try_metadata()->source_locator().text() == "Scenes/Child-Unedited.cuescene")
            {
                cleanRecoveryCandidate = &candidate;
                break;
            }
        }
    }
    if (cleanRecoveryCandidate == nullptr)
    {
        std::_Exit(47);
    }
    auto cleanRecoveryDocumentId =
        (*cleanRecoverySession.try_value())->open_recovery_scene(cleanRecoveryCandidate->scene_id());
    const cue::editor_core::EditorDocument *cleanRecoveryDocument =
        cleanRecoveryDocumentId ? (*cleanRecoverySession.try_value())
                                      ->controller()
                                      .session()
                                      .find_document(*cleanRecoveryDocumentId.try_value())
                                : nullptr;
    if (!cleanRecoveryDocumentId || cleanRecoveryDocument == nullptr ||
        cleanRecoveryDocument->scene_locator().text() != "Scenes/Child-Unedited.cuescene" ||
        cleanRecoveryDocument->scene_document().object_count() != 0U || cleanRecoveryDocument->has_saved_destination())
    {
        std::_Exit(48);
    }
    cleanRecoverySession.try_value()->reset();

    if (!run_editor_process(a_editorExecutable, projectPath, "autosave-recovery"))
    {
        std::_Exit(29);
    }
    auto recoverySession = cue::editor::WindowsEditorSession::create(
        make_parameters(projectPath, projectId.try_value()->text(), engineCompatibility, a_context),
        make_configuration(a_context), a_context);
    if (!recoverySession)
    {
        std::_Exit(30);
    }
    auto recoveryCandidates = (*recoverySession.try_value())->list_recovery_candidates();
    if (!recoveryCandidates || recoveryCandidates.try_value()->empty())
    {
        std::_Exit(31);
    }
    auto recoveryDocumentId =
        (*recoverySession.try_value())->open_recovery_scene(std::string_view(sceneText.data(), sceneText.size()));
    const cue::editor_core::EditorDocument *recoveryDocument =
        recoveryDocumentId
            ? (*recoverySession.try_value())->controller().session().find_document(*recoveryDocumentId.try_value())
            : nullptr;
    bool foundRecoveryChild = false;
    if (recoveryDocument != nullptr)
    {
        for (const cue::scene::SceneObject &object : recoveryDocument->scene_document().objects())
        {
            foundRecoveryChild = foundRecoveryChild || object.name() == "Child Process Recovery";
        }
    }
    if (!recoveryDocumentId || recoveryDocument == nullptr || !foundRecoveryChild)
    {
        std::_Exit(32);
    }
    recoverySession.try_value()->reset();

    if (!run_editor_process(a_editorExecutable, projectPath, "edit-close-save"))
    {
        std::_Exit(33);
    }
    if (!run_editor_process(a_editorExecutable, projectPath, "files-workflow") ||
        !std::filesystem::is_directory(sourceAssetsPath / L"FilesWorkflow") ||
        !std::filesystem::is_regular_file(sourceAssetsPath / L"FilesWorkflow" / L"Moved.txt") ||
        !std::filesystem::is_regular_file(sourceAssetsPath / L"Copy.txt"))
    {
        std::_Exit(49);
    }
    const std::string childSavedSceneBytes = read_file(savedScenePath);
    if (!run_editor_process(a_editorExecutable, projectPath))
    {
        std::_Exit(24);
    }
    if (read_file(savedScenePath) != childSavedSceneBytes)
    {
        std::_Exit(34);
    }

    auto reopened = cue::editor::WindowsEditorSession::create(
        make_parameters(projectPath, projectId.try_value()->text(), engineCompatibility, a_context,
                        std::string("Scenes/Main.cuescene"), std::string(sceneText.data(), sceneText.size())),
        make_configuration(a_context), a_context);
    if (!reopened || !(*reopened.try_value())->active_document_id().has_value())
    {
        std::_Exit(12);
    }
    const cue::editor_core::EditorDocument *reopenedDocument =
        (*reopened.try_value())->controller().session().find_document(*(*reopened.try_value())->active_document_id());
    bool foundSavedChild = false;
    bool foundOriginalObject = false;
    if (reopenedDocument != nullptr)
    {
        for (const cue::scene::SceneObject &object : reopenedDocument->scene_document().objects())
        {
            foundSavedChild = foundSavedChild || object.name() == "Child Process Saved";
            foundOriginalObject = foundOriginalObject ||
                                  (object.name() == "Persistent Root" && object.id().canonical_text() == objectText);
        }
    }
    if (reopenedDocument == nullptr)
    {
        std::_Exit(39);
    }
    if (reopenedDocument->scene_document().object_count() != 2U)
    {
        std::_Exit(40);
    }
    if (!foundSavedChild)
    {
        std::_Exit(41);
    }
    if (reopenedDocument->scene_document().scene_asset_id().canonical_text() != sceneText)
    {
        std::_Exit(42);
    }
    if (!foundOriginalObject)
    {
        std::_Exit(43);
    }
    if ((*reopened.try_value())->controller().session().project_descriptor().project_id() != *projectId.try_value())
    {
        std::_Exit(44);
    }
    cue::editor_core::RenameObjectIntent renameObject{reopenedDocument->scene_document().objects().front().id(),
                                                      "Dirty Root"};
    auto renamed = (*reopened.try_value())
                       ->controller()
                       .execute_intent(*(*reopened.try_value())->active_document_id(), std::move(renameObject),
                                       (*reopened.try_value())->identity_source(), {});
    auto saveAsLocator = cue::RelativePath::parse("Scenes/ConflictCopy.cuescene", a_context);
    if (!saveAsLocator)
    {
        std::_Exit(36);
    }
    auto savedAs = (*reopened.try_value())->save_active_scene_as_new(std::move(*saveAsLocator.try_value()));
    if (!renamed || !savedAs || savedAs.try_value()->status() != cue::scene::SceneSaveStatus::Committed ||
        read_file(savedScenePath) != childSavedSceneBytes)
    {
        std::_Exit(36);
    }
    reopenedDocument =
        (*reopened.try_value())->controller().session().find_document(*(*reopened.try_value())->active_document_id());
    if (reopenedDocument == nullptr)
    {
        std::_Exit(37);
    }
    cue::editor_core::RenameObjectIntent dirtyCopy{reopenedDocument->scene_document().objects().front().id(),
                                                   "Dirty Copy"};
    auto copiedRename = (*reopened.try_value())
                            ->controller()
                            .execute_intent(*(*reopened.try_value())->active_document_id(), std::move(dirtyCopy),
                                            (*reopened.try_value())->identity_source(), {});
    auto awaiting = (*reopened.try_value())->request_close();
    if (!copiedRename || !awaiting || *awaiting.try_value() != cue::editor_core::DocumentCloseState::AwaitingDecision)
    {
        std::_Exit(14);
    }
    auto cancelled = (*reopened.try_value())->respond_to_close(cue::editor_core::CloseDecision::Cancel);
    if (!cancelled || *cancelled.try_value() != cue::editor_core::DocumentCloseState::Open ||
        !(*reopened.try_value())->active_document_id().has_value())
    {
        std::_Exit(15);
    }
    awaiting = (*reopened.try_value())->request_close();
    auto discarded = (*reopened.try_value())->respond_to_close(cue::editor_core::CloseDecision::Discard);
    if (!awaiting || *awaiting.try_value() != cue::editor_core::DocumentCloseState::AwaitingDecision || !discarded ||
        *discarded.try_value() != cue::editor_core::DocumentCloseState::Closed ||
        (*reopened.try_value())->active_document_id().has_value())
    {
        std::_Exit(16);
    }

    auto wrongIdentity = cue::editor::WindowsEditorSession::create(
        make_parameters(projectPath, "00000000-0000-4000-8000-000000000902", engineCompatibility, a_context),
        make_configuration(a_context), a_context);
    if (wrongIdentity)
    {
        std::_Exit(17);
    }
    auto wrongCompatibilityParameters =
        make_parameters(projectPath, projectId.try_value()->text(), engineCompatibility, a_context);
    wrongCompatibilityParameters.engineCompatibilityId = "cue-engine:[9.0.0,)";
    auto wrongCompatibility = cue::editor::WindowsEditorSession::create(std::move(wrongCompatibilityParameters),
                                                                        make_configuration(a_context), a_context);
    if (wrongCompatibility)
    {
        std::_Exit(18);
    }
    auto invalidScene = cue::editor::WindowsEditorSession::create(
        make_parameters(projectPath, projectId.try_value()->text(), engineCompatibility, a_context,
                        std::string("../Outside.cuescene")),
        make_configuration(a_context), a_context);
    if (invalidScene)
    {
        std::_Exit(19);
    }
    auto wrongSceneIdentity = cue::editor::WindowsEditorSession::create(
        make_parameters(projectPath, projectId.try_value()->text(), engineCompatibility, a_context,
                        std::string("Scenes/Main.cuescene"), std::string("00000000-0000-4000-8000-000000000999")),
        make_configuration(a_context), a_context);
    if (wrongSceneIdentity)
    {
        std::_Exit(52);
    }
    reopened.try_value()->reset();

    if (!run_editor_process(a_editorExecutable, projectPath, "play-repeated-workflow") ||
        read_file(savedScenePath) != childSavedSceneBytes)
    {
        std::_Exit(50);
    }
    if (!run_editor_process(a_editorExecutable, projectPath, k_buildWorkflowAction))
    {
        std::_Exit(51);
    }
    if (!run_editor_process(a_editorExecutable, projectPath, k_packageWorkflowAction))
    {
        std::_Exit(53);
    }
    if (!run_editor_process(a_editorExecutable, projectPath, k_packageWorkflowAction))
    {
        std::_Exit(55);
    }
#if CUE_TEST_BUILD_CONFIGURATION == 3
    if (!run_editor_process(a_editorExecutable, projectPath, k_shippingWorkflowAction))
    {
        std::_Exit(61);
    }
#endif

    const std::vector<std::filesystem::path> packages = list_package_directories(projectPath);
    if (packages.size() != 2U)
    {
        std::_Exit(56);
    }
    const std::string firstManifest = read_file(packages[0] / L"CuePackage.json");
    const std::string secondManifest = read_file(packages[1] / L"CuePackage.json");
    if (!json_text_contains_windows_absolute_path("{\"path\":\"//server/share/runtime.json\"}") ||
        !json_text_contains_windows_absolute_path("{\"path\":\"/Generated/Build/runtime.json\"}") ||
        !json_text_contains_windows_absolute_path("{\"path\":\"C:/workspace/runtime.json\"}") ||
        json_text_contains_windows_absolute_path("{\"url\":\"https://example.invalid/runtime\"}"))
    {
        std::_Exit(60);
    }
    std::filesystem::path runtimeSceneName(std::string(sceneText.data(), sceneText.size()));
    runtimeSceneName += L".cueruntime.json";
    const std::filesystem::path runtimeProject = L"Data/CueProject.runtime.json";
    const std::filesystem::path runtimeScene = std::filesystem::path(L"Data/Scenes") / runtimeSceneName;
    if (firstManifest.empty() || secondManifest.empty() ||
        read_file(packages[0] / runtimeProject) != read_file(packages[1] / runtimeProject) ||
        read_file(packages[0] / runtimeScene) != read_file(packages[1] / runtimeScene) ||
        package_json_contains_absolute_path(packages[0]) || package_json_contains_absolute_path(packages[1]))
    {
        std::_Exit(57);
    }

    const std::filesystem::path relocatedPackage = directory.path() / L"RelocatedPackage";
    std::filesystem::copy(packages[1], relocatedPackage, std::filesystem::copy_options::recursive);
    if (read_file(relocatedPackage / L"CuePackage.json") != secondManifest ||
        package_json_contains_absolute_path(relocatedPackage))
    {
        std::_Exit(58);
    }

#if CUE_TEST_BUILD_CONFIGURATION == 3
    const std::vector<std::filesystem::path> shippingPackages = list_shipping_package_directories(projectPath);
    const std::filesystem::path engineBuildRoot =
        std::filesystem::absolute(a_editorExecutable).parent_path().parent_path().parent_path();
    if (shippingPackages.size() != 1U || !has_minimal_shipping_inventory(shippingPackages.front(), runtimeScene) ||
        package_json_contains_absolute_path(shippingPackages.front()) ||
        package_contains_path_reference(shippingPackages.front(), engineBuildRoot, a_context.fatal_handler()))
    {
        std::_Exit(62);
    }
    const std::filesystem::path relocatedShippingPackage = directory.path() / L"RelocatedShippingPackage";
    std::filesystem::copy(shippingPackages.front(), relocatedShippingPackage,
                          std::filesystem::copy_options::recursive);
    const std::vector<PackageTreeEntry> shippingTreeBefore = capture_package_tree(relocatedShippingPackage);

    /// @brief Shipping Packageを独立したTamper Caseへ複製する
    const auto copy_shipping_package = [&directory, &relocatedShippingPackage](std::wstring_view a_name)
    {
        const std::filesystem::path destination = directory.path() / a_name;
        std::filesystem::copy(relocatedShippingPackage, destination, std::filesystem::copy_options::recursive);
        return destination;
    };
    const std::filesystem::path tamperedExecutable = copy_shipping_package(L"TamperedExecutable");
    const std::filesystem::path mismatchedProject = copy_shipping_package(L"MismatchedProject");
    const std::filesystem::path mismatchedConfiguration = copy_shipping_package(L"MismatchedConfiguration");
    const std::filesystem::path mismatchedArchitecture = copy_shipping_package(L"MismatchedArchitecture");
    const std::filesystem::path mismatchedRole = copy_shipping_package(L"MismatchedRole");
    const std::filesystem::path tamperedRuntimeData = copy_shipping_package(L"TamperedRuntimeData");
    const std::filesystem::path plantedDll = copy_shipping_package(L"PlantedDll");
    {
        std::ofstream stream(tamperedExecutable / L"CueGameProduct.exe", std::ios::binary | std::ios::app);
        stream.put('\n');
    }
    if (!replace_file_text(mismatchedProject / L"CuePackage.json", "00000000-0000-4000-8000-000000000901",
                           "00000000-0000-4000-8000-000000000902") ||
        !replace_file_text(mismatchedConfiguration / L"CuePackage.json", "\"configuration\": \"Release\"",
                           "\"configuration\": \"Debug\"") ||
        !replace_file_text(mismatchedArchitecture / L"CuePackage.json", "\"architecture\": \"x64\"",
                           "\"architecture\": \"arm\"") ||
        !replace_file_text(mismatchedRole / L"CuePackage.json", "\"role\": \"applicationExecutable\"",
                           "\"role\": \"runtimeDependency\""))
    {
        std::_Exit(63);
    }
    {
        std::ofstream stream(tamperedRuntimeData / runtimeScene, std::ios::binary | std::ios::app);
        stream.put('\n');
    }
    {
        std::ofstream stream(plantedDll / L"unexpected.dll", std::ios::binary | std::ios::trunc);
        stream << "not-a-runtime-dependency";
    }
#endif

    const std::filesystem::path unavailableProject = directory.path() / L"Project.SourceUnavailable";
    std::filesystem::rename(projectPath, unavailableProject);
    const std::filesystem::path unrelatedWorkingDirectory = directory.path() / L"UnrelatedWorkingDirectory";
    std::filesystem::create_directories(unrelatedWorkingDirectory);
    const std::optional<DWORD> relocatedRun =
        run_relocated_package(relocatedPackage, unrelatedWorkingDirectory, L"CueRuntimeHost.exe");
#if CUE_TEST_BUILD_CONFIGURATION == 3
    const std::optional<DWORD> shippingRun =
        run_relocated_package(relocatedShippingPackage, unrelatedWorkingDirectory, L"CueGameProduct.exe");
    const std::vector<PackageTreeEntry> shippingTreeAfter = capture_package_tree(relocatedShippingPackage);
    const std::array tamperedPackages = {
        tamperedExecutable,  mismatchedProject, mismatchedConfiguration, mismatchedArchitecture, mismatchedRole,
        tamperedRuntimeData, plantedDll};
    bool rejectedEveryTamperedPackage = true;
    for (const std::filesystem::path &package : tamperedPackages)
    {
        const std::optional<DWORD> result =
            run_relocated_package(package, unrelatedWorkingDirectory, L"CueGameProduct.exe");
        rejectedEveryTamperedPackage = rejectedEveryTamperedPackage && result.has_value() && *result != 0U;
    }
#endif
    std::filesystem::rename(unavailableProject, projectPath);
    if (!relocatedRun.has_value() || *relocatedRun != 0U)
    {
        std::_Exit(59);
    }
#if CUE_TEST_BUILD_CONFIGURATION == 3
    if (!shippingRun.has_value() || *shippingRun != 0U || shippingTreeAfter != shippingTreeBefore ||
        !rejectedEveryTamperedPackage || has_staging_entry(projectPath))
    {
        std::_Exit(64);
    }
#endif
}
} // namespace

/// @brief Headless制作Workflowと実CueEditorTool再起動境界を検証する
int wmain(int a_argumentCount, wchar_t **a_arguments)
{
    if (a_argumentCount != 3)
    {
        return 1;
    }
    TestFatalHandler handler;
    std::vector<std::unique_ptr<cue::LogSink>> sinks;
    cue::Logger logger(handler, std::move(sinks));
    cue::AssertContext context(logger, handler);
    test_process_round_trip(a_arguments[1], a_arguments[2], context);
    return 0;
}
