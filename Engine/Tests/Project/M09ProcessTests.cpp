#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Fatal.h>
#include <Cue/Foundation/Log.h>
#include <Cue/IO/Windows/WindowsFilesystem.h>
#include <Cue/Project/Compatibility.h>
#include <Cue/Project/Error.h>
#include <Cue/Project/Generator.h>
#include <Cue/Project/Registry.h>

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
class TestFatalHandler final : public cue::FatalHandler
{
  public:
    /// @brief Test中の回復不能失敗を即座に終了Codeへ変換する
    [[noreturn]] void terminate() noexcept override
    {
        std::_Exit(90);
    }

    /// @brief Message付き回復不能失敗を即座に終了Codeへ変換する
    [[noreturn]] void terminate(std::string_view) noexcept override
    {
        std::_Exit(91);
    }
};

/// @brief Native Test PathをWindows Filesystem Factory用のUTF-8へ変換する
[[nodiscard]] std::string to_utf8(std::wstring_view a_path)
{
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, a_path.data(), static_cast<int>(a_path.size()),
                                          nullptr, 0, nullptr, nullptr);
    if (count <= 0)
    {
        return {};
    }
    std::string result(static_cast<std::size_t>(count), '\0');
    const int converted = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, a_path.data(),
                                              static_cast<int>(a_path.size()), result.data(), count, nullptr, nullptr);
    return converted == count ? result : std::string{};
}

class TestDirectory final
{
  public:
    /// @brief Processと時刻から一意な実Filesystem Test Rootを作成する
    TestDirectory()
    {
        std::array<wchar_t, MAX_PATH> temporary{};
        const DWORD length = GetTempPathW(static_cast<DWORD>(temporary.size()), temporary.data());
        if (length == 0U || length >= temporary.size())
        {
            return;
        }
        m_path = temporary.data();
        m_path +=
            L"CueM09ProcessTests-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
        m_isCreated = CreateDirectoryW(m_path.c_str(), nullptr) != FALSE;
    }

    /// @brief Test Rootの一意所有を保つためCopy構築を禁止する
    TestDirectory(const TestDirectory &) = delete;
    /// @brief Test Rootの一意所有を保つためCopy代入を禁止する
    TestDirectory &operator=(const TestDirectory &) = delete;
    /// @brief Native Pathの所有位置を固定するためMove構築を禁止する
    TestDirectory(TestDirectory &&) = delete;
    /// @brief Native Pathの所有位置を固定するためMove代入を禁止する
    TestDirectory &operator=(TestDirectory &&) = delete;

    /// @brief Test専用の一意なTemporary Rootだけを再帰Cleanupする
    ~TestDirectory()
    {
        if (!m_isCreated)
        {
            return;
        }
        std::error_code error;
        std::filesystem::remove_all(std::filesystem::path(m_path), error);
    }

    /// @brief Root作成に成功して安全なCleanup対象を所有するか返す
    [[nodiscard]] bool is_created() const noexcept
    {
        return m_isCreated;
    }

    /// @brief Root配下のNative Pathを存在確認と移動操作へ返す
    [[nodiscard]] std::wstring child(std::wstring_view a_relative) const
    {
        return m_path + L"\\" + std::wstring(a_relative);
    }

    /// @brief Windows Filesystem Factoryへ渡すRootのUTF-8 Absolute Pathを返す
    [[nodiscard]] std::string utf8_path() const
    {
        return to_utf8(m_path);
    }

    /// @brief Test RootのNative Absolute PathをFilesystem検証へ返す
    [[nodiscard]] const std::wstring &native_path() const noexcept
    {
        return m_path;
    }

    /// @brief 指定ChildのUTF-8 Absolute LocatorをRecent Registryへ返す
    [[nodiscard]] std::string child_utf8(std::wstring_view a_relative) const
    {
        return to_utf8(child(a_relative));
    }

  private:
    std::wstring m_path;
    bool m_isCreated = false;
};

