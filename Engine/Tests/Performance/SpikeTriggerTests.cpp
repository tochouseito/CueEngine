#include "SpikeTrigger.h"

#include <array>
#include <thread>

/// @brief 待受開始境界、最初の通知の一貫性、GPU Tick の桁落ちと別 Frame の混入拒否を確認する
int main()
{
    cue::test::SpikeTrigger trigger;
    trigger.armedAt = 100;
    trigger.consider(1, 10, 30000000, 99);
    trigger.consider(1, 11, 19999999, 100);
    if (trigger.time != 0)
    {
        return 1;
    }
    std::array<std::thread, 2> workers{std::thread([&]() { trigger.consider(1, 100, 30000000, 101); }),
                                       std::thread([&]() { trigger.consider(2, 200, 10000000, 102); })};
    for (auto &worker : workers)
    {
        worker.join();
    }
    const auto at = trigger.time.load(std::memory_order_acquire);
    if (!((at == 101 && trigger.kind == 1 && trigger.frame == 100 && trigger.duration == 30000000) ||
          (at == 102 && trigger.kind == 2 && trigger.frame == 200 && trigger.duration == 10000000)))
    {
        return 2;
    }
    cue::TimingTraceGpuRecord cal, submit, pass;
    submit.kind = 1;
    pass.kind = 2;
    cal.frame = submit.frame = pass.frame = 4;
    cal.queue = submit.queue = pass.queue = 7;
    cal.frequency = pass.frequency = 1000000000;
    cal.beginTick = 1792393187287185408;
    cal.cpuTick = 10000;
    submit.cpuAfter = 10010;
    pass.beginTick = cal.beginTick + 8000001;
    std::int64_t duration = 0;
    if (!cue::test::gpu_submit_lead(cal, submit, pass, 10000000, duration) || duration != 7999001)
    {
        return 3;
    }
    ++pass.frame;
    if (cue::test::gpu_submit_lead(cal, submit, pass, 10000000, duration))
    {
        return 4;
    }
    return 0;
}
