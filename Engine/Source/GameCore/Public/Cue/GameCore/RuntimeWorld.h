#pragma once

#include <Cue/Foundation/Result.h>
#include <Cue/GameCore/CommandBuffer.h>
#include <Cue/GameCore/World.h>
#include <Cue/Math/Transform.h>
#include <Cue/Schema/Types.h>

#include <memory>
#include <optional>
#include <string_view>
#include <thread>

namespace cue::game_core
{
/// @brief Headless Runtime World の明示的な寿命状態
enum class RuntimeWorldState
{
    Initializing,
    Running,
    Stopping,
    Shutdown,
    Failed
};

/// @brief ECS、Core Transform、Structural Safe Point を所有する Headless Runtime Session
/// @details create を呼び出した Thread を Owner とし、全公開 API と Destructor は同じ Thread からだけ呼び出す
/// @details Owner Thread 契約への違反は Data Race を防ぐため Debug／Development／Release の全構成で Fatal 終了する
/// @details RuntimeSystemRegistry の Start 後は全 System の Stop 完了まで終了と破棄を拒否する
class RuntimeWorld final
{
  public:
    /// @brief Factory だけが Runtime World Constructor へ渡せる生成権限
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
        friend class RuntimeWorld;

        /// @brief RuntimeWorld Factory だけに生成権限を発行する
        ConstructionKey() noexcept = default;
    };

    /// @brief 呼び出し Thread を Owner とする初期化前の Headless Runtime World 所有者を生成する
    /// @param a_identitySource Process 全体で共有し全 RuntimeWorld、発行済み EntityHandle、ComponentType
    /// より長く生存する発行元
    /// @param a_schemaRegistry RuntimeWorld より長く生存する Seal 済み Schema Registry
    /// @param a_transformTypeId Core Transform を識別する登録済み Schema Type
    /// @param a_assertContext RuntimeWorld より長く生存する非所有診断 Context
    [[nodiscard]] static std::unique_ptr<RuntimeWorld> create(WorldIdentitySource &a_identitySource,
                                                              const schema::SchemaRegistry &a_schemaRegistry,
                                                              schema::TypeId a_transformTypeId,
                                                              const AssertContext &a_assertContext) noexcept;

    /// @brief Factory を通らない Runtime World 生成を禁止する
    RuntimeWorld() = delete;
    /// @brief Runtime Session 所有権の複製を禁止する
    RuntimeWorld(const RuntimeWorld &) = delete;
    /// @brief Runtime Session 所有権の複製代入を禁止する
    RuntimeWorld &operator=(const RuntimeWorld &) = delete;
    /// @brief 所有する World Address を固定するため Move 構築を禁止する
    RuntimeWorld(RuntimeWorld &&) = delete;
    /// @brief 所有する World Address を固定するため Move 代入を禁止する
    RuntimeWorld &operator=(RuntimeWorld &&) = delete;
    /// @brief Owner Thread 上で未終了 Session を安全な逆順で終了する
    ~RuntimeWorld() noexcept;

    /// @brief Factory が保持する外部寿命と Transform Schema Type を固定する
    RuntimeWorld(ConstructionKey, WorldIdentitySource &a_identitySource, const schema::SchemaRegistry &a_schemaRegistry,
                 schema::TypeId a_transformTypeId, const AssertContext &a_assertContext) noexcept;

    /// @brief ECS World、Core Transform、Command Buffer を順に初期化する
    [[nodiscard]] Result<void> initialize() noexcept;
    /// @brief Command Buffer を一つの Structural Safe Point で適用する
    [[nodiscard]] Result<StructuralCommandReport> tick() noexcept;
    /// @brief 次の Tick Safe Point で終了する Stopping 状態へ移る
    [[nodiscard]] Result<void> request_stop() noexcept;
    /// @brief 構築済み要素を逆順解放して冪等に Shutdown する
    [[nodiscard]] Result<void> shutdown() noexcept;

