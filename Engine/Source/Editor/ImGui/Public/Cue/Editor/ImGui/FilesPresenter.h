#pragma once

#include <Cue/EditorCore/FilesWorkspace.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace cue
{
class AssertContext;
}

namespace cue::editor
{
/// @brief Files UIがFrame末尾にFilesWorkspaceServiceへ渡す意味操作
enum class FilesIntentKind : std::uint8_t
{
    Refresh,
    SetSearchFilter,
    SetExpanded,
    NavigateDirectory,
    Select,
    ClearSelection,
    CreateFolder,
    CreateEmptyFile,
    Rename,
    Move,
    Copy,
    PrepareDelete,
    Delete,
    Restore
};

/// @brief Native PathやWidget IDを持たないProject相対のFiles操作値
struct FilesIntent final
{
    FilesIntentKind kind = FilesIntentKind::Refresh;
    std::string source;
    std::string destination;
    bool isExpanded = false;
};

/// @brief FilesViewModelの不変SnapshotをImGui操作へ変換するPresentation Adapter
///
/// FilesWorkspaceServiceとAssertContextはPresenterより長く生存させ、生成Threadだけでdrawとsubmitを呼び出す
class FilesPresenter final
{
  public:
    /// @brief Serviceへの非所有参照と空のPresentation Stateを構築する
    explicit FilesPresenter(editor_core::FilesWorkspaceService &a_service,
                            const AssertContext &a_assertContext) noexcept;
    /// @brief Service参照の重複を防ぐためCopy構築を禁止する
    FilesPresenter(const FilesPresenter &) = delete;
    /// @brief Service参照の重複を防ぐためCopy代入を禁止する
    FilesPresenter &operator=(const FilesPresenter &) = delete;
    /// @brief ImGui Session中のAddress安定性を保つためMove構築を禁止する
    FilesPresenter(FilesPresenter &&) = delete;
    /// @brief ImGui Session中のAddress安定性を保つためMove代入を禁止する
    FilesPresenter &operator=(FilesPresenter &&) = delete;
    /// @brief Presentation Stateだけを解放しServiceの所有権は変更しない
    ~FilesPresenter() = default;

    /// @brief Files Windowを描画し、View参照を解放したFrame末尾で最大一Intentを適用する
    void draw() noexcept;
    /// @brief Project相対の意味操作をFilesWorkspaceServiceへ適用して日本語診断を更新する
    [[nodiscard]] Result<void> submit(FilesIntent a_intent) noexcept;

    /// @brief Delete確認が未確定の対象を保持しているか返す
    [[nodiscard]] bool is_delete_confirmation_pending() const noexcept;
    /// @brief Delete確認中の対象自身を含むEntry数を返す
    [[nodiscard]] std::size_t delete_confirmation_entry_count() const noexcept;
    /// @brief Delete確認中のRegular File合計Logical Byte数を返す
    [[nodiscard]] std::uint64_t delete_confirmation_byte_size() const noexcept;
    /// @brief 最後の操作結果または回復可能診断を返す
    [[nodiscard]] std::string_view message() const noexcept;
    /// @brief 現在Messageが回復可能な操作失敗を表すか返す
    [[nodiscard]] bool has_error_message() const noexcept;

  private:
    enum class DialogMode : std::uint8_t
    {
        None,
        CreateFolder,
        CreateEmptyFile,
        Rename,
        Move,
        Copy
    };

    /// @brief 展開済みSnapshotだけを辿りDirectory Tree操作をIntentへ退避する
    void draw_directory_children(std::string_view a_parent, const editor_core::FilesViewModel &a_view,
                                 std::optional<FilesIntent> &a_pendingIntent);
    /// @brief 現在DirectoryまたはSearch結果をFile Listとして描画する
    void draw_file_list(const editor_core::FilesViewModel &a_view, std::optional<FilesIntent> &a_pendingIntent);
    /// @brief Create／Rename／Move／Copy Dialogを描画し確定操作だけをIntentへ変換する
    void draw_action_dialog(std::optional<FilesIntent> &a_pendingIntent);
    /// @brief 復元可能Deleteの確認とCancel経路を描画する
    void draw_delete_dialog(std::optional<FilesIntent> &a_pendingIntent);
    /// @brief ToolbarとKeyboard Shortcutから一つの操作候補を生成する
    void draw_toolbar(const editor_core::FilesViewModel &a_view, std::optional<FilesIntent> &a_pendingIntent);
    /// @brief Internal Drag PayloadをProject相対Move Intentへ変換する
    void accept_move_drop(std::string_view a_destinationDirectory, std::optional<FilesIntent> &a_pendingIntent);
    /// @brief Project相対Locatorから末尾Segmentを返す
    [[nodiscard]] static std::string_view filename(std::string_view a_locator) noexcept;
    /// @brief Directoryと末尾SegmentをProject相対Locatorへ合成する
    [[nodiscard]] static std::string compose_destination(std::string_view a_directory,
                                                         std::string_view a_name);
    /// @brief 固定長Input BufferへProject相対値を切り詰めず設定する
    static void assign_input(std::array<char, 256U> &a_input, std::string_view a_value) noexcept;
    /// @brief 現在Dialog種別の表示名を返す
    [[nodiscard]] const char *dialog_title() const noexcept;
    /// @brief Service Errorを日本語の回復可能Messageへ変換する
    void set_error(const Error &a_error);
    /// @brief 成功または注意が必要な操作結果を日本語Messageへ変換する
    void set_operation_status(FilesIntentKind a_kind, project_files::ProjectFileOperationOutcome a_outcome);
    /// @brief 予期しないAllocation失敗をFatal境界へ渡す
    [[noreturn]] void terminate_exception() const noexcept;

    editor_core::FilesWorkspaceService *m_service;
    const AssertContext *m_assertContext;
    std::array<char, 256U> m_search{};
    std::array<char, 256U> m_source{};
    std::array<char, 256U> m_destination{};
    std::string m_currentDirectory;
    std::optional<std::string> m_deleteTarget;
    std::size_t m_deleteEntryCount = 0U;
    std::uint64_t m_deleteByteSize = 0U;
    std::string m_message;
    DialogMode m_dialogMode = DialogMode::None;
    bool m_openActionDialog = false;
    bool m_openDeleteDialog = false;
    bool m_focusSearch = false;
    bool m_hasError = false;
};
} // namespace cue::editor
