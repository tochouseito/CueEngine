#include <WindowsHost/PerformanceSettings.h>

#include <limits>

#include "../Support/FileSystemProbe.h"
#include "../Support/TemporaryFiles.h"

/// @brief 容量、先行数、表示同期の不正な組合せを変更前に拒否する
int main()
{
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
    return 0;
}
