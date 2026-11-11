#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <ECS/ECSManager.h>
#include <Foundation/ScopedFlag.h>
#include <Runtime/WorldHandle.h>
#include <Runtime/WorldSystem.h>

namespace cue
{
/// @brief ECS・Instance・世代付き Entity を所有する実行空間。生成 Thread を Owner とする
/// 全操作と破棄を Owner Thread で直列化する。Initialize / Update / Shutdown の再入は禁止
/// Handle は非所有。借用 Component / Span は同じ Pool の構造変更・確定・World 破棄で失効する
/// 例外・System / Deferred Callback の失敗は終端失敗。shutdown で回収し、新しい World を作る
class World final
{
  public:
    /// @brief create だけが生成できる構築 Key。make_unique による一意所有を維持する
    class ConstructionKey final
    {
      public:
        /// @brief Factory から Constructor へ Key を移す
        ConstructionKey(ConstructionKey &&) = default;

      private:
        friend class World;
        /// @brief 外部から World 識別子を指定して構築させない
        ConstructionKey() = default;
    };
    /// @brief Factory が発行した識別子と Owner を固定する。利用側は create を呼ぶ
    explicit World(ConstructionKey, std::uint64_t a_id);
    /// @brief Process 内で再利用しない World 識別子を確保し、Owner Thread に空の World を作る
    [[nodiscard]] static Result<std::unique_ptr<World>> create();
    /// @brief 未停止なら Owner Thread で System を停止し、全所有値を破棄する
    ~World();
    World(const World &) = delete;
    World &operator=(const World &) = delete;
    World(World &&) = delete;
    World &operator=(World &&) = delete;

    /// @brief Scene の識別文字列を所有する独立 Instance を作る。同じ Scene ID の複数生成を許可する
    /// File は読まない。System / Query 中の呼出しは拒否するため defer で予約する
    [[nodiscard]] Result<SceneInstanceHandle> create_scene_instance(std::string_view a_sceneId);
    /// @brief Instance の全 Entity を破棄する。更新・Query 中は非公開化して削除を予約する
    [[nodiscard]] Result<void> destroy_scene_instance(SceneInstanceHandle a_instance);
    /// @brief Instance 内で一意の Object ID と名前を所有する Entity を作る。名前の重複は許可する
    [[nodiscard]] Result<EntityHandle> create_entity(SceneInstanceHandle a_instance, std::string_view a_objectId,
                                                     std::string_view a_name);
    /// @brief Entity を削除する。更新・Query 中は Handle を検索から外し、実体の破棄を予約する
    [[nodiscard]] Result<void> destroy_entity(EntityHandle a_entity);
    /// @brief Instance 内の永続 Object ID から Entity を検索する。戻り値は非所有
    [[nodiscard]] Result<EntityHandle> find_by_id(SceneInstanceHandle a_instance, std::string_view a_objectId) const;
    /// @brief Instance 内の名前を完全一致で検索する。欠損・複数一致は失敗とする
    [[nodiscard]] Result<EntityHandle> find_by_name(SceneInstanceHandle a_instance, std::string_view a_name) const;
    /// @brief Instance の生存 Entity の Handle を所有配列へ複写する。配列は後の生存を保証しない
    [[nodiscard]] Result<std::vector<EntityHandle>> entities(SceneInstanceHandle a_instance) const;
    /// @brief 非 Active を含む全生存 Entity を所有配列へ複写する。System の寿命追跡に使う
    [[nodiscard]] Result<std::vector<EntityHandle>> entities() const;
    /// @brief 世代を検証して Entity の実行対象状態を返す。Component 自身の状態は含めない
    [[nodiscard]] Result<bool> is_active(EntityHandle a_entity) const;
    /// @brief 生存中の Entity が属する Scene Instance の世代付き Handle を返す
    [[nodiscard]] Result<SceneInstanceHandle> scene_instance(EntityHandle a_entity) const;
    /// @brief Owner Thread 上で World 識別・世代・生存を検証する
    [[nodiscard]] Result<bool> is_alive(EntityHandle a_entity) const;
    /// @brief Instance の World 識別・世代・生存を検証する
    [[nodiscard]] Result<bool> is_alive(SceneInstanceHandle a_instance) const;
    /// @brief 構造変更せず Entity の実行対象状態を変更する
    [[nodiscard]] Result<void> set_active(EntityHandle a_entity, bool a_active);

