#pragma once

#include <Cue/EditorCore/EditorDocument.h>
#include <Cue/Foundation/Result.h>
#include <Cue/Input/FrameInputSnapshot.h>
#include <Cue/Input/InputEvent.h>
#include <Cue/Runtime/RuntimeSystemFactory.h>
#include <Cue/Schema/Types.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <thread>
#include <vector>

namespace cue
{
class AssertContext;
}

namespace cue::game_core
{
class MonotonicClock;
class WorldIdentitySource;
} // namespace cue::game_core

namespace cue::runtime
{
class RuntimeApplicationSession;
}

namespace cue::schema
{
class SchemaRegistry;
}

namespace cue::editor_core
{
class ProjectWorkspaceSession;

/// @brief Editor PresentationへRuntime所有物を渡さず通知するPlay Session状態
enum class EditorPlaySessionState : std::uint8_t
{
    Idle,
    Running,
    StopRequested,
    CleanupFailed,
    Stopped
};

/// @brief 一時Play SessionのIdentityと進行状態だけを所有するPresentation向け値
struct EditorPlaySessionSnapshot final
{
    EditorPlaySessionState state = EditorPlaySessionState::Idle;
    std::optional<EditorDocumentId> documentId;
    std::uint64_t generation = 0U;
    std::uint64_t worldId = 0U;
    std::uint64_t frameCount = 0U;
    bool hasFailure = false;
};

/// @brief EditorDocumentから独立Snapshotを作り一つのRuntime Application Sessionを所有するController
///
/// RuntimeWorld、SceneInstance、EntityHandle、Runtime Pointerを公開せず、状態は所有Value Snapshotだけで通知する
/// 全公開APIとDestructorはcreateを呼んだOwner Threadから実行する
/// Workspace Session、Clock、World Identity Source、Schema Registry、System
/// Factory、AssertContextはControllerより長く生存させる
class EditorPlaySessionController final
{
  public:
    /// @brief FactoryだけがEditor Play Session Controller Constructorへ渡せる生成権限
    class ConstructionKey final
    {
      public:
        /// @brief 生成権限を値として複製する
        ConstructionKey(const ConstructionKey &) noexcept = default;
        /// @brief 生成権限を値として複製代入する
        ConstructionKey &operator=(const ConstructionKey &) noexcept = default;
        /// @brief 生成権限を値として移動する
        ConstructionKey(ConstructionKey &&) noexcept = default;
        /// @brief 生成権限を値として移動代入する
        ConstructionKey &operator=(ConstructionKey &&) noexcept = default;
        /// @brief bit_castによる権限生成を防ぐnon-trivialな破棄を行う
        ~ConstructionKey() noexcept
        {
        }

      private:
        friend class EditorPlaySessionController;

        /// @brief EditorPlaySessionController Factoryだけに生成権限を発行する
        ConstructionKey() noexcept = default;
    };

    /// @brief Project Scope依存とRuntime構成値から空のPlay Controllerを生成する
    /// @param a_workspaceSession Controllerより長く生存しDocument所属検証に使用するRead-only Session
    /// @param a_worldIdentitySource Controllerより長く生存し全Play Worldへ一意Identityを発行するProcess Scope Owner
    /// @param a_clock Controllerより長く生存し同時SessionとMutable状態を共有しない単調Clock
    /// @param a_schemaRegistry Controllerより長く生存するSeal済みSchema定義
    /// @param a_systemFactories Controllerより長く生存しSessionごとに独立Systemを生成するProject Scope定義
    /// @param a_firstGeneration Process Composition Rootが重複しない範囲として割り当てる最初のnon-zero世代
    /// @param a_maxDeltaNanoseconds Simulationへ一Frameで適用できる正の最大Delta
    /// @param a_assertContext Controllerと返却Errorより長く生存する診断Context
    [[nodiscard]] static Result<std::unique_ptr<EditorPlaySessionController>> create(
        const ProjectWorkspaceSession &a_workspaceSession, game_core::WorldIdentitySource &a_worldIdentitySource,
        game_core::MonotonicClock &a_clock, const schema::SchemaRegistry &a_schemaRegistry,
        std::span<const runtime::RuntimeSystemFactory *const> a_systemFactories, schema::TypeId a_transformTypeId,
        schema::TypeId a_sceneObjectStateTypeId, std::uint64_t a_firstGeneration, std::int64_t a_maxDeltaNanoseconds,
        const AssertContext &a_assertContext) noexcept;

