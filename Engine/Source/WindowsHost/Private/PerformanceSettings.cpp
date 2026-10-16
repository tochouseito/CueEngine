#include <WindowsHost/PerformanceSettings.h>

#include <cmath>
#include <iomanip>
#include <locale>
#include <new>
#include <sstream>
#include <string>

namespace cue
{
/// @brief 未適用の表示 Queue 上限を設定値で埋めず、Native の成功結果を反映する
PerformanceSettingsStatus resolve_performance_status(const PerformanceSettings &a_requested,
                                                     const FrameProgress &a_progress, bool a_isTearingEnabled,
                                                     std::optional<std::uint32_t> a_queuedFrameLimit, bool a_isPending)
{
    PerformanceSettingsStatus status;
    status.requested = a_requested;
    status.effective.frame = a_requested.frame;
    status.effective.frame.maxUpdateLeadFrames = update_lead_frames(a_requested.frame);
    status.effective.frame.syncMode = a_progress.effectiveSyncMode;
    status.effective.frame.spinDuration = effective_spin_duration(a_requested.frame);
    status.effective.gpu = a_requested.gpu;
    const auto &present = a_requested.presentation;
    status.effective.presentation = {present.bufferCount, present.isVSyncEnabled,         a_isTearingEnabled,
                                     present.clearColor,  a_queuedFrameLimit.has_value(), a_queuedFrameLimit};
    status.syncFallback = a_progress.syncFallback;
    status.isPending = a_isPending;
    return status;
}

/// @brief ゲーム固有の最大値と編集許可を共通検証へ重ねる
Result<void> validate_performance_policy(const PerformanceSettings &a_settings,
                                         const PerformanceSettingsPolicy &a_policy)
{
    auto valid = validate_performance_settings(a_settings);
    if (!valid.has_value())
    {
        return valid;
    }
    if (!a_policy.usersMayEdit || a_settings.frame.maxFps > a_policy.maxFps ||
        (!a_policy.allowsUnlimitedFps && a_settings.frame.maxFps == 0) ||
        a_settings.frame.maxFramesInFlight > a_policy.maxCpuFrames ||
        a_settings.gpu.resourceFrameCount > a_policy.maxGpuFrames ||
        update_lead_frames(a_settings.frame) > a_policy.maxUpdateLeadFrames ||
        a_settings.presentation.bufferCount > a_policy.maxSwapChainBuffers ||
        a_settings.presentation.maxQueuedFrames > a_policy.maxQueuedFrames)
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "PerformanceSettings.policy"});
    }
    return Result<void>::success();
}

/// @brief 遅延と並列性の出発点を値として提供し、各タイトルで再調整できるようにする
PerformanceSettings performance_preset(PerformancePreset a_preset)
{
    PerformanceSettings settings;
    if (a_preset == PerformancePreset::LowLatency)
    {
        settings.frame.maxFramesInFlight = 1;
        settings.frame.maxUpdateLeadFrames = 0;
        settings.gpu.maxFramesInFlight = 1;
    }
    else if (a_preset == PerformancePreset::Throughput)
    {
        settings.frame.maxFramesInFlight = 3;
        settings.frame.maxUpdateLeadFrames = 2;
        settings.gpu = {3, 3};
        settings.presentation.bufferCount = 3;
        settings.presentation.maxQueuedFrames = 2;
    }
    return settings;
}

/// @brief 固定 Schema の値を保存し、部分書込で既存設定を破壊しない
Result<void> save_performance_settings(IFileSystem &a_files, const Path &a_path,
                                       const PerformanceSettings &a_settings)
{
    auto valid = validate_performance_settings(a_settings);
    if (!valid.has_value())
    {
        return valid;
    }
    try
    {
        std::ostringstream stream;
        stream.imbue(std::locale::classic());
        const auto &frame = a_settings.frame;
        const auto &gpu = a_settings.gpu;
        const auto &present = a_settings.presentation;
        stream << "CuePerformance 1\n" << frame.maxFramesInFlight << ' ' << frame.useWorkerThreads << ' '
               << frame.maxFps << ' ' << frame.maxUpdateLeadFrames.has_value() << ' '
               << frame.maxUpdateLeadFrames.value_or(0) << ' ' << static_cast<int>(frame.waitMode) << ' '
               << frame.spinDuration.count() << ' ' << static_cast<int>(frame.syncMode) << ' '
               << frame.predictionSlack.count() << '\n'
               << gpu.resourceFrameCount << ' ' << gpu.maxFramesInFlight << '\n'
               << present.bufferCount << ' ' << present.isVSyncEnabled << ' ' << present.isTearingAllowed << ' '
               << present.isWaitableEnabled << ' ' << present.maxQueuedFrames << '\n' << std::setprecision(9);
        for (const float color : present.clearColor)
        {
            stream << color << ' ';
        }
        stream << '\n';
        const auto text = stream.str();
        auto directories = a_files.create_directories(a_path.parent());
        if (!directories.has_value())
        {
            return directories;
        }
        auto saved = a_files.replace_file(a_path, std::as_bytes(std::span(text.data(), text.size())));
        return saved.has_value() ? Result<void>::success() : Result<void>::failure(*saved.try_error());
    }
    catch (const std::bad_alloc &)
    {
        return Result<void>::failure({ErrorCategory::PlatformFailure, "PerformanceSettings.save.allocation"});
    }
}

