#include <Cue/Editor/ImGui/FilesPresenter.h>
#include <Cue/Editor/Windows/EditorSession.h>

#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Fatal.h>
#include <Cue/Foundation/Log.h>
#include <Cue/Foundation/Windows/UtfConversion.h>
#include <Cue/IO/Windows/WindowsFilesystem.h>
#include <Cue/Project/Compatibility.h>
#include <Cue/Project/Generator.h>

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <Windows.h>
#include <imgui.h>

namespace
{
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
    /// @brief Temporary Root下へProcess固有Directoryを作成する
    TestDirectory()
    {
        m_path = std::filesystem::temp_directory_path() /
                 (L"CueFilesImGui-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                  std::to_wstring(GetTickCount64()));
        std::filesystem::create_directories(m_path);
    }

    /// @brief Test Directoryの一意所有を保つためCopy構築を禁止する
    TestDirectory(const TestDirectory &) = delete;
    /// @brief Test Directoryの一意所有を保つためCopy代入を禁止する
    TestDirectory &operator=(const TestDirectory &) = delete;

    /// @brief Test所有Directoryだけを終了時に除去する
    ~TestDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(m_path, error);
    }

    /// @brief Temporary RootのNative Pathを返す
    [[nodiscard]] const std::filesystem::path &path() const noexcept
    {
        return m_path;
    }

  private:
    std::filesystem::path m_path;
};

/// @brief 条件が偽ならTest Processを固定Codeで終了する
void require(bool a_condition, int a_code) noexcept
{
    if (!a_condition)
    {
        std::_Exit(a_code);
    }
}

/// @brief Windows PathをStrict UTF-8へ変換する
[[nodiscard]] std::string to_utf8(const std::filesystem::path &a_path, cue::FatalHandler &a_handler)
{
    std::string converted;
    const cue::WindowsUtfConversionResult result =
        cue::convert_windows_utf16_to_utf8(a_path.native(), converted, a_handler);
    require(result.status == cue::WindowsUtfConversionStatus::Success, 2);
    return converted;
}

/// @brief M13 Editorが使用する現在Engine互換性入力を生成する
[[nodiscard]] cue::editor::WindowsEditorEngineConfiguration make_configuration(const cue::AssertContext &a_context)
{
    cue::Result<cue::ProjectCapabilityProfile> profile = cue::ProjectCapabilityProfile::create({}, a_context);
    cue::Result<cue::ProjectCapabilitySnapshot> snapshot = cue::ProjectCapabilitySnapshot::create({}, a_context);
    require(profile && snapshot, 3);
    return {1U, cue::EngineVersion{1U, 0U, 0U}, std::move(*profile.try_value()), std::move(*snapshot.try_value())};
}

/// @brief 生成Projectへ対応するEditor起動値を作る
[[nodiscard]] cue::editor::WindowsEditorLaunchParameters make_parameters(
    const std::filesystem::path &a_projectPath, std::string_view a_projectId,
    const cue::EngineCompatibility &a_compatibility, const cue::AssertContext &a_context)
{
    return {cue::k_editorLaunchProtocolVersion,
            to_utf8(a_projectPath / L"CueProject.json", a_context.fatal_handler()), std::string(a_projectId),
            cue::make_engine_compatibility_id(a_compatibility, a_context), std::nullopt};
}

/// @brief Files ViewModelに指定Locatorが操作可能Entryとして存在するか返す
[[nodiscard]] bool contains_entry(const cue::editor_core::FilesViewModel &a_view,
                                  std::string_view a_locator) noexcept
{
    for (const cue::project_files::ProjectFileDirectorySnapshot &directory : a_view.directories())
    {
        for (const cue::project_files::ProjectFileEntry &entry : directory.entries)
        {
            if (entry.locator == a_locator && entry.is_operable())
            {
                return true;
            }
        }
    }
    return false;
}

/// @brief Headless ImGui FrameでFiles Windowを描画する
void draw_frame(cue::editor::FilesPresenter &a_presenter) noexcept
{
    ImGui::NewFrame();
    ImGui::SetNextWindowFocus();
    a_presenter.draw();
    ImGui::Render();
}

