#include <EditorHost/EditorHost.h>

#include <chrono>
#include <cstdio>
#include <thread>
#include <utility>

#include <imgui.h>

namespace
{
/// @brief CSV に出す経過時間を同じ単位へ揃える
double milliseconds(std::chrono::nanoseconds a_duration)
{
    return std::chrono::duration<double, std::milli>(a_duration).count();
}

/// @brief 同じ描画と容量で同期方式だけ変更し、Warmup 後の最新 120 標本を出す
int measure(cue::FrameSyncMode a_mode, bool a_vsync)
{
    cue::EditorHostConfig config;
    config.window.clientSize = {320, 240};
    config.imgui.settingsFile.clear();
    config.frame.syncMode = a_mode;
    config.presentation.isVSyncEnabled = a_vsync;
    config.buildUi = []()
    {
        ImGui::Begin("Test");
        ImGui::TextUnformatted("TEST");
        ImGui::End();
        return cue::Result<void>::success();
    };
    cue::EditorHost host(std::move(config));
    if (!host.initialize().has_value())
    {
        return 1;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
    bool completed = false;
    while (std::chrono::steady_clock::now() < deadline)
    {
        auto stepped = host.step();
        auto progress = host.frame_progress();
        if (!stepped.has_value() || !*stepped.try_value() || !progress.has_value())
        {
            return 2;
        }
        if (progress.try_value()->renderedFrames >= 180)
        {
            completed = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    auto timing = host.frame_timing_info();
    auto graph = host.graph_performance();
    auto status = host.performance_settings_status();
    if (!completed || !timing.has_value() || !graph.has_value() || !status.has_value())
    {
        return 3;
    }
    const auto &cpu = *timing.try_value();
    const auto &gpu = *graph.try_value();
    std::printf("%d,%d,%d,%d,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%u,%llu,%d,%.3f,%.3f\n",
                static_cast<int>(a_mode), a_vsync, static_cast<int>(status.try_value()->effective.frame.syncMode),
                static_cast<int>(status.try_value()->syncFallback), milliseconds(cpu.render.averageDuration),
                milliseconds(cpu.cycleInterval.averageDuration), milliseconds(cpu.cycleInterval.p95Duration),
                milliseconds(cpu.limitWait.averageDuration), milliseconds(cpu.presentationWait.averageDuration),
                milliseconds(gpu.present.averageDuration), milliseconds(gpu.presentInterval.averageDuration),
                milliseconds(gpu.presentInterval.p95Duration), milliseconds(cpu.inputToRenderComplete.averageDuration),
                gpu.peakUnfinishedGpuFrames, static_cast<unsigned long long>(gpu.completedGpuFrames),
                gpu.presentation.hasRefreshTiming, milliseconds(gpu.presentation.refreshInterval),
                milliseconds(gpu.displayInterval.averageDuration));
    return host.shutdown().has_value() ? 0 : 4;
}
} // namespace

/// @brief 速度改善の主張と区別し、同じ条件で同期挙動の観測値を記録する
int main()
{
    std::puts("requested_mode,vsync,effective_mode,fallback,render_avg_ms,cycle_avg_ms,cycle_p95_ms,fps_wait_avg_ms,"
              "ready_wait_avg_ms,present_avg_ms,present_interval_avg_ms,present_interval_p95_ms,"
              "input_sample_to_cpu_complete_avg_ms,peak_gpu_frames,completed_gpu_frames,has_refresh_statistics,"
              "refresh_interval_ms,derived_display_interval_avg_ms");
    for (const bool vsync : {false, true})
    {
        for (const auto mode : {cue::FrameSyncMode::FixedPeriod, cue::FrameSyncMode::PresentationReady,
                                cue::FrameSyncMode::DisplayPrediction})
        {
            if (const auto result = measure(mode, vsync); result != 0)
            {
                return result;
            }
        }
    }
    return 0;
}
