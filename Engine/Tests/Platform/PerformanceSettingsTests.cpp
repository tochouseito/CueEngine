#include <WindowsHost/PerformanceSettings.h>

#include <array>
#include <chrono>
#include <limits>
#include <span>
#include <string>
#include <string_view>

#include "../Support/FileSystemProbe.h"
#include "../Support/TemporaryFiles.h"

namespace
{
/// @brief 旧設定を GPU 資源枠数へ移行し、破損設定を補正で隠さず拒否する
bool test_schema_migration(cue::IFileSystem &a_files, const cue::Path &a_path)
{
    const std::array<std::string, 3> legacy{
        "CuePerformance 1\n3 1 75 1 0 1 456000 0 1000000\n2 2\n3 0 1 0 2\n0.1 0.2 0.3 1\n",
        "CuePerformance 1\n2 0 75 1 1 1 456000 0 1000000\n3 1\n3 0 1 0 2\n0.1 0.2 0.3 1\n",
        "CuePerformance 1\n2 1 75 1 1 1 456000 0 1000000\n1 1\n3 0 1 0 2\n0.1 0.2 0.3 1\n"};
    const std::array<std::uint32_t, 3> counts{2, 3, 1};
    for (std::size_t index = 0; index < legacy.size(); ++index)
    {
        const auto &text = legacy[index];
        if (!a_files.write_all(a_path, std::as_bytes(std::span(text.data(), text.size()))).has_value())
        {
            return false;
        }
        auto loaded = cue::load_performance_settings(a_files, a_path);
        if (!loaded.has_value())
        {
            return false;
        }
        const auto &settings = *loaded.try_value();
        if (settings.frame.maxFramesInFlight != counts[index] || settings.gpu.resourceFrameCount != counts[index] ||
            settings.frame.useWorkerThreads != (counts[index] > 1) ||
            settings.frame.maxUpdateLeadFrames != counts[index] - 1 || settings.frame.maxFps != 75 ||
            settings.frame.spinDuration != std::chrono::microseconds(456) || settings.presentation.bufferCount != 3 ||
            !settings.presentation.isTearingAllowed || settings.presentation.isWaitableEnabled ||
            settings.presentation.maxQueuedFrames != 2 ||
            !cue::save_performance_settings(a_files, a_path, settings).has_value())
        {
            return false;
        }
        auto bytes = a_files.read_all(a_path, 4096);
        if (!bytes.has_value() ||
            !std::string_view(reinterpret_cast<const char *>(bytes.try_value()->data()), bytes.try_value()->size())
                 .starts_with("CuePerformance 2\n"))
        {
            return false;
        }
        auto reloaded = cue::load_performance_settings(a_files, a_path);
        if (!reloaded.has_value() || *reloaded.try_value() != settings)
        {
            return false;
        }
    }
    for (const std::string invalid :
         {"CuePerformance 1\n9 1 75 1 0 1 456000 0 1000000\n2 2\n3 0 1 0 2\n0.1 0.2 0.3 1\n",
          "CuePerformance 2\n1 75 1 456000 0 1000000\n2\n3 0 1 0 2\n0.1 0.2 0.3 1\n",
          "CuePerformance 2\n0 75 1 456000 0 1000000\n1\n3 0 1 0 2\n0.1 0.2 0.3 1\n"})
    {
        if (!a_files.write_all(a_path, std::as_bytes(std::span(invalid.data(), invalid.size()))).has_value() ||
            cue::load_performance_settings(a_files, a_path).has_value())
        {
            return false;
        }
    }
    // 旧 API の派生値が残っても保存時は共通設定を正本とし、入力自体は変更しない
    cue::PerformanceSettings settings;
    settings.frame.useWorkerThreads = false;
    settings.frame.maxFramesInFlight = 3;
    settings.frame.maxUpdateLeadFrames = 0;
    const auto original = settings;
    if (!cue::save_performance_settings(a_files, a_path, settings).has_value())
    {
        return false;
    }
    auto loaded = cue::load_performance_settings(a_files, a_path);
    return settings == original && loaded.has_value() && loaded.try_value()->frame.useWorkerThreads &&
           loaded.try_value()->frame.maxFramesInFlight == 2 && loaded.try_value()->frame.maxUpdateLeadFrames == 1;
}
} // namespace

