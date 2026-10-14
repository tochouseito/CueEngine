#include <Runtime/Simulation.h>

#include <algorithm>

namespace cue
{
/// @brief 固定更新の最大 Hz、追いつき回数と採取間隔の上限を制限する
Result<void> validate_simulation_config(const SimulationConfig &a_config)
{
    if (a_config.frequencyHz > 1000 || a_config.maxCatchUpSteps == 0 || a_config.maxCatchUpSteps > 32 ||
        a_config.maxElapsed <= std::chrono::nanoseconds::zero() || a_config.maxElapsed > std::chrono::seconds(1))
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "SimulationConfig.validate"});
    }
    return Result<void>::success();
}

/// @brief Tick 0 を初期 World とし、最初の Frame 採取から経過を測る
SimulationStepper::SimulationStepper(SimulationConfig a_config) noexcept : m_config(a_config)
{
}

/// @brief Callback を一回の更新として成功確定し、Snapshot に必要な Tick 組を返す
Result<FrameSimulationInfo> SimulationStepper::advance(std::uint64_t a_frame,
                                                       std::chrono::steady_clock::time_point a_sample,
                                                       const simulationCallback &a_update, std::stop_token a_token)
{
    if (m_isFailed)
    {
        return Result<FrameSimulationInfo>::failure({ErrorCategory::InvalidState, "SimulationStepper.failed"});
    }
    auto valid = validate_simulation_config(m_config);
    if (!valid.has_value())
    {
        return Result<FrameSimulationInfo>::failure(*valid.try_error());
    }
    if ((m_previousSample && a_sample < *m_previousSample) || (m_config.frequencyHz != 0 && !a_update))
    {
        return Result<FrameSimulationInfo>::failure({ErrorCategory::InvalidState, "SimulationStepper.advance"});
    }
    FrameSimulationInfo info;
    info.frameIndex = a_frame;
    info.isFixed = m_config.frequencyHz != 0;
    info.frameElapsed = m_previousSample
                            ? std::chrono::duration_cast<std::chrono::nanoseconds>(a_sample - *m_previousSample)
                            : std::chrono::nanoseconds::zero();
    m_previousSample = a_sample;
    const auto elapsed = (std::min)(info.frameElapsed, m_config.maxElapsed);
    info.droppedElapsed = info.frameElapsed - elapsed;
    std::uint64_t steps = 1;
    if (info.isFixed)
    {
        m_phaseUnits += static_cast<std::uint64_t>(elapsed.count()) * m_config.frequencyHz;
        steps = (std::min<std::uint64_t>)(m_phaseUnits / 1'000'000'000ULL,
                                          static_cast<std::uint64_t>(m_config.maxCatchUpSteps));
    }
    const auto delta = info.isFixed ? std::chrono::duration<double>(1.0 / m_config.frequencyHz)
                                    : std::chrono::duration<double>(elapsed);
    for (std::uint64_t index = 0; index < steps; ++index)
    {
        if (a_token.stop_requested())
        {
            info.wasCanceled = true;
            break;
        }
        if (a_update)
        {
            try
            {
                auto updated = a_update({m_tick + 1, delta, m_simulationTime + delta}, a_token);
                if (!updated.has_value())
                {
                    m_isFailed = true;
                    return Result<FrameSimulationInfo>::failure(*updated.try_error());
                }
            }
            catch (...)
            {
                m_isFailed = true;
                return Result<FrameSimulationInfo>::failure(
                    {ErrorCategory::InvalidState, "SimulationStepper.callback.exception"});
            }
        }
        ++m_tick;
        ++info.updateSteps;
        m_simulationTime += delta;
        if (info.isFixed)
        {
            m_phaseUnits -= 1'000'000'000ULL;
        }
    }
    if (info.isFixed)
    {
        // 過負荷で残った完全 Tick は捨て、次 Frame に無制限の追いつき作業を持ち越さない
        const auto discarded = m_phaseUnits / 1'000'000'000ULL;
        info.droppedElapsed += std::chrono::nanoseconds(discarded * 1'000'000'000ULL / m_config.frequencyHz);
        m_phaseUnits %= 1'000'000'000ULL;
        info.interpolationAlpha = static_cast<double>(m_phaseUnits) / 1'000'000'000.0;
    }
    info.currentTick = m_tick;
    info.previousTick = m_tick ? m_tick - 1 : 0;
    return Result<FrameSimulationInfo>::success(info);
}

/// @brief 周期変更の検証後にだけ蓄積時間を捨て、既存 World の Tick を保持する
Result<void> SimulationStepper::configure(SimulationConfig a_config)
{
    if (m_isFailed)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "SimulationStepper.failed"});
    }
    auto valid = validate_simulation_config(a_config);
    if (!valid.has_value())
    {
        return valid;
    }
    m_config = a_config;
    m_phaseUnits = 0;
    m_previousSample.reset();
    return Result<void>::success();
}

/// @brief 設定変更や最小化の停止時間を新しい更新 Delta へ加算しない
void SimulationStepper::reset_sample_time() noexcept
{
    m_previousSample.reset();
}
} // namespace cue
