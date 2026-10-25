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
/// Profiler の系列実装が所有し、その Mutex で保護する。単体利用では外部排他が必要
class TimingSamples final
{
  public:
    /// @brief 非負の経過時間を追加し、満杯なら最古の Sample を置き換える
    void add(std::chrono::nanoseconds a_duration) noexcept
    {
        m_last = (std::max)(a_duration, std::chrono::nanoseconds::zero());
        if (m_count == m_samples.size())
        {
            const auto old = m_samples[m_next];
            const auto found = std::lower_bound(m_ordered.begin(), m_ordered.end(), old);
            std::move(found + 1, m_ordered.end(), found);
            m_sumQuotient -= old.count() / 120;
            m_sumRemainder -= old.count() % 120;
        }
        else
        {
            ++m_count;
        }
        const auto end = m_ordered.begin() + m_count - 1;
        const auto position = std::lower_bound(m_ordered.begin(), end, m_last);
        std::move_backward(position, end, end + 1);
        *position = m_last;
        m_sumQuotient += m_last.count() / 120;
        m_sumRemainder += m_last.count() % 120;
        m_samples[m_next] = m_last;
        m_next = (m_next + 1) % m_samples.size();
    }

    /// @brief 集計済みでない固定容量の有効 Sample 数を返す
    [[nodiscard]] std::size_t sample_count() const noexcept
    {
        return m_count;
    }

    /// @brief 全標本から正確に維持した平均・nearest-rank p95・最大を定数時間で返す
    [[nodiscard]] TimingStatistics statistics() const noexcept
    {
        TimingStatistics result{};
        result.sampleCount = m_count;
        result.lastDuration = m_last;
        if (m_count == 0)
        {
            return result;
        }
        // 総和を 120 の商と余りで保持し、Warmup 中も 64 bit を溢れさせず整数平均を復元する
        const auto count = static_cast<std::chrono::nanoseconds::rep>(m_count);
        result.averageDuration = std::chrono::nanoseconds((m_sumQuotient / count) * 120 +
                                                          ((m_sumQuotient % count) * 120 + m_sumRemainder) / count);
        result.p95Duration = m_ordered[(95 * m_count + 99) / 100 - 1];
        result.maxDuration = m_ordered[m_count - 1];
        return result;
    }

  private:
    std::array<std::chrono::nanoseconds, 120> m_samples{};
    std::array<std::chrono::nanoseconds, 120> m_ordered{};
    std::chrono::nanoseconds::rep m_sumQuotient = 0;
    std::chrono::nanoseconds::rep m_sumRemainder = 0;
    std::size_t m_next = 0;
    std::size_t m_count = 0;
    std::chrono::nanoseconds m_last{};
};
} // namespace cue
