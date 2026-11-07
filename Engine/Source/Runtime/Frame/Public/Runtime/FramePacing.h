#pragma once

#include <algorithm>
#include <chrono>
#include <functional>
#include <optional>
#include <stop_token>

#include <Foundation/Result.h>

namespace cue
{
/// @brief Platform 型を含めず周期の同期方針を指定する
enum class FrameSyncMode
{
    FixedPeriod,
    PresentationReady,
    DisplayPrediction,
};

enum class FrameSyncFallback
{
    None,
    PresentationUnavailable,
    DisplayTimingUnavailable,
};

/// @brief Host が所有する表示機能を Controller の停止まで借用する
struct FramePacingSource final
{
    // Render は停止可能に待つ。単一 Thread は非待機判定で false を返し Pump に戻る
    std::function<Result<bool>(std::stop_token, bool)> waitForPresentation;
    // Controller の Clock と同じ時間軸の次の表示候補。未対応 / 不連続 / 古い統計は nullopt
    std::function<std::optional<std::chrono::steady_clock::time_point>()> nextDisplayTime;
    // 設定変更で表示待機が無効になった場合、Callback を残しても実効方針へ反映する
    std::function<bool()> isPresentationAvailable;
};

/// @brief 要求値とは別に実際の同期方針と待機期限を返す
struct FramePacingPlan final
{
    std::chrono::steady_clock::time_point deadline;
    FrameSyncMode effectiveMode = FrameSyncMode::FixedPeriod;
    FrameSyncFallback fallback = FrameSyncFallback::None;
    bool waitsForPresentation = false;
};

/// @brief 表示統計が利用できないときは固定周期へ戻し、外部の FPS 待機を作らない
[[nodiscard]] inline FramePacingPlan plan_frame_cycle(
    FrameSyncMode a_requested, std::chrono::steady_clock::time_point a_fixedDeadline,
    std::chrono::steady_clock::time_point a_now, std::optional<std::chrono::steady_clock::time_point> a_nextDisplay,
    std::chrono::nanoseconds a_slack, bool a_hasPresentation)
{
    // FPS 上限の期限をすべての同期方式に共通の下限とする
    FramePacingPlan plan{a_fixedDeadline};
    // 固定周期では表示統計や Waitable の状態を使わない
    if (a_requested == FrameSyncMode::FixedPeriod)
    {
        return plan;
    }
    // 表示枠を待てない環境では固定周期へ戻す
    if (!a_hasPresentation)
    {
        plan.fallback = FrameSyncFallback::PresentationUnavailable;
        return plan;
    }
    if (a_requested == FrameSyncMode::DisplayPrediction)
    {
        // 取得できない表示時刻や遠すぎる予測値を採用しない
        if (!a_nextDisplay || *a_nextDisplay <= a_now || *a_nextDisplay - a_now > std::chrono::milliseconds(250))
        {
            plan.fallback = FrameSyncFallback::DisplayTimingUnavailable;
            return plan;
        }
        // 表示直前の余裕を確保しつつ、FPS 上限より早く開始しない
        plan.deadline = (std::max)(a_fixedDeadline, *a_nextDisplay - a_slack);
    }
    // 利用可能な表示待機だけを実効計画へ反映する
    plan.effectiveMode = a_requested;
    plan.waitsForPresentation = true;
    return plan;
}
} // namespace cue
