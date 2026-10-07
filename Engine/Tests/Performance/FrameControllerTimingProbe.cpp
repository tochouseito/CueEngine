#include <algorithm>
#include <chrono>
#include <cstdio>
#include <thread>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Platform/Windows/WindowsPlatform.h>
#include <Runtime/FrameController.h>
#include <windows.h>

/// @brief 現 Process の Kernel / User 時間を合計し、全 Worker の CPU 使用時間を取得する
[[nodiscard]] double cpu_seconds()
{
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
    {
        return -1.0;
    }
    ULARGE_INTEGER kernelTicks{}, userTicks{};
    kernelTicks.LowPart = kernel.dwLowDateTime;
    kernelTicks.HighPart = kernel.dwHighDateTime;
    userTicks.LowPart = user.dwLowDateTime;
    userTicks.HighPart = user.dwHighDateTime;
    return static_cast<double>(kernelTicks.QuadPart + userTicks.QuadPart) * 1e-7;
}

/// @brief GPU と Window を含まない Controller の直列 / 並列処理と投入待機回数を計測する
[[nodiscard]] int measure(bool a_workers, unsigned a_slots, unsigned a_fps, unsigned a_sleepMs, unsigned a_frames,
                          unsigned a_repeat)
{
    auto servicesResult = cue::create_windows_thread_services();
    if (!servicesResult.has_value())
        return 1;
    auto services = servicesResult.take_value();
    auto callback = [a_sleepMs](std::uint64_t, std::stop_token)
    {
        if (a_sleepMs != 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(a_sleepMs));
        return cue::Result<void>::success();
    };
    cue::FrameController controller({a_slots, a_workers, a_fps}, *services.clock, *services.waiter,
                                    *services.threadFactory, callback, callback);
    if (!controller.start().has_value())
        return 2;
    const auto begin = std::chrono::steady_clock::now();
    const auto cpuBegin = cpu_seconds();
    std::uint64_t steps = 0, full = 0;
    while (true)
    {
        const auto progress = controller.progress();
        if (progress.renderedFrames == a_frames)
            break;
        if (std::chrono::steady_clock::now() - begin > std::chrono::seconds(15))
            return 3;
        if (progress.submittedFrames < a_frames)
        {
            ++steps;
            auto result = controller.step();
            if (!result.has_value())
                return 4;
            if (!*result.try_value())
                ++full;
        }
        else
        {
            // 最終 Frame を投入した後の観測待ちだけを行う
            const auto generation = services.waiter->generation();
            if (controller.progress().renderedFrames != a_frames)
            {
                [[maybe_unused]] auto status =
                    services.waiter->wait_for_change(generation, std::chrono::milliseconds(1), {});
            }
        }
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
    const double cpu = cpu_seconds() - cpuBegin;
    const auto progress = controller.progress();
    if (!controller.stop().has_value())
        return 5;
    std::printf("%u,%u,%u,%u,%u,%u,%.6f,%.2f,%.6f,%.2f,%llu,%llu,%.3f\n", a_repeat, a_workers, a_slots, a_fps,
                a_sleepMs, a_frames, seconds, a_frames / seconds, cpu, cpu / seconds * 100.0,
                static_cast<unsigned long long>(steps), static_cast<unsigned long long>(full),
                std::chrono::duration<double, std::milli>(progress.lastFrameInterval).count());
    return 0;
}

/// @brief 同じ Callback 条件で三つの Controller 構成を二回ずつ比較する
int main()
{
    std::puts("repeat,workers,slots,max_fps,callback_sleep_ms,frames,wall_s,average_fps,cpu_s,one_core_cpu_pct,step_"
              "calls,full_calls,last_interval_ms");
    for (unsigned repeat = 1; repeat <= 2; ++repeat)
    {
        for (unsigned mode = 0; mode < 3; ++mode)
        {
            const bool workers = mode != 0;
            const unsigned slots = mode == 2 ? 2 : 1;
            for (unsigned scenario = 0; scenario < 3; ++scenario)
            {
                const unsigned fps = scenario == 0 ? 60 : 0;
                const unsigned sleepMs = scenario == 2 ? 16 : 0;
                const unsigned frames = scenario == 0 ? 120 : scenario == 1 ? 10000 : 60;
                if (const int result = measure(workers, slots, fps, sleepMs, frames, repeat); result != 0)
                {
                    std::fprintf(stderr, "measurement failed: %d\n", result);
                    return result;
                }
            }
        }
    }
    return 0;
}
