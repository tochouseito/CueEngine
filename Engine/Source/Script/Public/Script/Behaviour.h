#pragma once

#include <chrono>
#include <concepts>
#include <cstdint>
#include <memory>
#include <string_view>
#include <typeinfo>
#include <utility>
#include <vector>

#include <Foundation/Result.h>
#include <Script/GameObject.h>
#include <Script/ScriptTransform.h>
#include <ScriptData/ScriptData.h>

namespace cue
{
template <typename... Args> class ScriptEvent;

/// @brief ゲーム側で継承する実行単位。生成・破棄は Game Module、呼出順は ScriptSystem が管理する
/// Callback は World の Owner Thread 上で直列実行する。GameObject の借用を Member に保持しない
class Behaviour
{
  public:
    /// @brief Module が生成した Instance にだけ購読を紐付ける
    Behaviour() = default;
    /// @brief Instance の購読を複製しない
    Behaviour(const Behaviour &) = delete;
    /// @brief Instance の購読を複製しない
    Behaviour &operator=(const Behaviour &) = delete;
    /// @brief Game Module 側で派生 Instance を破棄する
    virtual ~Behaviour() = default;
    /// @brief 最初の更新より前に初期化する。失敗後は stop で部分初期化を回収する
    virtual void start()
    {
    }
    /// @brief 所属 Entity と時間を基底クラスから取得して更新する。未実装は Module 境界で失敗にする
    /// C++ 例外と暗黙 Transform 操作の失敗は Module が回収し、Session へ通知する
    virtual void update()
    {
        throw Error{ErrorCategory::InvalidState, "Behaviour.update_missing"};
    }
    /// @brief 開始した Instance を一度だけ停止する。部分初期化の回収も担当する
    [[nodiscard]] virtual Result<void> stop(GameObject &)
    {
        return Result<void>::success();
    }
    /// @brief 登録済みの設定値を受け取る。既定は公開 Property のない Behaviour として拒否する
    [[nodiscard]] virtual Result<void> set_property(std::string_view, const ScriptPropertyValue &)
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "Behaviour.property_missing"});
    }
  protected:
    /// @brief 自分の Entity を操作する非所有の窓口を返す。Owner Thread の現在の Callback 内だけで使う
    /// Callback 外では取得を拒否する。取得に成功しても Entity の生存は is_alive() で確認する
    [[nodiscard]] Result<GameObject> self() const
    {
        if (!m_callback)
        {
            return Result<GameObject>::failure({ErrorCategory::InvalidState, "Behaviour.callback_context"});
        }
        return Result<GameObject>::success(m_callback->object);
    }
    /// @brief 現在の Script Callback の時間を返す。更新以外はゼロ
    [[nodiscard]] std::chrono::nanoseconds delta_time() const noexcept
    {
        return m_callback ? m_callback->update.deltaTime : std::chrono::nanoseconds{};
    }
    /// @brief 現在の更新時間を秒単位の float で返す。更新時間がないときは 0
    [[nodiscard]] float delta_time_seconds() const noexcept
    {
        return std::chrono::duration<float>(delta_time()).count();
    }
    /// @brief 現在の World 更新番号を返す。開始・停止など更新番号のない Callback では 0
    [[nodiscard]] std::uint64_t update_tick() const noexcept
    {
        return m_callback ? m_callback->update.tick : 0;
    }
    /// @brief 自分の Component を Callback 中に所有値として取得する
    template <typename T> [[nodiscard]] Result<T> get_component() const
    {
        if (!m_callback)
        {
            return Result<T>::failure({ErrorCategory::InvalidState, "Behaviour.callback_context"});
        }
        return m_callback->object.get_component<T>();
    }
    /// @brief 自分と同じ Scene Instance 内の Object を Callback 中だけ借用する
    [[nodiscard]] Result<GameObject> find_by_name(std::string_view a_name) const
    {
        if (!m_callback)
        {
            return Result<GameObject>::failure({ErrorCategory::InvalidState, "Behaviour.callback_context"});
        }
        return m_callback->object.find_by_name(a_name);
    }
    /// @brief 同じ Scene Instance の Object 名で起動済みの T を解決する
    /// Owner Thread の現在の Callback 中だけ利用する。同じ型の割当が複数ある場合は ID を指定する
    template <std::derived_from<Behaviour> T>
    [[nodiscard]] GameObject::ScriptReference<T> find_script_by_name(std::string_view a_name,
                                                                    std::string_view a_instanceId = {}) const
    {
        auto object = find_by_name(a_name);
        if (!object.has_value())
        {
            return GameObject::ScriptReference<T>(std::move(*object.try_error()));
        }
        const auto &target = *object.try_value();
        auto script = target.find_script(typeid(T), a_instanceId);
        if (!script.has_value())
        {
            return GameObject::ScriptReference<T>(std::move(*script.try_error()));
        }
        return GameObject::ScriptReference<T>(target, a_instanceId);
    }

    ScriptTransform transform{*this};

  private:
    friend class GameObject::CallbackScope;
    friend class ScriptModule;
    friend class ScriptTransform;
    template <typename... Args> friend class ScriptEvent;

    /// @brief 発行側の時間を継承し、購読側の Entity で同期通知する
    template <typename Callback> [[nodiscard]] Result<void> invoke_event(Callback &&a_callback)
    {
        const auto *caller = m_scriptAccess ? m_scriptAccess->callback : nullptr;
        if (!caller || caller->object.handle().world != m_entity.world ||
            *m_scriptAccess->callDepth >= GameObject::k_maxCallDepth)
        {
            return Result<void>::failure({ErrorCategory::InvalidState, "ScriptEvent.callback_context"});
        }
        const GameObject object(caller->object.m_api, m_entity, m_scriptAccess);
        GameObject::CallScope depth(*m_scriptAccess->callDepth);
        GameObject::CallbackScope callback(*this, object, caller->update);
        return callback.finish(a_callback());
    }

    // 購読者が Callback を所有する。停止・失敗時には Module Adapter が破棄する
    struct EventSubscription
    {
        bool isConnected = true;
    };

    std::vector<std::shared_ptr<EventSubscription>> m_eventSubscriptions;
    GameObject::ScriptAccess *m_scriptAccess = nullptr;
    GameObject::CallbackScope *m_callback = nullptr;
    CueScriptEntity m_entity{};
    bool m_acceptsSubscriptions = false;
    bool m_isActive = false;
};
} // namespace cue
