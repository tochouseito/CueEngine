#pragma once

#include <Cue/Build/Service.h>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cue
{
class AssertContext;
class Error;
} // namespace cue

namespace cue::editor
{
/// @brief Build UIからGame Build Serviceへ送る意味Command
enum class EditorBuildCommand : std::uint8_t
{
    Start,
    Cancel,
    Retry
};

/// @brief Build中のEditor終了確認へ適用するUser判断
enum class EditorBuildShutdownDecision : std::uint8_t
{
    CancelBuildAndClose,
    KeepEditorOpen
};

/// @brief 一回のBuildへ割り当てるlowercase UUID Version 4の発行境界
class BuildOperationIdSource
{
  public:
    /// @brief Operation ID Source実装の暗黙Copy構築を禁止する
    BuildOperationIdSource(const BuildOperationIdSource &) = delete;
    /// @brief Operation ID Source実装の暗黙Copy代入を禁止する
    BuildOperationIdSource &operator=(const BuildOperationIdSource &) = delete;
    /// @brief 派生Operation ID Sourceを正しく破棄する
    virtual ~BuildOperationIdSource() = default;

    /// @brief 新しいBuild Operation IDを発行する
    [[nodiscard]] virtual Result<std::string> next_operation_id() noexcept = 0;

  protected:
    /// @brief 派生Operation ID Sourceを初期化する
    BuildOperationIdSource() noexcept = default;
};

/// @brief Game Build ServiceのSnapshotをBuild操作、Console、Artifact表示へ変換するImGui Adapter
///
/// GameBuildServiceはPresenterより長く生存させる。Operation ID SourceはPresenterが所有する。
/// 全操作は生成Threadだけで行い、PresenterはCMakeまたはChild Process APIを直接参照しない。
class BuildPresenter final
{
  public:
    /// @brief Service借用とUI状態のCopy構築を禁止する
    BuildPresenter(const BuildPresenter &) = delete;
    /// @brief Service借用とUI状態のCopy代入を禁止する
    BuildPresenter &operator=(const BuildPresenter &) = delete;
    /// @brief 安定したUI所有位置を保つためMove構築を禁止する
    BuildPresenter(BuildPresenter &&) = delete;
    /// @brief 安定したUI所有位置を保つためMove代入を禁止する
    BuildPresenter &operator=(BuildPresenter &&) = delete;
    /// @brief 保持するOperation ID SourceとUI状態を解放する
    ~BuildPresenter() noexcept;

    /// @brief Build Service、Project Root、Operation ID Sourceを一つのPresentation Ownerへ束ねる
    [[nodiscard]] static std::unique_ptr<BuildPresenter> create(
        GameBuildService &a_service, std::string a_projectRoot, BuildWorkspaceCompatibility a_workspaceCompatibility,
        std::unique_ptr<BuildOperationIdSource> a_operationIdSource, const AssertContext &a_assertContext) noexcept;

    /// @brief Service Snapshotを現在OperationとSession内完了履歴へ同期する
    void refresh() noexcept;
    /// @brief Keyboard操作を意味Commandへ変換する
    void process_shortcuts() noexcept;
    /// @brief Build設定、操作、進捗、Console、Artifact、終了確認を描画する
    void draw() noexcept;

    /// @brief Commandが現在有効な場合だけGame Build Serviceへ送る
    [[nodiscard]] bool submit(EditorBuildCommand a_command) noexcept;
    /// @brief 次回Startで使用するConfigurationをBuild停止中だけ変更する
    [[nodiscard]] bool set_configuration(BuildConfiguration a_configuration) noexcept;
    /// @brief 次回StartがConfigureを必須にするかBuild停止中だけ変更する
    [[nodiscard]] bool set_force_configure(bool a_forceConfigure) noexcept;

    /// @brief Build中なら終了確認を開始し、停止済みなら即時終了可能を返す
    [[nodiscard]] bool begin_editor_shutdown() noexcept;
    /// @brief 終了確認判断を適用し、Cancel完了後に終了可能通知を予約する
    [[nodiscard]] bool respond_to_editor_shutdown(EditorBuildShutdownDecision a_decision) noexcept;
    /// @brief UIで確定した終了可能通知を一度だけ返す
    [[nodiscard]] bool take_shutdown_ready() noexcept;