    /// @brief Component を追加する。単一型の再追加は既存値、Multi 型は新要素を返す
    /// System / Query 中は拒否するため defer を使う。失敗例外後は World を停止する
    template <ecs::ComponentType T> [[nodiscard]] Result<T *> add_component(EntityHandle a_entity)
    {
        auto access = check_structure();
        if (!access.has_value())
            return Result<T *>::failure(*access.try_error());
        if (!live(a_entity))
            return Result<T *>::failure(invalid_handle("World.add_component"));
        try
        {
            if (ecs::ComponentID<T>() >= 256)
                return Result<T *>::failure({ErrorCategory::InvalidArgument, "World.component_type_capacity"});
            ScopedFlag mutation(m_isMutating);
            return Result<T *>::success(m_ecs->add_component<T>(m_entities[a_entity.index].entity));
        }
        catch (...)
        {
            return fail_exception<T *>("World.add_component.exception");
        }
    }
    /// @brief Handle と型を検証し Component を借用する。Multi 型では先頭要素を返す
    template <ecs::ComponentType T> [[nodiscard]] Result<T *> get_component(EntityHandle a_entity)
    {
        auto access = check_access();
        if (!access.has_value())
            return Result<T *>::failure(*access.try_error());
        if (!live(a_entity))
            return Result<T *>::failure(invalid_handle("World.get_component"));
        try
        {
            if (ecs::ComponentID<T>() >= 256)
                return Result<T *>::failure({ErrorCategory::InvalidArgument, "World.component_type_capacity"});
            auto *component = m_ecs->get_component<T>(m_entities[a_entity.index].entity);
            if (!component)
                return Result<T *>::failure({ErrorCategory::InvalidArgument, "World.component_missing"});
            return Result<T *>::success(component);
        }
        catch (...)
        {
            return fail_exception<T *>("World.get_component.exception");
        }
    }
    /// @brief Multi 型の実体配列を借用する。構造変更後に Span を保持しない
    template <ecs::ComponentType T>
        requires ecs::IsMultiComponent<T>::value
    [[nodiscard]] Result<std::span<T>> get_components(EntityHandle a_entity)
    {
        auto component = get_component<T>(a_entity);
        if (!component.has_value())
            return Result<std::span<T>>::failure(*component.try_error());
        auto *storage = m_ecs->get_all_components<T>(m_entities[a_entity.index].entity);
        return Result<std::span<T>>::success(std::span<T>(*storage));
    }
    /// @brief Component を削除する。Multi 型は全要素、更新・Query 中は確定境界へ予約する
    template <ecs::ComponentType T> [[nodiscard]] Result<void> remove_component(EntityHandle a_entity)
    {
        auto access = check_access();
        if (!access.has_value())
            return access;
        if (!live(a_entity))
            return Result<void>::failure(invalid_handle("World.remove_component"));
        if (must_defer())
            return defer(
                [a_entity](World &a_world)
                {
                    if (!a_world.live(a_entity))
                        return Result<void>::success();
                    return a_world.remove_component<T>(a_entity);
                });
        auto structure = check_structure();
        if (!structure.has_value())
            return structure;
        try
        {
            if (ecs::ComponentID<T>() >= 256)
                return Result<void>::failure({ErrorCategory::InvalidArgument, "World.component_type_capacity"});
            ScopedFlag mutation(m_isMutating);
            if constexpr (ecs::IsMultiComponent<T>::value)
                m_ecs->remove_all_components<T>(m_entities[a_entity.index].entity);
            else
                m_ecs->remove_component<T>(m_entities[a_entity.index].entity);
            return Result<void>::success();
        }
        catch (...)
        {
            return fail_exception<void>("World.remove_component.exception");
        }
    }

    /// @brief Active な単一 Component 群を借用し、各 Entity を一度ずつ処理する
    /// Callback は Result<void>(EntityHandle, T&...)。参照は Callback 中だけ有効、構造変更は defer を使う
    template <ecs::ComponentType... T, typename F>
        requires(sizeof...(T) > 0 && (!ecs::IsMultiComponent<T>::value && ...) &&
                 std::is_invocable_r_v<Result<void>, F &, EntityHandle, T &...>)
    [[nodiscard]] Result<void> for_each(F &&a_callback)
    {
        auto access = check_access();
        if (!access.has_value())
            return access;
        try
        {
            if (((ecs::ComponentID<T>() >= 256) || ...))
                return Result<void>::failure({ErrorCategory::InvalidArgument, "World.component_type_capacity"});
            ScopedFlag iteration(m_isIterating);
            for (std::size_t index = 0; index < m_entities.size(); ++index)
            {
                const auto &entry = m_entities[index];
                if (entry.state != SlotState::Live || !m_ecs->is_entity_active(entry.entity))
                    continue;
                const auto components = std::tuple<T *...>{m_ecs->get_component<T>(entry.entity)...};
                const bool active = std::apply([](auto *...a_values)
                                               { return ((a_values && a_values->is_active()) && ...); }, components);
                if (!active)
                    continue;
                auto result =
                    std::apply([&](auto *...a_values)
                               { return std::invoke(a_callback, entity_handle(index), *a_values...); }, components);
                if (!result.has_value())
                {
                    record_failure(*result.try_error());
                    return result;
                }
                if (m_lifecycle == Lifecycle::Failed && m_phase != Phase::Stopping)
                    return Result<void>::failure(*m_failure);
            }
            return Result<void>::success();
        }
        catch (...)
        {
            return fail_exception<void>("World.for_each.exception");
        }
    }