/// @brief Files操作WorkflowとKeyboard Delete Cancelを実Project Sessionで検証する
void test_files_presenter(const cue::AssertContext &a_context)
{
    TestDirectory directory;
    cue::Result<std::unique_ptr<cue::FilesystemRoot>> parent =
        cue::create_windows_filesystem_root(to_utf8(directory.path(), a_context.fatal_handler()), a_context);
    cue::Result<cue::ProjectId> projectId =
        cue::ProjectId::parse("00000000-0000-4000-8000-000000000902", a_context);
    const cue::EngineCompatibility compatibility{cue::EngineVersion{1U, 0U, 0U},
                                                  cue::EngineVersion{2U, 0U, 0U}};
    require(parent && projectId, 4);
    auto generated = cue::generate_blank_project(**parent.try_value(), "FilesImGuiProject", "Files ImGui Project",
                                                 *projectId.try_value(), {compatibility}, a_context);
    require(generated.has_value(), 5);

    const std::filesystem::path projectPath = directory.path() / L"FilesImGuiProject";
    cue::Result<std::unique_ptr<cue::editor::WindowsEditorSession>> session =
        cue::editor::WindowsEditorSession::create(
            make_parameters(projectPath, projectId.try_value()->text(), compatibility, a_context),
            make_configuration(a_context), a_context);
    require(session.has_value(), 6);
    cue::editor::FilesPresenter presenter((*session.try_value())->files_workspace(), a_context);

    require(presenter.submit({cue::editor::FilesIntentKind::CreateFolder, {}, "Folder"}).has_value(), 7);
    require(presenter.submit({cue::editor::FilesIntentKind::CreateEmptyFile, {}, "Work.txt"}).has_value(), 8);
    require(presenter.submit({cue::editor::FilesIntentKind::Rename, "Work.txt", "Renamed.txt"}).has_value(), 9);
    require(presenter.submit({cue::editor::FilesIntentKind::Move, "Renamed.txt", "Folder/Moved.txt"}).has_value(), 10);
    require(presenter.submit({cue::editor::FilesIntentKind::Copy, "Folder/Moved.txt", "Copy.txt"}).has_value(), 11);
    require(contains_entry((*session.try_value())->files_workspace().view_model(), "Copy.txt"), 12);

    cue::Result<void> outside =
        presenter.submit({cue::editor::FilesIntentKind::Move, "Copy.txt", "../Outside.txt"});
    require(!outside && presenter.has_error_message() && presenter.message().starts_with("Files操作に失敗しました") &&
                contains_entry((*session.try_value())->files_workspace().view_model(), "Copy.txt"),
            13);
    require(presenter.submit({cue::editor::FilesIntentKind::Select, "Copy.txt"}).has_value(), 14);
    require(presenter.submit({cue::editor::FilesIntentKind::NavigateDirectory, "Folder"}).has_value() &&
                (*session.try_value())->files_workspace().view_model().selection() == "Folder",
            21);
    require(presenter.submit({cue::editor::FilesIntentKind::Select, "Copy.txt"}).has_value(), 22);

    require(ImGui::CreateContext() != nullptr, 15);
    ImGuiIO &input = ImGui::GetIO();
    input.IniFilename = nullptr;
    input.DisplaySize = ImVec2(1280.0F, 720.0F);
    input.DeltaTime = 1.0F / 60.0F;
    static_cast<void>(input.Fonts->Build());
    draw_frame(presenter);

    input.AddKeyEvent(ImGuiKey_Delete, true);
    draw_frame(presenter);
    draw_frame(presenter);
    const bool didOpenDelete = presenter.is_delete_confirmation_pending() &&
                               ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId) &&
                               presenter.delete_confirmation_entry_count() == 1U &&
                               presenter.delete_confirmation_byte_size() == 0U;
    input.AddKeyEvent(ImGuiKey_Delete, false);
    draw_frame(presenter);
    input.AddKeyEvent(ImGuiKey_Escape, true);
    draw_frame(presenter);
    const bool didCancelDelete = !presenter.is_delete_confirmation_pending() &&
                                 !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId) &&
                                 contains_entry((*session.try_value())->files_workspace().view_model(), "Copy.txt") &&
                                 (*session.try_value())->files_workspace().view_model().recovery_entries().empty();
    input.AddKeyEvent(ImGuiKey_Escape, false);
    draw_frame(presenter);

    const std::uint64_t generation = (*session.try_value())->files_workspace().view_model().generation();
    input.AddKeyEvent(ImGuiKey_F5, true);
    draw_frame(presenter);
    input.AddKeyEvent(ImGuiKey_F5, false);
    draw_frame(presenter);
    const bool didRefresh = (*session.try_value())->files_workspace().view_model().generation() > generation;
    ImGui::DestroyContext();
    require(didOpenDelete && didCancelDelete && didRefresh, 16);

    require(presenter.submit({cue::editor::FilesIntentKind::Delete, "Copy.txt"}).has_value(), 17);
    const std::span<const cue::project_files::RecoveryEntry> recovery =
        (*session.try_value())->files_workspace().view_model().recovery_entries();
    require(!contains_entry((*session.try_value())->files_workspace().view_model(), "Copy.txt") && !recovery.empty(),
            18);
    const std::string operationId = recovery.front().operationId;
    require(presenter.submit({cue::editor::FilesIntentKind::Restore, operationId}).has_value(), 19);
    require(contains_entry((*session.try_value())->files_workspace().view_model(), "Copy.txt"), 20);
}
} // namespace

/// @brief Files PresenterのService接続、Keyboard操作、Cancel、復元Workflowを検証する
int main()
{
    TestFatalHandler handler;
    std::vector<std::unique_ptr<cue::LogSink>> sinks;
    cue::Logger logger(handler, std::move(sinks));
    const cue::AssertContext context(logger, handler);
    test_files_presenter(context);
    return 0;
}
