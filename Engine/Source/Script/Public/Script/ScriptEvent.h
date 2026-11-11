#pragma once

#include <algorithm>
#include <cstddef>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

#include <Script/Behaviour.h>

namespace cue
{
/// @brief Game Module 内で Behaviour の出来事を同期通知する。World の Owner Thread 専用
/// 発行者が Event を所有し、購読者が Callback を所有する。両者の停止・破棄時に購読は失効する
template <typename... Args> class ScriptEvent final
{
    static_assert((std::is_object_v<Args> && ...));

  public:
    using callbackType = std::function<Result<void>(const Args &...)>;

    /// @brief Behaviour の Member として作成し、同じ World 内の購読に使う
    ScriptEvent() = default;
    /// @brief 発行者の破棄時に、購読者が所有する Callback と Capture も解放する
    ~ScriptEvent()
    {
        for (const auto &weak : m_listeners)
        {
            if (const auto listener = weak.lock())
            {
                listener->isConnected = false;
                listener->callback = {};
            }
        }
    }
    /// @brief 発行者から購読を複製しない
    ScriptEvent(const ScriptEvent &) = delete;
    /// @brief 発行者から購読を複製しない
    ScriptEvent &operator=(const ScriptEvent &) = delete;

    /// @brief start 以降の購読者に Callback を登録する。Callback は失敗を発行側へ返す
    /// 発行中の新規登録は次の発行から有効。GameObject を Callback に保持してはならない
    [[nodiscard]] Result<void> subscribe(Behaviour &a_subscriber, callbackType a_callback)
    {
        if (!a_subscriber.m_acceptsSubscriptions || !a_callback)
        {
            return Result<void>::failure({ErrorCategory::InvalidState, "ScriptEvent.subscribe"});
        }
        auto listener = std::make_shared<Listener>();
        listener->subscriber = &a_subscriber;
        listener->callback = std::move(a_callback);
        std::erase_if(a_subscriber.m_eventSubscriptions,
                      [](const auto &a_subscription) { return !a_subscription->isConnected; });
        a_subscriber.m_eventSubscriptions.push_back(listener);
        try
        {
            m_listeners.push_back(listener);
        }
        catch (...)
        {
            a_subscriber.m_eventSubscriptions.pop_back();
            throw;
        }
        return Result<void>::success();
    }

    /// @brief 生存中の購読者を登録順に呼ぶ。同じ Event の再発行を拒否し、最初の失敗で終了する
    [[nodiscard]] Result<void> publish(const Args &...a_args)
    {
        if (m_isPublishing)
        {
            return Result<void>::failure({ErrorCategory::InvalidState, "ScriptEvent.publish.reentrant"});
        }
        std::erase_if(m_listeners, [](const auto &a_listener) { return a_listener.expired(); });
        PublishScope scope(m_isPublishing);
        const auto count = m_listeners.size();
        for (std::size_t index = 0; index < count; ++index)
        {
            if (const auto listener = m_listeners[index].lock(); listener && listener->subscriber->m_isActive)
            {
                auto result = listener->subscriber->invoke_event([&] { return listener->callback(a_args...); });
                if (!result.has_value())
                {
                    return result;
                }
            }
        }
        return Result<void>::success();
    }

  private:
    struct Listener final : Behaviour::EventSubscription
    {
        Behaviour *subscriber = nullptr;
        callbackType callback;
    };

    class PublishScope final
    {
      public:
        /// @brief 発行中の再入を検出できるようにする
        explicit PublishScope(bool &a_isPublishing) noexcept : m_isPublishing(a_isPublishing)
        {
            m_isPublishing = true;
        }
        /// @brief 例外経路でも発行を再開できる状態に戻す
        ~PublishScope() { m_isPublishing = false; }

      private:
        bool &m_isPublishing;
    };

    std::vector<std::weak_ptr<Listener>> m_listeners;
    bool m_isPublishing = false;
};
} // namespace cue
