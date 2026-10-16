#include <EditorHost/PerformanceSettingsEditing.h>

#include "../Support/TemporaryFiles.h"

/// @brief Worker 切替で要求値を保持し、容量縮小時だけ依存値を整える
int main()
{
    for (const bool hasExplicitLead : {false, true})
    {
        auto settings = cue::performance_preset(cue::PerformancePreset::Throughput);
        if (!hasExplicitLead)
        {
            settings.frame.maxUpdateLeadFrames.reset();
        }
        const auto savedLead = settings.frame.maxUpdateLeadFrames;
        for (const bool usesWorkers : {true, false, true})
        {
            settings.frame.useWorkerThreads = usesWorkers;
            if (cue::requested_update_lead_frames(settings.frame) != 2 ||
                cue::update_lead_frames(settings.frame) != (usesWorkers ? 2u : 0u) ||
                settings.frame.maxUpdateLeadFrames != savedLead)
            {
                return 1;
            }
        }
        settings.frame.useWorkerThreads = false;
        cue::tests::TemporaryFiles temporary;
        if (!temporary.isCreated)
        {
            return 2;
        }
        auto path = temporary.root.join("Performance.cfg");
        if (!path.has_value() ||
            !cue::save_performance_settings(*temporary.files, *path.try_value(), settings).has_value())
        {
            return 3;
        }
        auto loaded = cue::load_performance_settings(*temporary.files, *path.try_value());
        if (!loaded.has_value() || *loaded.try_value() != settings)
        {
            return 4;
        }
        auto original = settings;
        if (cue::set_cpu_frame_capacity(settings, 0).has_value() ||
            cue::set_cpu_frame_capacity(settings, 9).has_value() || settings != original ||
            !cue::set_cpu_frame_capacity(settings, 2).has_value() ||
            cue::requested_update_lead_frames(settings.frame) != 1 ||
            settings.frame.maxUpdateLeadFrames.has_value() != hasExplicitLead ||
            !cue::validate_performance_settings(settings).has_value())
        {
            return 5;
        }
        if (!cue::set_cpu_frame_capacity(settings, 3).has_value() ||
            cue::requested_update_lead_frames(settings.frame) != (hasExplicitLead ? 1u : 2u) ||
            !cue::set_cpu_frame_capacity(settings, 1).has_value() ||
            cue::requested_update_lead_frames(settings.frame) != 0 ||
            !cue::validate_performance_settings(settings).has_value())
        {
            return 6;
        }
    }
    return 0;
}
