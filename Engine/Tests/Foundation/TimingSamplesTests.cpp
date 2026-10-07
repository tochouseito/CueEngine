#include <Foundation/ScopedFlag.h>
#include <Foundation/TimingSamples.h>

#include <chrono>
#include <limits>

namespace
{
/// @brief 固定容量の上書き、nearest-rank p95 と整数集計の境界を検証する
int run_tests()
{
    cue::TimingSamples samples;
    if (samples.statistics().sampleCount != 0)
    {
        return 1;
    }
    // 1..120 の分布は平均 60、p95 114。最古を上書きした後も直近 120 件だけを集計する
    for (int index = 1; index <= 120; ++index)
    {
        samples.add(std::chrono::nanoseconds(index));
    }
    auto stats = samples.statistics();
    if (stats.sampleCount != 120 || stats.averageDuration.count() != 60 || stats.p95Duration.count() != 114 ||
        stats.maxDuration.count() != 120 || stats.lastDuration.count() != 120)
    {
        return 2;
    }
    samples.add(std::chrono::nanoseconds(121));
    stats = samples.statistics();
    if (stats.sampleCount != 120 || stats.averageDuration.count() != 61 || stats.p95Duration.count() != 115)
    {
        return 3;
    }
    // 不正な負の経過時間はゼロとし、最大値の総和も Overflow させない
    cue::TimingSamples extremes;
    extremes.add(std::chrono::nanoseconds(-1));
    if (extremes.statistics().lastDuration.count() != 0)
    {
        return 4;
    }
    for (int index = 0; index < 120; ++index)
    {
        extremes.add(std::chrono::nanoseconds::max());
    }
    if (extremes.statistics().averageDuration != std::chrono::nanoseconds::max())
    {
        return 5;
    }
    // 入れ子で使っても外側の状態を復元し、元が true の場合も保持する
    bool isActive = false;
    {
        cue::ScopedFlag outer(isActive);
        {
            cue::ScopedFlag inner(isActive);
        }
        if (!isActive)
        {
            return 6;
        }
    }
    if (isActive)
    {
        return 7;
    }
    isActive = true;
    {
        cue::ScopedFlag guard(isActive);
    }
    return isActive ? 0 : 8;
}
} // namespace

/// @brief 計測の分布集計と共通 Scope Guard の契約を検証する
int main()
{
    return run_tests();
}
