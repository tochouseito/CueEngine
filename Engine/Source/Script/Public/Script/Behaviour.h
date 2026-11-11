#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#include <Foundation/Result.h>
#include <Script/GameObject.h>
#include <ScriptData/ScriptData.h>

namespace cue
{
template <typename... Args> class ScriptEvent;

/// @brief World の時間を受け取る SDK 値。Timer や FPS 待機を Script に持たせない
struct ScriptUpdateContext final
{
    std::chrono::nanoseconds deltaTime{};
    std::uint64_t tick = 0;
};

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
    /// @brief 有効な実行を初めて開始する。途中失敗も stop で回収できる状態にする
    [[nodiscard]] virtual Result<void> start(GameObject &)
    {
        return Result<void>::success();
    }
    /// @brief World 更新の時間入力で処理する。失敗では Session を停止する
    [[nodiscard]] virtual Result<void> update(GameObject &, const ScriptUpdateContext &) = 0;
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

  private:
    friend class ScriptModule;
    template <typename... Args> friend class ScriptEvent;

    // 購読者が Callback を所有する。停止・失敗時には Module Adapter が破棄する
    struct EventSubscription
    {
        bool isConnected = true;
    };

    std::vector<std::shared_ptr<EventSubscription>> m_eventSubscriptions;
    bool m_acceptsSubscriptions = false;
    bool m_isActive = false;
};
} // namespace cue