    /// @brief 現在のService Operation Snapshotを返す
    ///
    /// 返却参照は次に非const Member Functionを呼ぶまで有効であり、BuildPresenterの寿命を超えて保持しない。
    [[nodiscard]] const BuildOperationSnapshot &current_snapshot() const noexcept;
    /// @brief Session内に保持した完了Operationを古い順で返す
    ///
    /// 返却spanと各要素参照は次に非const Member Functionを呼ぶまで有効であり、BuildPresenterの寿命を超えて保持しない。
    [[nodiscard]] std::span<const BuildOperationSnapshot> saved_operations() const noexcept;
    /// @brief ConsoleとArtifactに表示するOperationをIdentityで選択する
    [[nodiscard]] bool select_operation(std::string_view a_operationId) noexcept;
    /// @brief 現在表示対象のOperation Snapshotを返す
    ///
    /// 返却参照は次に非const Member Functionを呼ぶまで有効であり、BuildPresenterの寿命を超えて保持しない。
    [[nodiscard]] const BuildOperationSnapshot &displayed_snapshot() const noexcept;
    /// @brief 現在表示対象でOperation Identityが一致するLog件数を返す
    [[nodiscard]] std::size_t displayed_log_count() const noexcept;

    /// @brief 最後のBuild UI操作に対応する日本語Messageを返す
    [[nodiscard]] std::string_view message() const noexcept;
    /// @brief 現在Messageが回復可能なBuild失敗を表すか返す
    [[nodiscard]] bool has_error_message() const noexcept;
    /// @brief 同時実行禁止を含め新規Startが可能か返す
    [[nodiscard]] bool can_start() const noexcept;
    /// @brief Active BuildへCancelを送れるか返す
    [[nodiscard]] bool can_cancel() const noexcept;
    /// @brief 完了した現在Operationを新Identityで再実行できるか返す
    [[nodiscard]] bool can_retry() const noexcept;
    /// @brief Editor終了確認がUser判断またはCancel完了待ちか返す
    [[nodiscard]] bool is_shutdown_confirmation_pending() const noexcept;

  private:
    /// @brief 検証済み依存とUI状態の所有権からPresenterを構築する
    BuildPresenter(GameBuildService &a_service, std::string a_projectRoot,
                   BuildWorkspaceCompatibility a_workspaceCompatibility,
                   std::unique_ptr<BuildOperationIdSource> a_operationIdSource,
                   const AssertContext &a_assertContext) noexcept;

    /// @brief 現在OperationをSession内の再表示履歴へ退避する
    void save_current_operation();
    /// @brief Build Errorを日本語UIへ変換する
    void set_error(const Error &a_error, std::string_view a_operation);
    /// @brief 正常操作の短い日本語Messageを設定する
    void set_status(std::string_view a_status);
    /// @brief Build操作とConfiguration選択を描画する
    void draw_toolbar() noexcept;
    /// @brief 現在または保存済みOperationのStageと状態を描画する
    void draw_progress() noexcept;
    /// @brief Operation選択、Filter、Copyを持つStructured Log Consoleを描画する
    void draw_console();
    /// @brief CurrentおよびLatest Successful Artifact Inventoryを描画する
    void draw_artifacts() noexcept;
    /// @brief Build中のEditor終了確認とCancel待ちを描画する
    void draw_shutdown_confirmation() noexcept;
    /// @brief Build終了確認を解除して一回のClose Ready通知を確定する
    void mark_shutdown_ready() noexcept;
    /// @brief 予期しない例外をFatalHandlerへ渡す
    [[noreturn]] void terminate_exception() const noexcept;

    GameBuildService *m_service;
    const AssertContext *m_assertContext;
    std::unique_ptr<BuildOperationIdSource> m_operationIdSource;
    std::string m_projectRoot;
    BuildWorkspaceCompatibility m_workspaceCompatibility;
    BuildOperationSnapshot m_current;
    std::vector<BuildOperationSnapshot> m_savedOperations;
    std::string m_displayOperationId;
    std::array<char, 128U> m_filter{};
    std::string m_message;
    BuildConfiguration m_configuration = BuildConfiguration::Debug;
    bool m_forceConfigure = true;
    bool m_hasError = false;
    bool m_openShutdownConfirmation = false;
    bool m_closeShutdownConfirmation = false;
    bool m_isShutdownConfirmationPending = false;
    bool m_isShutdownWaitingForCancel = false;
    bool m_isShutdownReady = false;
};
} // namespace cue::editor
