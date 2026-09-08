#pragma once

#include <Cue/Foundation/Result.h>
#include <Cue/GameCore/RuntimeSystem.h>
#include <Cue/Input/FrameInputSnapshot.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <thread>

namespace cue
{
class AssertContext;
class InputEventQueue;
class InputState;
} // namespace cue

namespace cue::game_core
{
class GameClock;
class MonotonicClock;
class RuntimeSystemRegistry;
class RuntimeWorld;
class WorldIdentitySource;
class StructuralCommandReport;
} // namespace cue::game_core

namespace cue::scene
{
class SceneSnapshot;
}

namespace cue::schema
{
class SchemaRegistry;
class TypeId;
} // namespace cue::schema

namespace cue::runtime
{
class RuntimeSceneSession;
enum class RuntimeError : std::int64_t;

/// @brief 一回のGame実行を所有するApplication Sessionの寿命状態
enum class RuntimeApplicationSessionState : std::uint8_t
{
    Constructed,
    Starting,
    RollingBack,
    Running,
    StopRequested,
    Stopping,
    CleanupFailed,
    Stopped
};

/// @brief Runtime Application Sessionが新規Frameを停止した最初の理由
enum class RuntimeApplicationStopReason : std::uint8_t
{
    None,
    Requested,
    WindowClosed,
    HostFailure,
    RuntimeFailure
};

/// @brief Input、Clock、System Registry、Scene Sessionを一つのOwner Threadへ束ねるGame実行Owner
///
/// Window、Native Handle、Renderer、Editor、SceneDocumentは所有せず、停止後の同一Object再利用を許可しない
/// 全公開APIとDestructorはcreateを呼んだOwner Threadから実行する
/// 注入ClockとAssertContextはSession破棄まで、開始時Identity SourceとSchema Registryは停止完了まで生存させる
class RuntimeApplicationSession final
{
  public:
    /// @brief FactoryだけがRuntime Application Session Constructorへ渡せる生成権限
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
        friend class RuntimeApplicationSession;

        /// @brief RuntimeApplicationSession Factoryだけに生成権限を発行する
        ConstructionKey() noexcept = default;
    };

    /// @brief Session GenerationとClock依存から未開始の一回限りSessionを生成する
    /// @param a_generation Composition Rootが一つのProcess内で重複させないnon-zero世代
    /// @param a_clock Sessionより長く生存しMutable状態を別Sessionと共有しない単調Clock
    /// @param a_maxDeltaNanoseconds Simulationへ一Frameで適用できる正の最大Delta
    /// @param a_assertContext Sessionと返却Errorより長く生存する診断Context
    [[nodiscard]] static Result<std::unique_ptr<RuntimeApplicationSession>> create(
        std::uint64_t a_generation, game_core::MonotonicClock &a_clock, std::int64_t a_maxDeltaNanoseconds,
        const AssertContext &a_assertContext) noexcept;

    /// @brief Factory外からの既定構築を禁止する
    RuntimeApplicationSession() = delete;
    /// @brief Session所有権の複製を禁止する
    RuntimeApplicationSession(const RuntimeApplicationSession &) = delete;
    /// @brief Session所有権の複製代入を禁止する
    RuntimeApplicationSession &operator=(const RuntimeApplicationSession &) = delete;
    /// @brief 内部OwnerのAddressを固定するためMove構築を禁止する
    RuntimeApplicationSession(RuntimeApplicationSession &&) = delete;
    /// @brief 内部OwnerのAddressを固定するためMove代入を禁止する
    RuntimeApplicationSession &operator=(RuntimeApplicationSession &&) = delete;
    /// @brief Owner Thread上で未終了Ownerが残っていないことを検証してSession-local状態を破棄する
    ~RuntimeApplicationSession() noexcept;

    /// @brief Factoryが検証済みGenerationと診断Lifetimeを固定する
    RuntimeApplicationSession(ConstructionKey, std::uint64_t a_generation,
                              const AssertContext &a_assertContext) noexcept;

