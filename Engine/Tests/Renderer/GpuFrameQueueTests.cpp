#include <RHI/GpuFrameQueue.h>

namespace
{
/// @brief 実 GPU を使わず未完了、完了、Device Lost の分岐を固定する
class Completion final : public cue::ICommandCompletion
{
  public:
    bool isDone = false;
    bool hasFailed = false;
    /// @brief 集約先の Queue 種類を返す
    cue::QueueType type() const noexcept override
    {
        return cue::QueueType::Graphics;
    }
    /// @brief Test の完了状態を返す
    bool is_complete() const noexcept override
    {
        return isDone;
    }
    /// @brief Device Lost を未完了と混同させない
    cue::Result<bool> completion_status() const override
    {
        return hasFailed ? cue::Result<bool>::failure({cue::ErrorCategory::PlatformFailure, "Test.device_lost"})
                         : cue::Result<bool>::success(isDone);
    }
    /// @brief この検証では無期限 wait を使わない
    cue::Result<void> wait() override
    {
        return cue::Result<void>::failure({cue::ErrorCategory::InvalidState, "Test.unexpected_wait"});
    }
};
} // namespace

/// @brief 上限で待ち、取消時は提出せず、Device Lost は Result で伝える
int main()
{
    cue::GpuFrameQueue queue(1);
    auto first = std::make_shared<Completion>();
    if (!queue.submitted(first).has_value() || queue.pending() != 1)
        return 1;
    bool waited = false;
    auto available = queue.wait_for_slot(
        [&]()
        {
            waited = true;
            first->isDone = true;
        },
        []() { return false; });
    if (!available.has_value() || !*available.try_value() || !waited || queue.pending() != 0)
        return 2;
    auto second = std::make_shared<Completion>();
    if (!queue.submitted(second).has_value())
        return 3;
    auto cancelled = queue.wait_for_slot([]() {}, []() { return true; });
    if (!cancelled.has_value() || *cancelled.try_value() || queue.pending() != 1)
        return 4;
    second->hasFailed = true;
    auto failed = queue.wait_for_slot([]() {}, []() { return false; });
    if (failed.has_value() || failed.try_error()->operation != "Test.device_lost")
        return 5;
    cue::GpuFrameQueue multiple(3);
    auto complete = std::make_shared<Completion>();
    auto pending = std::make_shared<Completion>();
    auto error = std::make_shared<Completion>();
    complete->isDone = true;
    error->hasFailed = true;
    if (!multiple.submitted(complete).has_value() || !multiple.submitted(pending).has_value() ||
        !multiple.submitted(error).has_value())
        return 6;
    auto rejected = multiple.wait_for_slot([]() {}, []() { return false; });
    if (rejected.has_value() || multiple.pending() != 3)
        return 7;
    error->hasFailed = false;
    auto retry = multiple.wait_for_slot([]() {}, []() { return false; });
    if (!retry.has_value() || !*retry.try_value() || multiple.pending() != 2)
        return 8;
    return 0;
}