    /// @brief Factory外からの既定構築を禁止する
    EditorPlaySessionController() = delete;
    /// @brief Play Session所有権の複製を禁止する
    EditorPlaySessionController(const EditorPlaySessionController &) = delete;
    /// @brief Play Session所有権の複製代入を禁止する
    EditorPlaySessionController &operator=(const EditorPlaySessionController &) = delete;
    /// @brief Owner Threadと非所有依存を固定するためMove構築を禁止する
    EditorPlaySessionController(EditorPlaySessionController &&) = delete;
    /// @brief Owner Threadと非所有依存を固定するためMove代入を禁止する
    EditorPlaySessionController &operator=(EditorPlaySessionController &&) = delete;
    /// @brief Owner Thread上でlive Runtime所有物がないことを検証してControllerを破棄する
    ~EditorPlaySessionController() noexcept;

    /// @brief 検証済みProject Scope依存と世代範囲を非所有で保持する
    EditorPlaySessionController(ConstructionKey, const ProjectWorkspaceSession &a_workspaceSession,
                                game_core::WorldIdentitySource &a_worldIdentitySource,
                                game_core::MonotonicClock &a_clock, const schema::SchemaRegistry &a_schemaRegistry,
                                std::vector<const runtime::RuntimeSystemFactory *> a_systemFactories,
                                schema::TypeId a_transformTypeId, schema::TypeId a_sceneObjectStateTypeId,
                                std::uint64_t a_firstGeneration, std::int64_t a_maxDeltaNanoseconds,
                                const AssertContext &a_assertContext) noexcept;

    /// @brief 現在Memory上のEditorDocumentを独立Snapshotへ複製して新しいPlay Sessionを開始する
    /// @details Dirty、Saved State、Undo／Redo、Selection、SceneDocumentを変更せずRunning到達後だけActiveとして公開する
    [[nodiscard]] Result<void> start(EditorDocumentId a_documentId) noexcept;
    /// @brief Running SessionへNative Pointerを含まないPortable Input Eventを値で登録する
    /// @return OverflowなしでFIFOへ保持した場合にtrueを返し、Overflow時もInput Reset契約を適用してfalseを返す
    [[nodiscard]] Result<bool> push_input_event(InputEvent a_event) noexcept;
    /// @brief Portable Input Captureを渡してEditor HostからRuntime Frameを一回進める
    [[nodiscard]] Result<void> advance_frame(InputCapture a_capture) noexcept;
    /// @brief Active Playへ冪等な利用者Stop要求を記録して新規Frameを停止する
    [[nodiscard]] Result<void> request_stop() noexcept;
    /// @brief Runtime所有物を完全に終了し、停止済みの値SnapshotだけをEditor側へ残す
    /// @details CleanupFailedでは未終了Ownerを保持し、同じOwner Threadからの再呼出しで未完了Cleanupだけを続行する
    [[nodiscard]] Result<void> stop() noexcept;
    /// @brief Runtime Pointerを含まない現在または直前Playの所有Value Snapshotを返す
    [[nodiscard]] EditorPlaySessionSnapshot state_snapshot() const noexcept;

  private:
    /// @brief 次のSession Generationを一度だけ発行してOverflow後の再Playを拒否する
    [[nodiscard]] Result<std::uint64_t> issue_generation() noexcept;
    /// @brief Active Runtime Sessionの値だけをPresentation向けSnapshotへ同期する
    void synchronize_snapshot() noexcept;
    /// @brief 下位ErrorへPlay対象DocumentのStable Contextを追加する
    void add_document_context(Error &a_error, EditorDocumentId a_documentId) const noexcept;
    /// @brief 現在ThreadがController Ownerであることを全構成で検証する
    void assert_owner_thread() const noexcept;

    const ProjectWorkspaceSession *m_workspaceSession;
    game_core::WorldIdentitySource *m_worldIdentitySource;
    game_core::MonotonicClock *m_clock;
    const schema::SchemaRegistry *m_schemaRegistry;
    const AssertContext *m_assertContext;
    std::thread::id m_ownerThread;
    std::vector<const runtime::RuntimeSystemFactory *> m_systemFactories;
    schema::TypeId m_transformTypeId;
    schema::TypeId m_sceneObjectStateTypeId;
    std::int64_t m_maxDeltaNanoseconds;
    std::uint64_t m_nextGeneration;
    bool m_isGenerationExhausted = false;
    std::unique_ptr<runtime::RuntimeApplicationSession> m_session;
    EditorPlaySessionSnapshot m_snapshot;
};
} // namespace cue::editor_core
