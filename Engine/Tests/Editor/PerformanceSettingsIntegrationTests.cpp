#include <EditorHost/EditorHost.h>

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

/// @brief 高負荷や長い FPS 待機中も設定を排出し、CPU / GPU / 表示容量を独立して切り替える
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
    for (const auto preset : {cue::PerformancePreset::LowLatency, cue::PerformancePreset::Throughput,
                              cue::PerformancePreset::Balanced})
    {
        for (const bool usesWorkers : {false, true})
        {
            auto settings = cue::performance_preset(preset);
            settings.frame.useWorkerThreads = usesWorkers;
            settings.frame.maxFps = 0;
            settings.presentation.isWaitableEnabled = usesWorkers;
            settings.presentation.isTearingAllowed = true;
            auto before = host.frame_progress();
            if (!before.has_value() || !host.request_performance_settings(settings).has_value() ||
                !host.has_pending_performance_settings() ||
                !advance_until(host, before.try_value()->submittedFrames, std::chrono::seconds(3)))
            {
                return 2;
            }
            auto applied = host.performance_settings();
            auto progress = host.frame_progress();
            auto transfer = host.ui_transfer_info();
            if (!applied.has_value() || *applied.try_value() != settings || !progress.has_value() ||
                !transfer.has_value() || transfer.try_value()->publishedFrames != progress.try_value()->submittedFrames ||
                transfer.try_value()->consumedFrames != progress.try_value()->renderedFrames ||
                transfer.try_value()->pendingFrames > settings.frame.maxFramesInFlight ||
                (progress.try_value()->renderThreadId != std::this_thread::get_id()) != usesWorkers)
            {
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
        config.callbacks.reconfigureRenderer = [throwsException](cue::IBackend &, std::uint32_t, std::uint32_t)
            -> cue::Result<void>
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
    return test_failed_reconfiguration();
}
