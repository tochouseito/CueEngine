#include <EditorHost/EditorHost.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <utility>

#include <Platform/Windows/WindowsFileSystem.h>

#include "../Support/TemporaryFiles.h"

namespace
{
/// @brief 設定適用の境界と次の描画まで Pump を継続する
bool advance_until(cue::EditorHost &a_host, std::uint64_t a_after, std::chrono::milliseconds a_timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + a_timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        auto stepped = a_host.step();
        if (!stepped.has_value())
        {
            std::fprintf(stderr, "step failed: %s\n", stepped.try_error()->operation.c_str());
            return false;
        }
        auto progress = a_host.frame_progress();
        if (!*stepped.try_value() || !progress.has_value())
        {
            return false;
        }
        if (!a_host.has_pending_performance_settings() && progress.try_value()->renderedFrames > a_after)
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

/// @brief 長い FPS 待機中も共通の枠数と Worker を切り替え、表示待機は独立して扱う
int test_reconfiguration()
{
    cue::EditorHostConfig config;
    config.window.clientSize = {320, 240};
    config.imgui.settingsFile.clear();
    config.performanceSettingsFile.clear();
    config.frame.maxFps = 1;
    cue::EditorHost host(std::move(config));
    if (!host.initialize().has_value() || !advance_until(host, 0, std::chrono::seconds(3)))
    {
        return 1;
    }
    for (const auto preset :
         {cue::PerformancePreset::LowLatency, cue::PerformancePreset::Throughput, cue::PerformancePreset::Balanced})
    {
        for (const bool isWaitableEnabled : {false, true})
        {
            auto settings = cue::performance_preset(preset);
            const bool usesWorkers = settings.gpu.resourceFrameCount > 1;
            settings.frame.maxFps = 0;
            settings.presentation.isWaitableEnabled = isWaitableEnabled;
            settings.presentation.isTearingAllowed = true;
            auto request = settings;
            // 旧 API の独立指定が残っても、要求の受付時に共通枠数へ揃える
            request.frame.useWorkerThreads = !usesWorkers;
            request.frame.maxFramesInFlight = usesWorkers ? 1 : 3;
            request.frame.maxUpdateLeadFrames = 0;
            auto before = host.frame_progress();
            if (!before.has_value() || !host.request_performance_settings(request).has_value() ||
                !host.has_pending_performance_settings() ||
                !advance_until(host, before.try_value()->submittedFrames, std::chrono::seconds(3)))
            {
                return 2;
            }
            auto applied = host.performance_settings();
            auto status = host.performance_settings_status();
            auto progress = host.frame_progress();
            auto transfer = host.ui_transfer_info();
            auto afterTransfer = host.frame_progress();
            // UI 消費は Render Callback 内、CPU 完了数は Callback 復帰後に公開される
            // 別々の Snapshot に同時点の一致を要求せず、取得前後の単調範囲と保存則を確認する
            if (!applied.has_value() || *applied.try_value() != settings || !status.has_value() ||
                status.try_value()->requested != settings ||
                status.try_value()->effective.frame.maxFramesInFlight != settings.gpu.resourceFrameCount ||
                status.try_value()->effective.frame.maxUpdateLeadFrames != settings.gpu.resourceFrameCount - 1 ||
                status.try_value()->effective.frame.useWorkerThreads != usesWorkers ||
                status.try_value()->effective.presentation.maxQueuedFrames !=
                    (settings.presentation.isWaitableEnabled ? std::optional(settings.presentation.maxQueuedFrames)
                                                             : std::nullopt) ||
                !progress.has_value() || !transfer.has_value() || !afterTransfer.has_value() ||
                transfer.try_value()->publishedFrames != progress.try_value()->submittedFrames ||
                transfer.try_value()->consumedFrames < progress.try_value()->renderedFrames ||
                transfer.try_value()->consumedFrames > afterTransfer.try_value()->renderedFrames + 1 ||
                transfer.try_value()->publishedFrames !=
                    transfer.try_value()->consumedFrames + transfer.try_value()->pendingFrames ||
                transfer.try_value()->pendingFrames > settings.frame.maxFramesInFlight ||
                (progress.try_value()->renderThreadId != std::this_thread::get_id()) != usesWorkers)
            {
                if (progress.has_value() && transfer.has_value())
                {
                    std::fprintf(stderr,
                                 "reconfiguration buffers=%u waitable=%d submitted=%llu rendered=%llu "
                                 "published=%llu consumed=%llu pending=%u\n",
                                 settings.gpu.resourceFrameCount, isWaitableEnabled,
                                 static_cast<unsigned long long>(progress.try_value()->submittedFrames),
                                 static_cast<unsigned long long>(progress.try_value()->renderedFrames),
                                 static_cast<unsigned long long>(transfer.try_value()->publishedFrames),
                                 static_cast<unsigned long long>(transfer.try_value()->consumedFrames),
                                 transfer.try_value()->pendingFrames);
                }
                return 3;
            }
        }
    }
    auto invalid = cue::PerformanceSettings{};
    invalid.gpu.resourceFrameCount = 0;
    if (host.request_performance_settings(invalid).has_value() || host.has_pending_performance_settings())
    {
        return 4;
    }
    return host.shutdown().has_value() ? 0 : 5;
}

/// @brief 保存値を次の起動で読み込み、現在の構成として公開する
int test_saved_restart()
{
    cue::tests::TemporaryFiles temporary;
    if (!temporary.isCreated)
    {
        return 1;
    }
    const auto settings = cue::performance_preset(cue::PerformancePreset::LowLatency);
    {
        cue::EditorHostConfig config;
        config.window.clientSize = {320, 240};
        config.imgui.settingsFile.clear();
        config.storage.dataRootOverride = temporary.root;
        cue::EditorHost host(std::move(config));
        if (!host.initialize().has_value() || !host.request_performance_settings(settings).has_value() ||
            host.save_performance_settings().has_value() || !advance_until(host, 0, std::chrono::seconds(3)) ||
            !host.save_performance_settings().has_value() || !host.shutdown().has_value())
        {
            return 2;
        }
    }
    cue::EditorHostConfig config;
    config.window.clientSize = {320, 240};
    config.imgui.settingsFile.clear();
    config.storage.dataRootOverride = temporary.root;
    cue::EditorHost host(std::move(config));
    if (!host.initialize().has_value())
    {
        return 3;
    }
    auto applied = host.performance_settings();
    return applied.has_value() && *applied.try_value() == settings && host.shutdown().has_value() ? 0 : 4;
}

/// @brief 再構成 Callback の失敗 / 例外後は旧実効値を公開せず、停止処理だけ許可する
int test_failed_reconfiguration()
{
    for (const bool throwsException : {false, true})
    {
        cue::WindowsHostConfig config;
        config.window = {"CueEngine Settings Failure", {320, 240}};
        config.frame = {1, false, 60};
        config.callbacks.initializeRenderer = [](cue::IBackend &, std::uint32_t, std::uint32_t)
        { return cue::Result<void>::success(); };
        config.callbacks.reconfigureRenderer = [throwsException](cue::IBackend &, std::uint32_t,
                                                                 std::uint32_t) -> cue::Result<void>
        {
            if (throwsException)
            {
                throw 1;
            }
            return cue::Result<void>::failure({cue::ErrorCategory::InvalidState, "Test.reconfiguration"});
        };
        cue::WindowsHost host(std::move(config));
        auto requested = cue::performance_preset(cue::PerformancePreset::Throughput);
        if (!host.initialize().has_value() || !host.request_performance_settings(requested).has_value())
        {
            return 1;
        }
        auto applied = host.step();
        if (applied.has_value() || host.performance_settings().has_value() || host.step().has_value() ||
            host.request_performance_settings({}).has_value() || !host.shutdown().has_value())
        {
            return 2;
        }
    }
    return 0;
}

/// @brief 固定 / 可変更新を Host から切り替え、FPS 設定変更でゲームの Hz を変えない
int test_simulation_settings()
{
    cue::EditorHostConfig config;
    config.window.clientSize = {320, 240};
    config.imgui.settingsFile.clear();
    config.performanceSettingsFile.clear();
    config.frame.maxFps = 120;
    config.simulation = {60, 4};
    std::atomic<std::uint64_t> lastTick = 0;
    config.simulationUpdate = [&](const cue::SimulationStep &a_step, std::stop_token)
    {
        lastTick.store(a_step.tick);
        return cue::Result<void>::success();
    };
    config.prepareFrame = [](const cue::FrameSimulationInfo &a_info, std::stop_token)
    {
        return a_info.interpolationAlpha >= 0 && a_info.interpolationAlpha <= 1
                   ? cue::Result<void>::success()
                   : cue::Result<void>::failure({cue::ErrorCategory::InvalidState, "Test.interpolation"});
    };
    cue::EditorHost host(std::move(config));
    if (!host.initialize().has_value() || !advance_until(host, 10, std::chrono::seconds(3)))
    {
        return 1;
    }
    auto fixedProgress = host.frame_progress();
    auto fixed = host.frame_simulation_info(fixedProgress.try_value()->lastRenderFrame);
    if (!fixed.has_value() || !fixed.try_value()->isFixed || lastTick.load() == 0 ||
        !host.request_simulation_config({0, 4}).has_value() ||
        !advance_until(host, fixedProgress.try_value()->submittedFrames, std::chrono::seconds(3)))
    {
        return 2;
    }
    auto variableProgress = host.frame_progress();
    auto variable = host.frame_simulation_info(variableProgress.try_value()->lastRenderFrame);
    if (!variable.has_value() || variable.try_value()->isFixed ||
        variable.try_value()->currentTick <= fixed.try_value()->currentTick ||
        !host.request_simulation_config({60, 4}).has_value() ||
        !advance_until(host, variableProgress.try_value()->submittedFrames, std::chrono::seconds(3)))
    {
        return 3;
    }
    auto settings = host.performance_settings();
    settings.try_value()->frame.maxFps = 30;
    auto before = host.frame_progress();
    if (!host.request_performance_settings(*settings.try_value()).has_value() ||
        !advance_until(host, before.try_value()->submittedFrames, std::chrono::seconds(3)))
    {
        return 4;
    }
    auto after = host.frame_progress();
    auto unchanged = host.frame_simulation_info(after.try_value()->lastRenderFrame);
    return unchanged.has_value() && unchanged.try_value()->isFixed && host.shutdown().has_value() ? 0 : 5;
}
} // namespace

/// @brief 実 ImGui / GPU 上で枠変更と Thread 切替後も単調 Frame ID を維持する
int main()
{
    if (const auto configured = test_reconfiguration(); configured != 0)
    {
        return configured;
    }
    if (const auto saved = test_saved_restart(); saved != 0)
    {
        return 10 + saved;
    }
    if (const auto failed = test_failed_reconfiguration(); failed != 0)
    {
        return 20 + failed;
    }
    const auto simulation = test_simulation_settings();
    return simulation == 0 ? 0 : 30 + simulation;
}