    /// @brief 開始前に一意System所有権と決定的実行順定義をSessionへ登録する
    [[nodiscard]] Result<void> register_system(game_core::RuntimeSystemDescriptor a_descriptor,
                                               std::unique_ptr<game_core::RuntimeSystem> a_system) noexcept;
    /// @brief Snapshotを新Worldへ実体化しSystemとClockを開始してRunningへ移る
    /// @details Snapshotは呼出中だけ参照し、開始失敗時も成功済みOwnerだけを逆順Cleanupする
    /// @param a_snapshot 呼出中だけ借用し開始成功後は保持しない不変Scene入力
    /// @param a_identitySource Session停止完了まで生存するWorld Identity発行元
    /// @param a_schemaRegistry Session停止完了まで不変かつ生存するSchema定義
    /// @param a_transformTypeId RuntimeWorldが使用するTransformのStable Type ID
    /// @param a_sceneObjectStateTypeId Scene実体化が使用するObject StateのStable Type ID
    [[nodiscard]] Result<void> start(const scene::SceneSnapshot &a_snapshot,
                                     game_core::WorldIdentitySource &a_identitySource,
                                     const schema::SchemaRegistry &a_schemaRegistry, schema::TypeId a_transformTypeId,
                                     schema::TypeId a_sceneObjectStateTypeId) noexcept;
    /// @brief Portable Input、Clock、System Update、Structural Safe Pointを一Frameの固定順で実行する
    /// @details Runtime失敗はFatalにせず保持し、StopRequestedへ移って新規Frameを拒否する
    [[nodiscard]] Result<void> advance_frame(InputCapture a_capture) noexcept;
    /// @brief 次の安全な境界で新規Frameを停止する最初のHost理由を冪等に記録する
    [[nodiscard]] Result<void> request_stop(RuntimeApplicationStopReason a_reason) noexcept;
    /// @brief System、Command、Scene、Worldを逆開始順で終了し未完了Cleanupだけを再試行する
    [[nodiscard]] Result<void> stop() noexcept;

    /// @brief Native AdapterがPortable Eventを格納するSession-local FIFOを返す
    /// @details 返却参照はSession破棄まで有効だが、入力とSession APIは同じOwner Threadで操作する
    [[nodiscard]] InputEventQueue &input_events() noexcept;
    /// @brief 現在のSession寿命状態を返す
    [[nodiscard]] RuntimeApplicationSessionState state() const noexcept;
    /// @brief Composition Rootが割り当てたStable Session Generationを返す
    [[nodiscard]] std::uint64_t generation() const noexcept;
    /// @brief Scene開始成功時のWorld Identityを停止後も値として返す
    [[nodiscard]] std::uint64_t world_id() const noexcept;
    /// @brief 次に成功するFrameへ割り当てるIndexを返す
    [[nodiscard]] std::uint64_t next_frame_index() const noexcept;
    /// @brief Sessionへ登録されたSystem数を返す
    [[nodiscard]] std::size_t system_count() const noexcept;
    /// @brief 新規Frameを停止した最初の理由を返す
    [[nodiscard]] RuntimeApplicationStopReason stop_reason() const noexcept;
    /// @brief 最初のRuntime失敗と順序付きCleanup診断を非所有参照で返す
    [[nodiscard]] const Error *try_failure() const noexcept;
    /// @brief Stopped Sessionから保持診断を一度だけ呼出し側へ移す
    [[nodiscard]] Result<std::optional<Error>> take_failure() noexcept;

  private:
    /// @brief 現在ThreadがSession Ownerであることを全構成で検証する
    void assert_owner_thread() const noexcept;
    /// @brief 既存Primaryを維持して後続失敗を順序付きCleanup診断へ追加する
    void retain_failure(Error &&a_error, const char *a_context, const char *a_label) noexcept;
    /// @brief Command Reportの失敗を保持診断へFIFO順で追加する
    void retain_command_failures(const game_core::StructuralCommandReport &a_report) noexcept;
    /// @brief System停止後の非空Command BatchをScene終了前に一度だけ消費する
    [[nodiscard]] Result<void> flush_system_commands() noexcept;
    /// @brief 開始途中に成功したOwnerだけを逆順Cleanupする
    [[nodiscard]] Result<void> rollback_start() noexcept;
    /// @brief SceneとWorldを終了して停止済み所有物を解放する
    [[nodiscard]] Result<void> finish_scene_cleanup() noexcept;
    /// @brief 詳細診断をSession内へ保持した操作失敗を呼出し元へ安定Categoryで通知する
    [[nodiscard]] Result<void> make_operation_failure(RuntimeError a_code, const char *a_summary) const noexcept;
    const AssertContext *m_assertContext;
    std::thread::id m_ownerThread;
    std::uint64_t m_generation;
    std::uint64_t m_worldId = 0;
    RuntimeApplicationSessionState m_state = RuntimeApplicationSessionState::Constructed;
    RuntimeApplicationStopReason m_stopReason = RuntimeApplicationStopReason::None;
    std::unique_ptr<InputEventQueue> m_inputEvents;
    std::unique_ptr<InputState> m_inputState;
    std::unique_ptr<game_core::GameClock> m_clock;
    std::unique_ptr<game_core::RuntimeSystemRegistry> m_systemRegistry;
    std::unique_ptr<RuntimeSceneSession> m_sceneSession;
    std::optional<Error> m_failure;
    bool m_hasFlushedSystemCommands = false;
};
} // namespace cue::runtime