    /// @brief System を所有し、優先度の昇順・同順位は登録順で実行する。初期化前だけ登録可能
    [[nodiscard]] Result<void> add_system(std::unique_ptr<WorldSystem> a_system, std::int32_t a_priority = 0);
    /// @brief 所有する Command を次の確定境界で実行する。捕捉先は実行または World 破棄まで生存させる
    /// 生成・追加は Command 内で行う。1 回の確定で最大 4096 Command、超過・Callback 失敗は終端失敗
    [[nodiscard]] Result<void> defer(std::function<Result<void>(World &)> a_command);
    /// @brief 初期 Entity と System を準備して一度だけ実行可能にする。失敗時は shutdown で回収する
    [[nodiscard]] Result<void> initialize();
    /// @brief System 実行後に構造変更と Transform を確定する。負の DeltaTime と再入は拒否する
    [[nodiscard]] Result<void> update(std::chrono::nanoseconds a_deltaTime);
    /// @brief 初期化を開始した System を逆順で停止し、失敗があっても全所有値を回収する
    /// Owner Thread から複数回呼べる。最初の実行失敗または停止失敗を返す
    [[nodiscard]] Result<void> shutdown();

  private:
    enum class SlotState
    {
        Free,
        Live,
        Retiring,
        Exhausted
    };
    enum class Lifecycle
    {
        Created,
        Running,
        Failed,
        Stopped
    };
    enum class Phase
    {
        Idle,
        Initializing,
        Updating,
        Committing,
        Stopping
    };
    struct EntitySlot final
    {
        std::uint64_t generation = 1;
        SlotState state = SlotState::Free;
        ecs::Entity entity{};
        SceneInstanceHandle instance{};
        std::string objectId;
        std::string name;
    };
    struct InstanceSlot final
    {
        std::uint64_t generation = 1;
        SlotState state = SlotState::Free;
        std::string sceneId;
    };
    class SystemAdapter;

    /// @brief Thread と生存状態を検証する。停止 Callback 中は読取を許可する
    [[nodiscard]] Result<void> check_access() const;
    /// @brief 借用走査・Lifecycle Callback・再入中の直接構造変更を拒否する
    [[nodiscard]] Result<void> check_structure() const;
    /// @brief 更新または Query によって実体の変更を遅延すべきか返す
    [[nodiscard]] bool must_defer() const noexcept;
    /// @brief Entity の World / 世代 / Instance / 生存を検証する。Owner Thread 専用
    [[nodiscard]] bool live(EntityHandle a_entity) const noexcept;
    /// @brief Instance の World / 世代 / 生存を検証する。Owner Thread 専用
    [[nodiscard]] bool live(SceneInstanceHandle a_instance) const noexcept;
    /// @brief Slot の現在世代を非所有 Handle にする
    [[nodiscard]] EntityHandle entity_handle(std::size_t a_index) const noexcept;
    /// @brief 同じ Instance の公開 Entity を検索する
    [[nodiscard]] Result<EntityHandle> find(SceneInstanceHandle a_instance, std::string_view a_key,
                                            bool a_byName) const;
    /// @brief 世代を照合して物理 Entity を破棄する。削除済みの予約は無処理とする
    void erase_entity(EntityHandle a_entity);
    /// @brief Instance に残る Entity を破棄して世代を進める
    void erase_instance(SceneInstanceHandle a_instance);
    /// @brief 追加された予約も含めて上限内で実行し、Transform を同期する
    [[nodiscard]] Result<void> commit();
    /// @brief Scene の全生存 Transform の Dirty 行列を同期する
    [[nodiscard]] Result<void> synchronize_transforms();
    /// @brief 最初の終端失敗を保持する。Owner Thread 専用
    void record_failure(Error a_error);
    /// @brief 無効 Handle の失敗を構築する
    [[nodiscard]] static Error invalid_handle(const char *a_operation);
    /// @brief 予期しない例外を終端失敗へ変換する。部分更新の再試行は許可しない
    template <typename T> [[nodiscard]] Result<T> fail_exception(const char *a_operation)
    {
        record_failure({ErrorCategory::Fatal, a_operation});
        return Result<T>::failure(*m_failure);
    }

    const std::uint64_t m_id;
    const std::thread::id m_owner;
    std::unique_ptr<ecs::ECSManager> m_ecs;
    std::vector<EntitySlot> m_entities;
    std::vector<InstanceSlot> m_instances;
    std::vector<SystemAdapter *> m_systems;
    std::vector<std::function<Result<void>(World &)>> m_commands;
    Lifecycle m_lifecycle = Lifecycle::Created;
    Phase m_phase = Phase::Idle;
    bool m_isIterating = false;
    bool m_isMutating = false;
    WorldUpdateContext m_context{};
    std::optional<Error> m_failure;
};
} // namespace cue
