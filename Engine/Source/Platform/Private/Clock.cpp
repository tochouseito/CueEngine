#include <Platform/Clock.h>

namespace cue
{
namespace
{
/// @brief 標準 Library の時計取得を Platform の実装へ閉じる
class SteadyClock final : public Clock
{
  public:
    /// @brief 時刻調整の影響を受けない共通時間軸の現在値を返す
    [[nodiscard]] std::chrono::steady_clock::time_point now() const noexcept override
    {
        // OS ごとの差は標準 Library の実装へ委譲し、利用側には単調時計だけを返す
        return std::chrono::steady_clock::now();
    }
};
} // namespace

Clock &get_default_clock() noexcept
{
    // 既定時計は差し替えず、単体利用の共通の時間軸として保持する
    static SteadyClock clock;
    return clock;
}

std::unique_ptr<Clock> create_clock()
{
    // Host が明示的に所有できる、同じ時間軸の時計を生成する
    return std::make_unique<SteadyClock>();
}
} // namespace cue
