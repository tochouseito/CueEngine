#pragma once

#include <Cue/Foundation/Result.h>
#include <Cue/Scene/Instantiation.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <thread>

namespace cue
{
class AssertContext;
}

namespace cue::game_core
{
class RuntimeWorld;
class WorldIdentitySource;
} // namespace cue::game_core

namespace cue::schema
{
class SchemaRegistry;
class TypeId;
} // namespace cue::schema

namespace cue::runtime::details
{
class SceneEndOperation;
}

namespace cue::runtime::test_support
{
class RuntimeSceneSessionProbe;
}

namespace cue::runtime
{
class RuntimeApplicationSession;

/// @brief Runtime Scene Sessionの明示的な所有状態
enum class RuntimeSceneSessionState : std::uint8_t
{
    Constructed,
    Running,
    CleanupFailed,
    Stopped
};

/// @brief 一つのRuntimeWorldと最大一つのSceneInstanceを同じOwner Threadで所有する境界
///
/// Snapshotはstart中だけ参照し、成功後もMutable SceneDocumentまたはSnapshotを保持しない
class RuntimeSceneSession final
{
  public:
    /// @brief Factory だけが Runtime Scene Session Constructor へ渡せる生成権限
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
        /// @brief bit_cast による権限生成を防ぐ non-trivial な破棄を行う
        ~ConstructionKey() noexcept
        {
        }

      private:
        friend class RuntimeSceneSession;

        /// @brief RuntimeSceneSession Factory だけに生成権限を発行する
        ConstructionKey() noexcept = default;
    };

    /// @brief 新規WorldへSnapshotを同期実体化しRunning Sessionだけを返す
    /// @details 失敗時は生成済みEntityとWorldを終了し、SnapshotまたはSceneDocumentを保持しない
    [[nodiscard]] static Result<std::unique_ptr<RuntimeSceneSession>> start(
        const scene::SceneSnapshot &a_snapshot, game_core::WorldIdentitySource &a_identitySource,
        const schema::SchemaRegistry &a_schemaRegistry, schema::TypeId a_transformTypeId,
        schema::TypeId a_sceneObjectStateTypeId, const AssertContext &a_assertContext) noexcept;

    /// @brief Factory外からの既定構築を禁止する
    RuntimeSceneSession() = delete;
    /// @brief Session所有権の複製を禁止する
    RuntimeSceneSession(const RuntimeSceneSession &) = delete;
    /// @brief Session所有権の複製代入を禁止する
    RuntimeSceneSession &operator=(const RuntimeSceneSession &) = delete;
    /// @brief RuntimeWorld AddressとOwner Threadを固定するためMove構築を禁止する
    RuntimeSceneSession(RuntimeSceneSession &&) = delete;
    /// @brief RuntimeWorld AddressとOwner Threadを固定するためMove代入を禁止する
    RuntimeSceneSession &operator=(RuntimeSceneSession &&) = delete;
    /// @brief ConstructedまたはStopped状態だけを破棄しlive所有物の放棄を拒否する
    ~RuntimeSceneSession() noexcept;

    /// @brief Factory が固定する Owner Thread を保持する未公開開始前 Session を構築する
    RuntimeSceneSession(ConstructionKey, const AssertContext &a_assertContext) noexcept;

    /// @brief Sceneを一度だけ終了してからWorldの最終Safe PointとShutdownを実行する
    /// @details CleanupFailedでは完了済みStepを再実行せず未完了World Shutdownだけを再試行する
    [[nodiscard]] Result<void> end() noexcept;

    /// @brief 現在のSession所有状態を返す
    [[nodiscard]] RuntimeSceneSessionState state() const noexcept;
    /// @brief Sessionが発行したWorld Identityを停止後も値として返す
    [[nodiscard]] std::uint64_t world_id() const noexcept;
    /// @brief 現在SceneInstanceが所有する生存Entity数を返す
    [[nodiscard]] std::size_t entity_count() const noexcept;

  private:
    friend class RuntimeApplicationSession;
    friend class test_support::RuntimeSceneSessionProbe;

    /// @brief ProductionまたはTestのScene終了Operationを指定して同じStart経路を実行する
    [[nodiscard]] static Result<std::unique_ptr<RuntimeSceneSession>> start_with_operation(
        const scene::SceneSnapshot &a_snapshot, game_core::WorldIdentitySource &a_identitySource,
        const schema::SchemaRegistry &a_schemaRegistry, schema::TypeId a_transformTypeId,
        schema::TypeId a_sceneObjectStateTypeId, const AssertContext &a_assertContext,
        const details::SceneEndOperation &a_endOperation) noexcept;
    /// @brief Runtime Application Sessionへ所有Worldの短命な明示参照を渡す
    [[nodiscard]] game_core::RuntimeWorld &runtime_world() noexcept;
    /// @brief 現在ThreadがSession Ownerであることを全構成で検証する
    void assert_owner_thread() const noexcept;
    /// @brief Start失敗時にWorldを終了してSessionを安全に破棄可能にする
    [[nodiscard]] Error finish_failed_start(Error &&a_cause) noexcept;
    /// @brief Scene終了の外側ResultまたはReport FailureをFatal診断へ正規化する
    [[noreturn]] void terminate_scene_cleanup(Result<scene::SceneInstanceEndReport> &&a_result) noexcept;
    const AssertContext *m_assertContext;
    const details::SceneEndOperation *m_endOperation = nullptr;
    std::thread::id m_ownerThread;
    RuntimeSceneSessionState m_state = RuntimeSceneSessionState::Constructed;
    std::uint64_t m_worldId = 0;
    std::unique_ptr<game_core::RuntimeWorld> m_runtimeWorld;
    std::optional<scene::SceneInstance> m_sceneInstance;
};
} // namespace cue::runtime
