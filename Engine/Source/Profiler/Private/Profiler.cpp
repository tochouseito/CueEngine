#include <Profiler/Profiler.h>

#include <limits>
#include <mutex>
#include <new>
#include <stdexcept>
#include <utility>

namespace cue
{
namespace profiler_detail
{
/// @brief 系列の標本だけを排他し、使用中の名前と ID は変更しない
struct MetricState final
{
    /// @brief 登録時だけ名前と標本領域を確保する
    explicit MetricState(std::string_view a_name) : name(a_name)
    {
    }
    mutable std::mutex mutex;
    TimingSamples samples;
    std::uint64_t id = 0;
    const std::string name;
};
} // namespace profiler_detail

/// @brief Producer の破棄後も最終値を保持し、未使用の同名領域を再利用する
struct Profiler::State final
{
    mutable std::mutex mutex;
    std::vector<std::shared_ptr<profiler_detail::MetricState>> metrics;
    std::uint64_t nextId = 1;
};

ProfileMetric::ProfileMetric(std::shared_ptr<profiler_detail::MetricState> a_state) noexcept
    : m_state(std::move(a_state))
{
}

void ProfileMetric::add(std::chrono::nanoseconds a_duration) const noexcept
{
    if (m_state)
    {
        std::lock_guard lock(m_state->mutex);
        m_state->samples.add(a_duration);
    }
}

TimingStatistics ProfileMetric::statistics() const noexcept
{
    if (!m_state)
    {
        return {};
    }
    std::lock_guard lock(m_state->mutex);
    return m_state->samples.statistics();
}

std::size_t ProfileMetric::sample_count() const noexcept
{
    if (!m_state)
    {
        return 0;
    }
    std::lock_guard lock(m_state->mutex);
    return m_state->samples.sample_count();
}

void ProfileMetric::reset() const noexcept
{
    if (m_state)
    {
        std::lock_guard lock(m_state->mutex);
        m_state->samples = {};
    }
}

bool ProfileMetric::is_valid() const noexcept
{
    return static_cast<bool>(m_state);
}

Profiler::Profiler() : m_state(std::make_unique<State>())
{
}

Profiler::~Profiler() = default;

ProfileMetric Profiler::create_metric(std::string_view a_name)
{
    std::lock_guard lock(m_state->mutex);
    if (m_state->nextId == (std::numeric_limits<std::uint64_t>::max)())
    {
        throw std::overflow_error("Profiler metric ID exhausted");
    }
    for (auto &existing : m_state->metrics)
    {
        // Registry だけが所有する領域は、停止済み Producer のものである
        // 別 Producer が同名で稼働中の場合は再利用せず、計測結果を混ぜない
        if (existing.use_count() == 1 && existing->name == a_name)
        {
            std::lock_guard metricLock(existing->mutex);
            existing->samples = {};
            existing->id = m_state->nextId++;
            return ProfileMetric(existing);
        }
    }
    auto metric = std::make_shared<profiler_detail::MetricState>(a_name);
    metric->id = m_state->nextId;
    // Registry の追加失敗時は ID を消費せず、部分生成した系列も公開しない
    m_state->metrics.push_back(metric);
    ++m_state->nextId;
    return ProfileMetric(std::move(metric));
}

Result<ProfilerSnapshot> Profiler::snapshot() const
{
    try
    {
        std::vector<std::shared_ptr<profiler_detail::MetricState>> metrics;
        {
            // 借用中の系列を延命して再利用を防ぎ、集計中は Registry の登録を妨げない
            std::lock_guard registryLock(m_state->mutex);
            metrics = m_state->metrics;
        }
        ProfilerSnapshot result;
        result.metrics.reserve(metrics.size());
        for (const auto &metric : metrics)
        {
            ProfilerMetricSnapshot entry;
            entry.id = metric->id;
            entry.name = metric->name;
            {
                // 名前の複写や返却配列の操作中に Producer の系列 Mutex を保持しない
                std::lock_guard metricLock(metric->mutex);
                entry.statistics = metric->samples.statistics();
            }
            result.metrics.push_back(std::move(entry));
        }
        return Result<ProfilerSnapshot>::success(std::move(result));
    }
    catch (const std::bad_alloc &)
    {
        return Result<ProfilerSnapshot>::failure({ErrorCategory::PlatformFailure, "Profiler.snapshot.allocation"});
    }
}
} // namespace cue