/// @brief 全値と末尾を確認し、破損 File の一部を実行設定として使わない
Result<PerformanceSettings> load_performance_settings(IFileSystem &a_files, const Path &a_path)
{
    auto bytes = a_files.read_all(a_path, 4096);
    if (!bytes.has_value())
    {
        return Result<PerformanceSettings>::failure(*bytes.try_error());
    }
    try
    {
        std::string text(reinterpret_cast<const char *>(bytes.try_value()->data()), bytes.try_value()->size());
        std::istringstream stream(text);
        stream.imbue(std::locale::classic());
        PerformanceSettings settings;
        auto &frame = settings.frame;
        auto &gpu = settings.gpu;
        auto &present = settings.presentation;
        std::string schema;
        unsigned version = 0;
        bool hasLead = false;
        std::uint32_t lead = 0;
        int wait = 0, sync = 0;
        std::int64_t spin = 0, slack = 0;
        stream >> schema >> version >> frame.maxFramesInFlight >> frame.useWorkerThreads >> frame.maxFps >> hasLead
               >> lead >> wait >> spin >> sync >> slack >> gpu.resourceFrameCount >> gpu.maxFramesInFlight
               >> present.bufferCount >> present.isVSyncEnabled >> present.isTearingAllowed
               >> present.isWaitableEnabled >> present.maxQueuedFrames;
        for (auto &color : present.clearColor)
        {
            stream >> color;
        }
        if (!stream || schema != "CuePerformance" || version != 1 || wait < 0 || wait > 1 || sync < 0 || sync > 2)
        {
            return Result<PerformanceSettings>::failure({ErrorCategory::InvalidArgument, "PerformanceSettings.schema"});
        }
        stream >> std::ws;
        if (!stream.eof())
        {
            return Result<PerformanceSettings>::failure({ErrorCategory::InvalidArgument, "PerformanceSettings.trailing"});
        }
        frame.maxUpdateLeadFrames = hasLead ? std::optional<std::uint32_t>(lead) : std::nullopt;
        frame.waitMode = static_cast<FrameWaitMode>(wait);
        frame.syncMode = static_cast<FrameSyncMode>(sync);
        frame.spinDuration = std::chrono::nanoseconds(spin);
        frame.predictionSlack = std::chrono::nanoseconds(slack);
        auto valid = validate_performance_settings(settings);
        return valid.has_value() ? Result<PerformanceSettings>::success(settings)
                                 : Result<PerformanceSettings>::failure(*valid.try_error());
    }
    catch (const std::bad_alloc &)
    {
        return Result<PerformanceSettings>::failure({ErrorCategory::PlatformFailure, "PerformanceSettings.load.allocation"});
    }
}

/// @brief Worker がない場合は先行せず、未指定なら既存の CPU 容量に合わせる
std::uint32_t update_lead_frames(const FrameControllerDesc &a_desc) noexcept
{
    return a_desc.useWorkerThreads
               ? a_desc.maxUpdateLeadFrames.value_or(a_desc.maxFramesInFlight > 0 ? a_desc.maxFramesInFlight - 1 : 0)
               : 0;
}

/// @brief Native 実体を変更する前にすべての設定値を確認する
Result<void> validate_performance_settings(const PerformanceSettings &a_settings)
{
    const auto &frame = a_settings.frame;
    const auto &gpu = a_settings.gpu;
    const auto &presentation = a_settings.presentation;
    if (frame.maxFramesInFlight == 0 || frame.maxFramesInFlight > 8 ||
        (frame.maxUpdateLeadFrames && *frame.maxUpdateLeadFrames >= frame.maxFramesInFlight) ||
        (frame.waitMode != FrameWaitMode::Sleep && frame.waitMode != FrameWaitMode::Hybrid) ||
        (frame.syncMode != FrameSyncMode::FixedPeriod && frame.syncMode != FrameSyncMode::PresentationReady &&
         frame.syncMode != FrameSyncMode::DisplayPrediction) ||
        frame.spinDuration < std::chrono::nanoseconds::zero() || frame.spinDuration > std::chrono::milliseconds(5) ||
        frame.predictionSlack < std::chrono::nanoseconds::zero() ||
        frame.predictionSlack > std::chrono::milliseconds(100) || gpu.resourceFrameCount == 0 ||
        gpu.resourceFrameCount > 8 || gpu.maxFramesInFlight == 0 || gpu.maxFramesInFlight > gpu.resourceFrameCount ||
        presentation.bufferCount < 2 || presentation.bufferCount > 16 || presentation.maxQueuedFrames == 0 ||
        presentation.maxQueuedFrames > 16 || (presentation.isVSyncEnabled && presentation.isTearingAllowed))
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "PerformanceSettings.validate"});
    }
    for (const float color : presentation.clearColor)
    {
        if (!std::isfinite(color))
            return Result<void>::failure({ErrorCategory::InvalidArgument, "PerformanceSettings.clear_color"});
    }
    return Result<void>::success();
}
} // namespace cue
