#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>

namespace cue
{
/// @brief 同じ計測区間の直近値と固定期間の分布を所有値で返す
struct TimingStatistics final
{
    std::size_t sampleCount = 0;
    std::chrono::nanoseconds lastDuration{};
    std::chrono::nanoseconds averageDuration{};
    std::chrono::nanoseconds p95Duration{};
    std::chrono::nanoseconds maxDuration{};
};

/// @brief 直近 120 Sample の容量を固定し、Frame ごとの Allocation を避ける
///
/// Owner Thread または外部 Lock で保護する。コピーした Snapshot の集計は Lock 外で行える
class TimingSamples final
{
  public:
    /// @brief 非負の経過時間を追加し、満杯なら最古の Sample を置き換える
    void add(std::chrono::nanoseconds a_duration) noexcept
    {
        m_last = (std::max)(a_duration, std::chrono::nanoseconds::zero());
        m_samples[m_next] = m_last;
        m_next = (m_next + 1) % m_samples.size();
        m_count = (std::min)(m_count + 1, m_samples.size());
    }

    /// @brief 集計済みでない固定容量の有効 Sample 数を返す
    [[nodiscard]] std::size_t sample_count() const noexcept
    {
        return m_count;
    }

    /// @brief Sample のコピーを整列し、平均・nearest-rank p95・最大を返す
    [[nodiscard]] TimingStatistics statistics() const noexcept
    {
        TimingStatistics result{};
        result.sampleCount = m_count;
        result.lastDuration = m_last;
        if (m_count == 0)
        {
            return result;
        }
        auto ordered = m_samples;
        // 商と余りを別に足し、長い休止時間でも総和の Overflow と浮動小数の丸めを避ける
        std::chrono::nanoseconds::rep quotient = 0;
        std::chrono::nanoseconds::rep remainder = 0;
        for (std::size_t index = 0; index < m_count; ++index)
        {
            quotient += ordered[index].count() / static_cast<std::chrono::nanoseconds::rep>(m_count);
            remainder += ordered[index].count() % static_cast<std::chrono::nanoseconds::rep>(m_count);
        }
        std::sort(ordered.begin(), ordered.begin() + m_count);
        result.averageDuration =
            std::chrono::nanoseconds(quotient + remainder / static_cast<std::chrono::nanoseconds::rep>(m_count));
        result.p95Duration = ordered[(95 * m_count + 99) / 100 - 1];
        result.maxDuration = ordered[m_count - 1];
        return result;
    }

  private:
    std::array<std::chrono::nanoseconds, 120> m_samples{};
    std::size_t m_next = 0;
    std::size_t m_count = 0;
    std::chrono::nanoseconds m_last{};
};
} // namespace cue