/// @brief Native DirectoryがReparse Pointでない通常Directoryとして存在するか判定する
[[nodiscard]] bool is_directory(const std::wstring &a_path) noexcept
{
    const DWORD attributes = GetFileAttributesW(a_path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0U &&
           (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0U;
}

/// @brief Native FileがReparse Pointでない通常Fileとして存在するか判定する
[[nodiscard]] bool is_file(const std::wstring &a_path) noexcept
{
    const DWORD attributes = GetFileAttributesW(a_path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0U &&
           (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0U;
}

/// @brief ResultのProject Error分類が期待値と一致するか判定する
template <typename Value>
[[nodiscard]] bool has_project_error(const cue::Result<Value> &a_result, cue::ProjectError a_error) noexcept
{
    return !a_result && a_result.try_error()->code().domain() == "Cue.Project" &&
           a_result.try_error()->code().value() == static_cast<std::int64_t>(a_error);
}

/// @brief Test Root直下にProjectとWorkspace以外の書込みが残っていないか検証する
[[nodiscard]] bool has_expected_top_level(const TestDirectory &a_directory)
{
    std::error_code error;
    std::vector<std::wstring> names;
    for (const std::filesystem::directory_entry &entry :
         std::filesystem::directory_iterator(std::filesystem::path(a_directory.native_path()), error))
    {
        names.push_back(entry.path().filename().wstring());
    }
    if (error)
    {
        return false;
    }
    std::ranges::sort(names);
    constexpr std::array expected = {std::wstring_view(L"MovedProject"), std::wstring_view(L"Workspace")};
    return names.size() == expected.size() && names[0] == expected[0] && names[1] == expected[1];
}

/// @brief Createから移動・再関連付け・一覧除外までを実Filesystem上の一つのProcessで検証する
[[nodiscard]] bool test_m09_project_process(const cue::AssertContext &a_assertContext)
{
    TestDirectory directory;
    if (!directory.is_created() || CreateDirectoryW(directory.child(L"Workspace").c_str(), nullptr) == FALSE)
    {
        return false;
    }

    auto parent = cue::create_windows_filesystem_root(directory.utf8_path(), a_assertContext);
    auto projectId = cue::ProjectId::parse("12345678-1234-4abc-8def-1234567890ab", a_assertContext);
    if (!parent || !projectId)
    {
        return false;
    }
    const cue::BlankProjectTemplate projectTemplate = {
        cue::EngineCompatibility{cue::EngineVersion{0U, 1U, 0U}, cue::EngineVersion{1U, 0U, 0U}},
    };
    auto generated = cue::generate_blank_project(**parent.try_value(), "SampleProject", "M09 Process Project",
                                                 *projectId.try_value(), "00000000-0000-4000-8000-000000000099",
                                                 projectTemplate, a_assertContext);
    if (!generated || !is_file(directory.child(L"SampleProject\\CueProject.json")))
    {
        return false;
    }

    auto projectFilesystem =
        cue::create_windows_filesystem_root(directory.child_utf8(L"SampleProject"), a_assertContext);
    auto opened = projectFilesystem ? cue::load_project_descriptor(**projectFilesystem.try_value(), a_assertContext)
                                    : cue::Result<cue::ProjectDescriptor>::failure(
                                          cue::make_project_error(a_assertContext, cue::ProjectError::IoFailure,
                                                                  "Generated project root could not be opened"));
    if (!opened || !generated.try_value()->equivalent_to(*opened.try_value()))
    {
        return false;
    }

    auto profile = cue::ProjectCapabilityProfile::create({}, a_assertContext);
    auto snapshot = cue::ProjectCapabilitySnapshot::create({}, a_assertContext);
    auto compatibility =
        profile && snapshot ? cue::evaluate_project_compatibility(
                                  opened.try_value()->schema_version(), cue::k_currentProjectDescriptorSchemaVersion,
                                  opened.try_value()->engine_compatibility(), cue::EngineVersion{0U, 1U, 0U},
                                  *profile.try_value(), *snapshot.try_value(), a_assertContext)
                            : cue::Result<cue::ProjectCompatibilityReport>::failure(
                                  cue::make_project_error(a_assertContext, cue::ProjectError::InvalidCompatibilityInput,
                                                          "Compatibility inputs could not be created"));
    if (!compatibility || !compatibility.try_value()->can_open() ||
        compatibility.try_value()->status() != cue::ProjectCompatibilityStatus::Compatible)
    {
        return false;
    }

    cue::RecentProjectRegistry registry;
    const std::string originalLocator = directory.child_utf8(L"SampleProject");
    if (!registry.register_project(*opened.try_value(), originalLocator, 100U, a_assertContext) ||
        !registry.set_project_pinned(opened.try_value()->project_id(), true, a_assertContext))
    {
        return false;
    }
    auto workspaceFilesystem = cue::create_windows_filesystem_root(directory.child_utf8(L"Workspace"), a_assertContext);
    if (!workspaceFilesystem ||
        !cue::save_recent_project_registry(**workspaceFilesystem.try_value(), registry, a_assertContext))
    {
        return false;
    }
    auto reopenedRegistry = cue::load_recent_project_registry(**workspaceFilesystem.try_value(), a_assertContext);
    if (!reopenedRegistry || reopenedRegistry.try_value()->entries().size() != 1U ||
        !reopenedRegistry.try_value()->entries()[0].is_pinned())
    {
        return false;
    }

    projectFilesystem.try_value()->reset();
    if (MoveFileExW(directory.child(L"SampleProject").c_str(), directory.child(L"MovedProject").c_str(),
                    MOVEFILE_WRITE_THROUGH) == FALSE ||
        !reopenedRegistry.try_value()->mark_project_missing(opened.try_value()->project_id(), a_assertContext) ||
        reopenedRegistry.try_value()->entries()[0].locator_state() != cue::ProjectLocatorState::Missing)
    {
        return false;
    }
    if (!cue::save_recent_project_registry(**workspaceFilesystem.try_value(), *reopenedRegistry.try_value(),
                                           a_assertContext))
    {
        return false;
    }

    auto movedFilesystem = cue::create_windows_filesystem_root(directory.child_utf8(L"MovedProject"), a_assertContext);
    auto movedDescriptor =
        movedFilesystem ? cue::load_project_descriptor(**movedFilesystem.try_value(), a_assertContext)
                        : cue::Result<cue::ProjectDescriptor>::failure(cue::make_project_error(
                              a_assertContext, cue::ProjectError::IoFailure, "Moved project root could not be opened"));
    if (!movedDescriptor || movedDescriptor.try_value()->project_id() != opened.try_value()->project_id())
    {
        return false;
    }
    const std::string movedLocator = directory.child_utf8(L"MovedProject");
    auto duplicate = reopenedRegistry.try_value()->register_project(*movedDescriptor.try_value(), movedLocator, 200U,
                                                                    a_assertContext);
    if (!has_project_error(duplicate, cue::ProjectError::DuplicateProjectId) ||
        !reopenedRegistry.try_value()->reassociate_project(*movedDescriptor.try_value(), movedLocator, 200U,
                                                           a_assertContext) ||
        !reopenedRegistry.try_value()->register_project(*movedDescriptor.try_value(), movedLocator, 250U,
                                                        a_assertContext) ||
        reopenedRegistry.try_value()->entries()[0].locator_state() != cue::ProjectLocatorState::Available)
    {
        return false;
    }

    auto replacementId = cue::ProjectId::parse("87654321-4321-4abc-8def-ba0987654321", a_assertContext);
    auto existingDestination =
        replacementId
            ? cue::generate_blank_project(**parent.try_value(), "MovedProject", "Replacement",
                                          *replacementId.try_value(), "00000000-0000-4000-8000-000000000099",
                                          projectTemplate, a_assertContext)
            : cue::Result<cue::ProjectDescriptor>::failure(cue::make_project_error(
                  a_assertContext, cue::ProjectError::InvalidProjectId, "Replacement ProjectId could not be created"));
    auto invalidNested =
        replacementId
            ? cue::generate_blank_project(**parent.try_value(), "Nested/Project", "Outside", *replacementId.try_value(),
                                          "00000000-0000-4000-8000-000000000099", projectTemplate, a_assertContext)
            : cue::Result<cue::ProjectDescriptor>::failure(cue::make_project_error(
                  a_assertContext, cue::ProjectError::InvalidProjectId, "Nested ProjectId could not be created"));
    if (existingDestination || invalidNested || is_directory(directory.child(L"Nested")))
    {
        return false;
    }

    if (!reopenedRegistry.try_value()->remove_project(movedDescriptor.try_value()->project_id(), a_assertContext) ||
        !cue::save_recent_project_registry(**workspaceFilesystem.try_value(), *reopenedRegistry.try_value(),
                                           a_assertContext))
    {
        return false;
    }
    auto finalRegistry = cue::load_recent_project_registry(**workspaceFilesystem.try_value(), a_assertContext);
    return finalRegistry && finalRegistry.try_value()->entries().empty() &&
           is_directory(directory.child(L"MovedProject")) &&
           is_file(directory.child(L"MovedProject\\CueProject.json")) &&
           is_file(directory.child(L"Workspace\\CueWorkspace.json")) &&
           !is_file(directory.child(L"MovedProject\\CueWorkspace.json")) &&
           !is_file(directory.child(L"Workspace\\CueProject.json")) && has_expected_top_level(directory);
}
} // namespace

/// @brief M09 Project管理Coreの実Filesystem Process Gateを終了Codeで検証する
int main()
{
    TestFatalHandler fatalHandler;
    std::vector<std::unique_ptr<cue::LogSink>> sinks;
    cue::Logger logger(fatalHandler, std::move(sinks));
    cue::AssertContext assertContext(logger, fatalHandler);
    return test_m09_project_process(assertContext) ? 0 : 1;
}