/// @brief 容量、先行数、表示同期の不正な組合せを変更前に拒否する
int main()
{
    for (const bool usesWorkers : {false, true})
    {
        cue::PerformanceSettings requested;
        requested.frame.useWorkerThreads = usesWorkers;
        requested.frame.maxUpdateLeadFrames = 1;
        requested.presentation.maxQueuedFrames = 3;
        cue::FrameProgress progress;
        progress.syncFallback = cue::FrameSyncFallback::PresentationUnavailable;
        const auto inactive = cue::resolve_performance_status(requested, progress, false, std::nullopt, true);
        const auto active = cue::resolve_performance_status(requested, progress, true, 3);
        if (inactive.requested != requested || active.requested != requested ||
            inactive.effective.presentation.maxQueuedFrames || inactive.effective.presentation.isWaitableEnabled ||
            !inactive.isPending || inactive.syncFallback != progress.syncFallback ||
            inactive.effective.frame.maxUpdateLeadFrames != (usesWorkers ? 1u : 0u) ||
            active.effective.presentation.maxQueuedFrames != 3 || !active.effective.presentation.isWaitableEnabled ||
            !active.effective.presentation.isTearingAllowed || active.isPending)
        {
            return 16;
        }
    }
    cue::PerformanceSettings settings;
    if (!cue::validate_performance_settings(settings).has_value() || cue::update_lead_frames(settings.frame) != 1)
        return 1;
    settings.frame.maxFramesInFlight = 3;
    settings.frame.maxUpdateLeadFrames = 2;
    if (!cue::validate_performance_settings(settings).has_value() || cue::update_lead_frames(settings.frame) != 2)
        return 2;
    settings.frame.maxUpdateLeadFrames = 3;
    if (cue::validate_performance_settings(settings).has_value())
        return 3;
    settings = {};
    settings.gpu.maxFramesInFlight = 3;
    if (cue::validate_performance_settings(settings).has_value())
        return 4;
    settings = {};
    settings.presentation.isVSyncEnabled = true;
    settings.presentation.isTearingAllowed = true;
    if (cue::validate_performance_settings(settings).has_value())
        return 5;
    settings = {};
    settings.presentation.clearColor[0] = std::numeric_limits<float>::quiet_NaN();
    if (cue::validate_performance_settings(settings).has_value())
        return 6;
    settings = {};
    settings.frame.useWorkerThreads = false;
    if (cue::update_lead_frames(settings.frame) != 0)
        return 7;
    cue::PerformanceSettingsPolicy policy;
    policy.maxUpdateLeadFrames = 0;
    if (cue::validate_performance_policy({}, policy).has_value())
        return 8;
    settings = cue::performance_preset(cue::PerformancePreset::Throughput);
    settings.frame.useWorkerThreads = false;
    settings.frame.maxFramesInFlight = 1;
    settings.frame.maxUpdateLeadFrames = 0;
    if (cue::validate_performance_policy(settings, policy).has_value())
    {
        return 18;
    }
    cue::tests::TemporaryFiles temporary;
    if (!temporary.isCreated)
        return 9;
    auto pathResult = temporary.root.join("Performance.cfg");
    if (!pathResult.has_value())
        return 10;
    const auto path = pathResult.take_value();
    for (const auto preset : {cue::PerformancePreset::Balanced, cue::PerformancePreset::LowLatency,
                              cue::PerformancePreset::Throughput})
    {
        settings = cue::performance_preset(preset);
        settings.presentation.isWaitableEnabled = false;
        settings.presentation.maxQueuedFrames = 3;
        settings.frame.spinDuration = std::chrono::nanoseconds(234567);
        if (!cue::save_performance_settings(*temporary.files, path, settings).has_value())
            return 11;
        auto loaded = cue::load_performance_settings(*temporary.files, path);
        if (!loaded.has_value() || *loaded.try_value() != settings)
            return 12;
    }
    cue::tests::FileSystemProbe failing(*temporary.files);
    failing.failSave = true;
    if (cue::save_performance_settings(failing, path, {}).has_value())
        return 13;
    auto preserved = cue::load_performance_settings(*temporary.files, path);
    if (!preserved.has_value() || *preserved.try_value() != settings)
        return 14;
    const std::string corrupt = "CuePerformance 99\n";
    if (!temporary.files->write_all(path, std::as_bytes(std::span(corrupt.data(), corrupt.size()))).has_value() ||
        cue::load_performance_settings(*temporary.files, path).has_value())
        return 15;
    return test_schema_migration(*temporary.files, path) ? 0 : 17;
}