    /// @brief 現在の Runtime World 寿命状態を返す
    [[nodiscard]] RuntimeWorldState state() const noexcept;
    /// @brief Running または Stopping 中の ECS World を非所有 Pointer で返す
    /// @details 返却 Pointer は shutdown、Stopping 中の次の tick、または RuntimeWorld 破棄で失効する
    [[nodiscard]] World *try_world() noexcept;
    /// @brief Running または Stopping 中の ECS World を Const 非所有 Pointer で返す
    /// @details 返却 Pointer は shutdown、Stopping 中の次の tick、または RuntimeWorld 破棄で失効する
    [[nodiscard]] const World *try_world() const noexcept;
    /// @brief Running または Stopping 中の Command Buffer を非所有 Pointer で返す
    /// @details 返却 Pointer は shutdown、Stopping 中の次の tick、または RuntimeWorld 破棄で失効する
    [[nodiscard]] StructuralCommandBuffer *try_command_buffer() noexcept;
    /// @brief 初期化済み Core Transform Component Token を非所有 Pointer で返す
    /// @details 返却 Pointer は shutdown、Stopping 中の次の tick、または RuntimeWorld 破棄で失効する
    [[nodiscard]] const ComponentType<math::Transform> *try_transform_type() const noexcept;

  private:
    friend class RuntimeSystemRegistry;

    /// @brief Runtime World API を生成 Thread へ限定して Data Race を防ぐ
    void assert_owner_thread() const noexcept;
    /// @brief Started から全 System 停止まで World 終了を固定する Registry Binding を取得する
    [[nodiscard]] Result<void> bind_system_registry(const RuntimeSystemRegistry &a_registry) noexcept;
    /// @brief 全 System 停止後に同じ Registry の World 終了固定 Binding を解放する
    void unbind_system_registry(const RuntimeSystemRegistry &a_registry) noexcept;
    /// @brief System Callback中にWorld Lifecycleを固定するLeaseを開始する
    void begin_system_callback_lease() noexcept;
    /// @brief System Callback完了後にWorld Lifecycle固定Leaseを終了する
    void end_system_callback_lease() noexcept;
    /// @brief System Callback 中の Safe Point または Lifecycle 操作を回復可能 Error として拒否する
    [[nodiscard]] Result<void> validate_callback_inactive() const noexcept;
    /// @brief System Callback 中または Registry Binding 中の World 終了操作を回復可能 Error として拒否する
    [[nodiscard]] Result<void> validate_termination_allowed() const noexcept;
    /// @brief 現在 State が Tick と Runtime Data 参照を許可するか返す
    [[nodiscard]] bool is_operational() const noexcept;
    /// @brief Command Buffer、Transform Token、World を構築の逆順で解放する
    void release_owned_state() noexcept;
    /// @brief Runtime World の不正な状態遷移 Error を生成する
    [[nodiscard]] Error make_state_error(std::string_view a_summary) const noexcept;
    /// @brief Allocation 失敗を Engine の Fatal 契約へ変換する
    [[noreturn]] void terminate_allocation() noexcept;
    /// @brief 予期しない例外を Engine の Fatal 契約へ変換する
    [[noreturn]] void terminate_exception() noexcept;

    WorldIdentitySource *m_identitySource;
    const schema::SchemaRegistry *m_schemaRegistry;
    schema::TypeId m_transformTypeId;
    const AssertContext *m_assertContext;
    std::thread::id m_ownerThread;
    RuntimeWorldState m_state = RuntimeWorldState::Initializing;
    const RuntimeSystemRegistry *m_boundSystemRegistry = nullptr;
    bool m_isSystemCallbackActive = false;
    std::unique_ptr<World> m_world;
    std::optional<ComponentType<math::Transform>> m_transformType;
    std::unique_ptr<StructuralCommandBuffer> m_commandBuffer;
};
} // namespace cue::game_core
