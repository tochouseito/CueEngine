#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include <Profiler/Profiler.h>
#include <Profiler/TraceBuffer.h>

namespace
{
/// @brief 移行前の直近全標本 Sort を基準に、容量上書きと正確な分位値を比較する
int test_statistics()
{
    cue::Profiler profiler;
    auto metric = profiler.create_metric("Test.Statistics");
    std::vector<std::chrono::nanoseconds> reference;
    for (int index = 0; index < 1000; ++index)
    {
        const auto value = std::chrono::nanoseconds((index * 19) % 127);
        metric.add(value);
        if (reference.size() == 120)
        {
            reference.erase(reference.begin());
        }
        reference.push_back(value);
        auto sorted = reference;
        std::sort(sorted.begin(), sorted.end());
        std::int64_t total = 0;
        for (const auto item : reference)
        {
            total += item.count();
        }
        const auto result = metric.statistics();
        if (result.sampleCount != reference.size() || result.lastDuration != value ||
            result.averageDuration.count() != total / static_cast<std::int64_t>(reference.size()) ||
            result.p95Duration != sorted[(95 * sorted.size() + 99) / 100 - 1] || result.maxDuration != sorted.back())
        {
            return 1;
        }
    }
    metric.reset();
    if (metric.statistics().sampleCount || metric.sample_count())
    {
        return 2;
    }
    metric.add(std::chrono::nanoseconds(-1));
    if (metric.statistics().sampleCount != 1 || metric.statistics().lastDuration.count() != 0)
    {
        return 3;
    }
    return 0;
}

/// @brief 同名の別系列、共有 Handle、Profiler 破棄後の寿命、停止済み系列の再利用を検証する
int test_lifetime()
{
    cue::ProfileMetric empty;
    empty.add(std::chrono::nanoseconds(1));
    empty.reset();
    if (empty.is_valid() || empty.sample_count() || empty.statistics().sampleCount)
    {
        return 4;
    }
    cue::ProfileMetric retained;
    {
        cue::Profiler profiler;
        auto first = profiler.create_metric("SameName");
        auto second = profiler.create_metric("SameName");
        first.add(std::chrono::nanoseconds(11));
        second.add(std::chrono::nanoseconds(22));
        auto snapshot = profiler.snapshot();
        if (!snapshot.has_value() || snapshot.try_value()->metrics.size() != 2 ||
            snapshot.try_value()->metrics[0].id == snapshot.try_value()->metrics[1].id ||
            snapshot.try_value()->metrics[0].statistics.lastDuration.count() != 11 ||
            snapshot.try_value()->metrics[1].statistics.lastDuration.count() != 22)
        {
            return 5;
        }
        retained = first;
        first = {};
        second = {};
        snapshot = profiler.snapshot();
        if (!snapshot.has_value() || snapshot.try_value()->metrics.size() != 2 ||
            snapshot.try_value()->metrics[1].statistics.lastDuration.count() != 22)
        {
            return 6;
        }
        retained.add(std::chrono::nanoseconds(33));
        if (retained.sample_count() != 2)
        {
            return 7;
        }
        // 最終結果は停止後も残し、次の同名 Producer では使用中の系列を壊さず再利用する
        const auto retiredId = snapshot.try_value()->metrics[1].id;
        auto replacement = profiler.create_metric("SameName");
        snapshot = profiler.snapshot();
        if (!snapshot.has_value() || snapshot.try_value()->metrics.size() != 2 ||
            snapshot.try_value()->metrics[1].id == retiredId || replacement.sample_count() != 0 ||
            retained.statistics().lastDuration.count() != 33)
        {
            return 10;
        }
    }
    retained.add(std::chrono::nanoseconds(44));
    return retained.statistics().lastDuration.count() == 44 ? 0 : 8;
}

/// @brief 同時登録と複数 Producer の記録中も Snapshot が完全な所有値として成立するか確認する
int test_concurrency()
{
    cue::Profiler profiler;
    const auto shared = profiler.create_metric("Shared");
    std::atomic<bool> start = false;
    std::atomic<unsigned> completed = 0;
    std::array<std::thread, 4> workers;
    for (std::size_t index = 0; index < workers.size(); ++index)
    {
        workers[index] = std::thread(
            [&, index]()
            {
                while (!start.load(std::memory_order_acquire))
                {
                    std::this_thread::yield();
                }
                auto local = profiler.create_metric("Worker");
                for (int sample = 0; sample < 2000; ++sample)
                {
                    const auto value = std::chrono::nanoseconds(index + 1);
                    shared.add(value);
                    local.add(value);
                }
                ++completed;
            });
    }
    start.store(true, std::memory_order_release);
    bool wasValid = true;
    do
    {
        auto snapshot = profiler.snapshot();
        wasValid &= snapshot.has_value();
        if (snapshot.has_value())
        {
            for (const auto &item : snapshot.try_value()->metrics)
            {
                const auto &stats = item.statistics;
                wasValid &= stats.sampleCount <= 120 && stats.averageDuration >= std::chrono::nanoseconds::zero() &&
                            stats.averageDuration <= stats.maxDuration && stats.p95Duration <= stats.maxDuration;
            }
        }
    } while (completed.load() != workers.size());
    for (auto &worker : workers)
    {
        worker.join();
    }
    const auto stats = shared.statistics();
    return wasValid && stats.sampleCount == 120 && stats.maxDuration.count() <= 4 ? 0 : 9;
}

/// @brief 非循環の欠落、循環の順序、非可動 Slot と移動後状態を検証する
int test_trace_buffer()
{
    cue::TraceBuffer<int> bounded(2);
    if (!bounded.append(11) || !bounded.append(22) || bounded.append(33) || bounded.accepted_count() != 2 ||
        bounded.dropped_count() != 1 || bounded.overwritten_count() || bounded.at_chronological(0) != 11)
    {
        return 11;
    }
    bounded.set_circular(true);
    for (int index = 0; index < 5; ++index)
    {
        if (!bounded.append(index))
        {
            return 12;
        }
    }
    if (bounded.accepted_count() != 5 || bounded.retained_count() != 2 || bounded.overwritten_count() != 3 ||
        bounded.first_sequence() != 3 || bounded.at_chronological(0) != 3 || bounded.at_chronological(1) != 4)
    {
        return 13;
    }
    auto moved = std::move(bounded);
    if (moved.at_chronological(0) != 3 || bounded.size() != 0 || bounded.append(99))
    {
        return 14;
    }
    moved.resize(3);
    if (moved.retained_count() || moved.capacity() != 3)
    {
        return 15;
    }
    // Atomic の非可動 Member を持つ Slot は、Adapter が独自の排他で直接使う
    struct Slot final
    {
        std::atomic_flag writing = ATOMIC_FLAG_INIT;
        std::uint64_t sequence = 0;
    };
    cue::TraceBuffer<Slot> gpu(3);
    gpu[1].sequence = 123;
    return gpu.capacity() == 3 && gpu[1].sequence == 123 ? 0 : 16;
}
} // namespace

/// @brief 標本精度と Thread / 所有境界の回帰を検証する
int main()
{
    if (const auto result = test_statistics())
    {
        return result;
    }
    if (const auto result = test_lifetime())
    {
        return result;
    }
    if (const auto result = test_concurrency())
    {
        return result;
    }
    return test_trace_buffer();
}
