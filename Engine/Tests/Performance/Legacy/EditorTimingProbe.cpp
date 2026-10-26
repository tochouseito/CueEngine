#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <ostream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include <Windows.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <EditorHost/EditorHost.h>
#include <Profiler/TimingSamples.h>

#include "PerformanceStudy.h"
#include "PresentDispatchProbe.h"

namespace
{
/// @brief 製品の起動引数と混在させず、調査実行だけの条件を所有する
struct ProbeOptions final
{
    std::string caseName = "baseline";
    unsigned warmup = 120;
    unsigned frames = 600;
    unsigned repeat = 1;
    std::filesystem::path output = "out/validation/M06-PerformanceStudy/baseline.csv";
    std::filesystem::path captureReady;
};

/// @brief 符号、余剰文字、範囲外の値を許さず整数を解析する
[[nodiscard]] bool parse_unsigned(std::string_view a_text, unsigned &a_value)
{
    const auto result = std::from_chars(a_text.data(), a_text.data() + a_text.size(), a_value);
    return result.ec == std::errc{} && result.ptr == a_text.data() + a_text.size();
}

/// @brief 観測用の引数だけを受け付け、Timeout 内に終わらない条件を拒否する
[[nodiscard]] bool parse_options(int a_count, char **a_arguments, ProbeOptions &a_options)
{
    for (int index = 1; index < a_count; ++index)
    {
        const std::string_view option = a_arguments[index];
        if (index + 1 == a_count)
        {
            return false;
        }
        const std::string_view value = a_arguments[++index];
        if (option == "--case")
        {
            a_options.caseName = value;
        }
        else if (option == "--warmup")
        {
            if (!parse_unsigned(value, a_options.warmup))
            {
                return false;
            }
        }
        else if (option == "--frames")
        {
            if (!parse_unsigned(value, a_options.frames))
            {
                return false;
            }
        }
        else if (option == "--repeat")
        {
            // 独立 Process で反復する際の実行番号とし、一つの Process は一条件だけ測る
            if (!parse_unsigned(value, a_options.repeat))
            {
                return false;
            }
        }
        else if (option == "--output")
        {
            a_options.output = std::filesystem::path(value);
        }
        else if (option == "--capture-ready")
        {
            a_options.captureReady = std::filesystem::path(value);
        }
        else
        {
            return false;
        }
    }
    const auto &name = a_options.caseName;
    const bool isKnown =
        name == "baseline" || name == "minimal" || name == "readout15" || name == "gpuoff" || name == "pixoff" ||
        name == "tearing" || name == "minimal-gpuoff" || name == "minimal-pixoff" || name == "minimal-readout15" ||
        name == "statunlock" || name == "freqcache" || name == "nthstats" || name == "gpustats15" ||
        name == "combined" || name == "combined-readout15" || name == "preformat" || name == "preformat-combined" ||
        name == "preformat-combined-readout15" || name == "next-baseline" || name == "next-ui-gate" ||
        name == "next-frame1" || name == "next-main" || name == "next-buffer3" || name == "next-tearing" ||
        name == "next-command2" || name == "next-combined" || name == "next-vsync" || name == "next-cycles" ||
        name == "new-baseline" || name == "new-cache" || name == "new-snapshot" || name == "new-waitable" ||
        name == "new-descriptor" || name == "new-screen-windowed" || name == "new-borderless" ||
        name == "new-borderless-waitable" || name == "new-stress" || name == "new-descriptor-stress" ||
        name == "new-cache-buffer3" || name == "new-waitable-lifecycle" || name == "new-present-profile" ||
        name == "new-present-profile-main" || name == "new-present-profile-minimal" ||
        name == "new-present-profile-gate" || name == "new-present-profile-gpuwait" || name == "new-swap-latency1" ||
        name == "new-swap-buffer3-latency2" || name == "new-swap-buffer3-latency3" ||
        name == "new-swap-buffer4-latency4" || name == "new-swap-none" || name == "new-swap-none-lifecycle" ||
        name == "new-borderless-swap-none" || name == "new-borderless-swap-none-tearing";
    // 60 FPS での 30 秒以内の投入を上限とし、初期化を除く 35 秒 Timeout に余裕を残す
    return isKnown && a_options.frames > 0 && a_options.repeat > 0 && !a_options.output.empty() &&
           a_options.warmup <= 1800 && a_options.frames <= 1800 - a_options.warmup;
}

/// @brief JSON の文字列として制御文字と引用符を Escape する
void write_string(std::ostream &a_output, std::string_view a_value)
{
    constexpr char k_hex[] = "0123456789abcdef";
    a_output << '"';
    for (const unsigned char character : a_value)
    {
        if (character == '"' || character == '\\')
        {
            a_output << '\\' << static_cast<char>(character);
        }
        else if (character < 0x20)
        {
            a_output << "\\u00" << k_hex[character >> 4] << k_hex[character & 15];
        }
        else
        {
            a_output << static_cast<char>(character);
        }
    }
    a_output << '"';
}

/// @brief 統計表示の描画費用だけを比較できる同じ位置と寸法の Test Window を構築する
[[nodiscard]] cue::Result<void> build_minimal_ui()
{
    ImGui::SetNextWindowPos({32.0f, 32.0f}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize({640.0f, 480.0f}, ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Test"))
    {
        ImGui::Text("TEST");
    }
    ImGui::End();
    return cue::Result<void>::success();
}

/// @brief 全 Worker と GPU が停止してから所有済みの記録を CSV と条件 File へ保存する
[[nodiscard]] bool save_results(const ProbeOptions &a_options, const cue::study::Options &a_studyOptions,
                                const cue::FrameControllerDesc &a_frame, const cue::PresentationConfig &a_presentation,
                                const cue::FrameProgress &a_progress, const cue::ImGuiFrameInfo &a_ui,
                                const cue::ImGuiTransferInfo &a_transfer, double a_wallSeconds, int a_status)
{
    std::error_code error;
    if (!a_options.output.parent_path().empty())
    {
        std::filesystem::create_directories(a_options.output.parent_path(), error);
        if (error)
        {
            return false;
        }
    }
    std::ofstream csv(a_options.output, std::ios::binary);
    if (!csv)
    {
        return false;
    }
    const std::uint64_t endFrame = static_cast<std::uint64_t>(a_options.warmup) + a_options.frames;
    if (a_studyOptions.presentDispatch)
    {
        if (!cue::dispatch_probe::save(a_options.output, a_options.warmup, a_options.frames) ||
            cue::study::dropped_timeline_samples())
            return false;
        auto timelinePath = a_options.output;
        timelinePath += ".timeline.csv";
        std::ofstream timeline(timelinePath, std::ios::binary);
        timeline << "frame,metric,start_ns,end_ns,thread\n";
        // Message Pump は Frame 外で呼ばれるため、時間軸で Present と照合する
        for (const auto &sample : cue::study::timeline_samples())
            timeline << sample.frame << ',' << cue::study::metric_name(sample.metric) << ',' << sample.start << ','
                     << sample.end << ',' << sample.thread << '\n';
        timeline.flush();
        if (!timeline)
            return false;
        auto readinessPath = a_options.output;
        readinessPath += ".readiness.csv";
        std::ofstream readiness(readinessPath, std::ios::binary);
        readiness << "frame,fence,was_ready,is_ready\n";
        for (const auto &sample : cue::study::gpu_readiness_samples())
            if (sample.frame >= a_options.warmup && sample.frame < endFrame)
                readiness << sample.frame << ',' << sample.fence << ',' << sample.wasReady << ',' << sample.isReady << '\n';
        readiness.flush();
        if (!readiness)
            return false;
    }
    if (a_studyOptions.nativeCycles)
    {
        // CPU Cycle の単位を時間の CSV と混ぜない。取得は Windows 専用 Probe が提供する
        auto cyclePath = a_options.output;
        cyclePath += ".cycles.csv";
        std::ofstream cycles(cyclePath, std::ios::binary);
        cycles << "case,repeat,frame,metric,cycles,thread\n";
        for (const auto &sample : cue::study::cycle_samples())
        {
            if (sample.frame >= a_options.warmup && sample.frame < endFrame)
                cycles << a_options.caseName << ',' << a_options.repeat << ',' << sample.frame << ','
                       << cue::study::metric_name(sample.metric) << ',' << sample.cycles << ',' << sample.thread
                       << '\n';
        }
        cycles.flush();
        if (!cycles)
            return false;
    }
    csv << "case,repeat,frame,metric,nanoseconds,thread\n";
    std::size_t selected = 0;
    for (const auto &sample : cue::study::samples())
    {
        // Main / Render を時刻で結合せず、各 Hook の Frame 識別子で同じ観測範囲を選ぶ
        if (sample.frame >= a_options.warmup && sample.frame < endFrame)
        {
            csv << a_options.caseName << ',' << a_options.repeat << ',' << sample.frame << ','
                << cue::study::metric_name(sample.metric) << ',' << sample.nanoseconds << ',' << sample.thread << '\n';
            ++selected;
        }
    }
    csv.flush();
    if (!csv)
    {
        return false;
    }
    auto metadataPath = a_options.output;
    metadataPath += ".json";
    std::ofstream metadata(metadataPath, std::ios::binary);
    if (!metadata)
    {
        return false;
    }
    metadata << "{\n    \"case\": ";
    write_string(metadata, a_options.caseName);
    metadata << ",\n    \"configuration\": ";
    write_string(metadata, CUE_STUDY_CONFIGURATION);
    const auto adapter = cue::study::adapter();
    metadata << ",\n    \"platform\": \"Windows\",\n    \"pointer_bits\": " << sizeof(void *) * 8
             << ",\n    \"repeat\": " << a_options.repeat << ",\n    \"warmup_frames\": " << a_options.warmup
             << ",\n    \"measurement_frames\": " << a_options.frames << ",\n    \"status\": " << a_status
             << ",\n    \"wall_seconds\": " << a_wallSeconds << ",\n    \"selected_samples\": " << selected
             << ",\n    \"dropped_samples\": " << cue::study::dropped_samples()
             << ",\n    \"window\": {\"title\": \"CueEngine Performance Study\", \"width\": "
             << cue::study::window().width << ", \"height\": " << cue::study::window().height
             << ", \"refresh_hz\": " << cue::study::window().refreshHz
             << ", \"borderless\": " << (cue::study::window().isBorderless ? "true" : "false")
             << ", \"primary_monitor\": " << (cue::study::window().isPrimaryMonitor ? "true" : "false")
             << ", \"monitor_width\": " << cue::study::window().monitorWidth
             << ", \"monitor_height\": " << cue::study::window().monitorHeight
             << ", \"outer_x\": " << cue::study::window().outerX << ", \"outer_y\": " << cue::study::window().outerY
             << ", \"outer_width\": " << cue::study::window().outerWidth
             << ", \"outer_height\": " << cue::study::window().outerHeight
             << "},"
                "\n    \"frame\": {\"workers\": "
             << (a_frame.useWorkerThreads ? "true" : "false") << ", \"frames_in_flight\": " << a_frame.maxFramesInFlight
             << ", \"max_fps\": " << a_frame.maxFps
             << "},"
                "\n    \"presentation\": {\"buffers\": "
             << a_presentation.bufferCount << ", \"vsync\": " << (a_presentation.isVSyncEnabled ? "true" : "false")
             << ", \"tearing\": " << (a_presentation.isTearingAllowed ? "true" : "false")
             << ", \"actual_tearing\": " << (cue::study::tearing_enabled() ? "true" : "false")
             << "},"
                "\n    \"study\": {\"readout_stride\": "
             << a_studyOptions.readoutStride
             << ", \"gpu_timestamps\": " << (a_studyOptions.gpuTimestamps ? "true" : "false")
             << ", \"pix_markers\": " << (a_studyOptions.pixMarkers ? "true" : "false")
             << ", \"statistics_outside_lock\": " << (a_studyOptions.statisticsOutsideLock ? "true" : "false")
             << ", \"cache_timestamp_frequency\": " << (a_studyOptions.cacheTimestampFrequency ? "true" : "false")
             << ", \"nth_statistics\": " << (a_studyOptions.useNthStatistics ? "true" : "false")
             << ", \"gpu_statistics_stride\": " << a_studyOptions.gpuStatisticsStride
             << ", \"preformat_ui_text\": " << (a_studyOptions.preformatUiText ? "true" : "false")
             << ", \"wait_before_ui\": " << (a_studyOptions.waitBeforeUi ? "true" : "false")
             << ", \"rotate_command_slots\": " << (a_studyOptions.rotateCommandSlots ? "true" : "false")
             << ", \"native_cycles\": " << (a_studyOptions.nativeCycles ? "true" : "false")
             << ", \"cache_graph_preparation\": " << (a_studyOptions.cacheGraphPreparation ? "true" : "false")
             << ", \"separate_imgui_recording\": " << (a_studyOptions.separateImGuiRecording ? "true" : "false")
             << ", \"waitable_swap_chain\": " << (a_studyOptions.waitableSwapChain ? "true" : "false")
             << ", \"maximum_presentation_latency\": "
             << (a_studyOptions.waitableSwapChain ? a_studyOptions.maximumPresentationLatency : 0)
             << ", \"no_presentation_scaling\": " << (a_studyOptions.noPresentationScaling ? "true" : "false")
             << ", \"swap_chain_created\": {\"buffers\": " << cue::study::presentation().buffers
             << ", \"maximum_latency\": " << cue::study::presentation().maximumLatency
             << ", \"scaling\": " << cue::study::presentation().scaling
             << ", \"flags\": " << cue::study::presentation().flags
             << ", \"width\": " << cue::study::presentation().width
             << ", \"height\": " << cue::study::presentation().height << "}"
             << ", \"descriptor_lookup\": " << (a_studyOptions.descriptorLookup ? "true" : "false")
             << ", \"present_dispatch\": " << (a_studyOptions.presentDispatch ? "true" : "false")
             << ", \"present_gpu_wait\": " << (a_studyOptions.presentGpuWait ? "true" : "false")
             << ", \"stress_texture_count\": " << (a_options.caseName.ends_with("stress") ? 256 : 0)
             << "},"
                "\n    \"progress\": {\"submitted\": "
             << a_progress.submittedFrames << ", \"updated\": " << a_progress.updatedFrames
             << ", \"rendered\": " << a_progress.renderedFrames << "},\n    \"ui\": {\"frames\": " << a_ui.frames
             << ", \"vertices\": " << a_ui.vertexCount << ", \"indices\": " << a_ui.indexCount
             << ", \"snapshot_allocations\": " << a_transfer.snapshotAllocations
             << ", \"copied_bytes\": " << a_transfer.copiedBytes
             << "},"
                "\n    \"layout_file\": null,"
                "\n    \"adapter\": {\"source\": \"Actual RenderDevice adapter\", \"vendor_id\": "
             << adapter.vendorId << ", \"device_id\": " << adapter.deviceId
             << ", \"dedicated_video_memory_bytes\": " << adapter.dedicatedVideoMemory
             << "},"
                "\n    \"method\": \"Raw Hook elapsed times; fixed preallocated collector; "
                "no timing-statistics polling or file IO during frames. Hooks and clock reads add overhead. "
                "Select warmup <= frame < warmup + frames. Metric intervals may nest and must not be summed blindly.\"";
    // 実際の再利用を停止後に確認する。初期化と Warmup の貸出も含む
    metadata << ",\n    \"cycle_failures\": " << cue::study::cycle_failures() << ",\n    \"command_slot_counts\": [";
    const auto slotCounts = cue::study::command_slot_counts();
    for (std::size_t index = 0; index < slotCounts.size(); ++index)
        metadata << (index ? ", " : "") << slotCounts[index];
    metadata << "]\n}\n";
    metadata.flush();
    return static_cast<bool>(metadata);
}

/// @brief 調査の部分選択集計が既存 Sort と同じ全統計を返すことを Worker 起動前に確認する
[[nodiscard]] bool verify_statistics()
{
    for (unsigned pattern = 0; pattern < 8; ++pattern)
    {
        cue::TimingSamples samples;
        for (unsigned count = 0; count < 260; ++count)
        {
            cue::study::configure({});
            const auto sorted = samples.statistics();
            cue::study::Options options;
            options.useNthStatistics = true;
            cue::study::configure(options);
            const auto selected = samples.statistics();
            if (sorted.sampleCount != selected.sampleCount || sorted.lastDuration != selected.lastDuration ||
                sorted.averageDuration != selected.averageDuration || sorted.p95Duration != selected.p95Duration ||
                sorted.maxDuration != selected.maxDuration)
            {
                return false;
            }
            // 空、単一、重複、昇順、降順、循環枠、負値、長い休止時間を含める
            const std::array<std::int64_t, 8> values{0,
                                                     7,
                                                     count,
                                                     260 - count,
                                                     (count * 7919) % 12347,
                                                     -static_cast<std::int64_t>(count),
                                                     count % 5,
                                                     (count % 3) * 1000000000000000LL};
            samples.add(std::chrono::nanoseconds(values[pattern]));
        }
    }
    cue::study::configure({});
    return true;
}

/// @brief 同じ Thread の User / Kernel CPU Cycle を取得する。経過時間への変換には使用しない
[[nodiscard]] std::uint64_t read_thread_cycles() noexcept
{
    ULONG64 cycles = 0;
    return QueryThreadCycleTime(GetCurrentThread(), &cycles) ? cycles : 0;
}

/// @brief 製品と同じ Host を一定 Frame 数だけ進め、成功・失敗のどちらも明示停止して回収する
[[nodiscard]] int measure(const ProbeOptions &a_options)
{
    cue::study::Options studyOptions;
    const bool isNext = a_options.caseName.starts_with("next-") || a_options.caseName.starts_with("new-");
    studyOptions.readoutStride = isNext || a_options.caseName.ends_with("readout15") ? 15 : 1;
    studyOptions.gpuTimestamps = !a_options.caseName.ends_with("gpuoff");
    studyOptions.pixMarkers = !a_options.caseName.ends_with("pixoff");
    const bool isCombined = !isNext && a_options.caseName.find("combined") != std::string::npos;
    studyOptions.statisticsOutsideLock = isCombined || a_options.caseName == "statunlock";
    studyOptions.cacheTimestampFrequency = isCombined || a_options.caseName == "freqcache";
    studyOptions.useNthStatistics = isCombined || a_options.caseName == "nthstats";
    studyOptions.gpuStatisticsStride = isCombined || a_options.caseName == "gpustats15" ? 15 : 1;
    studyOptions.preformatUiText = a_options.caseName.starts_with("preformat");
    studyOptions.waitBeforeUi = a_options.caseName == "next-ui-gate" || a_options.caseName == "next-combined" ||
                                a_options.caseName == "new-present-profile-gate";
    studyOptions.rotateCommandSlots = a_options.caseName == "next-command2" || a_options.caseName == "next-combined";
    studyOptions.nativeCycles = a_options.caseName == "next-cycles";
    studyOptions.cacheGraphPreparation = a_options.caseName == "new-cache" || a_options.caseName == "new-cache-buffer3";
    studyOptions.separateImGuiRecording = a_options.caseName == "new-snapshot";
    studyOptions.waitableSwapChain =
        a_options.caseName.starts_with("new-waitable") || a_options.caseName == "new-borderless-waitable" ||
        a_options.caseName == "new-swap-latency1" || a_options.caseName.starts_with("new-swap-buffer");
    // 条件ごとに一要因を分け、CPU の二枠を維持したまま表示受付の制限を比較する
    studyOptions.maximumPresentationLatency = a_options.caseName == "new-swap-latency1"           ? 1
                                              : a_options.caseName == "new-swap-buffer3-latency3" ? 3
                                              : a_options.caseName == "new-swap-buffer4-latency4" ? 4
                                                                                                  : 2;
    studyOptions.noPresentationScaling = a_options.caseName.find("swap-none") != std::string::npos;
    studyOptions.borderlessWindow = a_options.caseName.starts_with("new-borderless");
    studyOptions.descriptorLookup = a_options.caseName.starts_with("new-descriptor");
    studyOptions.presentDispatch = a_options.caseName.starts_with("new-present-profile");
    studyOptions.presentGpuWait = a_options.caseName == "new-present-profile-gpuwait";
    cue::study::configure(studyOptions);
    if (studyOptions.presentDispatch)
    {
        cue::dispatch_probe::calibrate();
        cue::study::set_dispatch_hooks(cue::dispatch_probe::begin_present, cue::dispatch_probe::end_present);
    }
    cue::study::set_cycle_reader(read_thread_cycles);
    if (studyOptions.nativeCycles && !read_thread_cycles())
        return 10;

    cue::EditorHostConfig config;
    config.window.title = "CueEngine Performance Study";
    // 各条件で同じ初期 Layout を使用し、既存 Editor 設定の読込・保存を観測に混ぜない
    config.imgui.settingsFile.clear();
    config.presentation.isTearingAllowed = a_options.caseName == "tearing" || a_options.caseName == "next-tearing" ||
                                           a_options.caseName == "new-borderless-swap-none-tearing";
    config.presentation.isVSyncEnabled = a_options.caseName == "next-vsync";
    config.frame.maxFramesInFlight = a_options.caseName == "next-frame1" ? 1 : 2;
    config.frame.useWorkerThreads =
        a_options.caseName != "next-main" && a_options.caseName != "new-present-profile-main";
    config.presentation.bufferCount = a_options.caseName == "next-buffer3" || a_options.caseName == "next-combined" ||
                                              a_options.caseName == "new-cache-buffer3" ||
                                              a_options.caseName.starts_with("new-swap-buffer3")
                                          ? 3
                                      : a_options.caseName == "new-swap-buffer4-latency4" ? 4
                                                                                          : 2;
    if (studyOptions.borderlessWindow || a_options.caseName == "new-screen-windowed")
    {
        // Borderless と同じ Pixel 数の通常 Window を別の基準として用意する
        config.window.clientSize = {static_cast<std::uint32_t>(GetSystemMetrics(SM_CXSCREEN)),
                                    static_cast<std::uint32_t>(GetSystemMetrics(SM_CYSCREEN))};
    }
    std::unique_ptr<ImTextureData[]> textures;
    if (a_options.caseName.ends_with("stress"))
    {
        textures = std::make_unique<ImTextureData[]>(256);
        config.imgui.rendererDescriptorCapacity = 257;
        config.buildUi = [data = textures.get(), isRegistered = false]() mutable -> cue::Result<void>
        {
            // Texture の Address は Host shutdown 完了まで固定する。外部画像 File を条件に混ぜない
            if (!isRegistered)
            {
                for (unsigned index = 0; index < 256; ++index)
                {
                    auto &texture = data[index];
                    texture.Create(ImTextureFormat_RGBA32, 4, 4);
                    for (unsigned pixel = 0; pixel < 16; ++pixel)
                    {
                        auto *rgba = texture.Pixels + pixel * 4;
                        rgba[0] = static_cast<unsigned char>(index);
                        rgba[1] = static_cast<unsigned char>(255 - index);
                        rgba[2] = 127;
                        rgba[3] = 255;
                    }
                    ImGui::RegisterUserTexture(&texture);
                }
                isRegistered = true;
            }
            ImGui::SetNextWindowPos({32, 32}, ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize({640, 480}, ImGuiCond_FirstUseEver);
            ImGui::Begin("Test");
            ImGui::TextUnformatted("TEST");
            const auto origin = ImGui::GetCursorScreenPos();
            for (unsigned index = 0; index < 256; ++index)
            {
                const ImVec2 point{origin.x + (index % 16) * 20.0f, origin.y + (index / 16) * 20.0f};
                ImGui::GetWindowDrawList()->AddImage(data[index].GetTexRef(), point, {point.x + 16, point.y + 16});
            }
            ImGui::Dummy({320, 320});
            ImGui::End();
            return cue::Result<void>::success();
        };
    }
    const auto frameConfig = config.frame;
    const auto presentationConfig = config.presentation;
    if (a_options.caseName.starts_with("minimal") || a_options.caseName == "new-present-profile-minimal")
    {
        config.buildUi = build_minimal_ui;
    }
    cue::EditorHost host(std::move(config));
    int status = 0;
    auto initialized = host.initialize();
    if (!initialized.has_value())
    {
        std::fprintf(stderr, "initialization failed: %s\n", initialized.try_error()->operation.c_str());
        status = 2;
    }
    if (status == 0 && a_options.caseName == "new-screen-windowed")
    {
        // 通常 Window の OS 自動寸法調整を比較から除外する。変更の反映は Warmup 中に終える
        const auto window = FindWindowW(nullptr, L"CueEngine Performance Study");
        RECT outer{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
        RECT client{};
        if (!window || !AdjustWindowRectEx(&outer, WS_OVERLAPPEDWINDOW, FALSE, 0) ||
            !SetWindowPos(window, nullptr, 0, 0, outer.right - outer.left, outer.bottom - outer.top,
                          SWP_NOZORDER | SWP_NOMOVE | SWP_NOSENDCHANGING) ||
            !GetClientRect(window, &client) || client.right != GetSystemMetrics(SM_CXSCREEN) ||
            client.bottom != GetSystemMetrics(SM_CYSCREEN))
        {
            std::fprintf(stderr, "windowed comparison size mismatch\n");
            status = 12;
        }
        else
        {
            auto info = cue::study::window();
            info.width = static_cast<std::uint32_t>(client.right);
            info.height = static_cast<std::uint32_t>(client.bottom);
            if (GetWindowRect(window, &outer))
            {
                info.outerWidth = outer.right - outer.left;
                info.outerHeight = outer.bottom - outer.top;
            }
            cue::study::set_window(info);
        }
    }
    cue::FrameProgress progress;
    cue::ImGuiFrameInfo ui;
    cue::ImGuiTransferInfo transfer;
    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + std::chrono::seconds(35);
    unsigned lifecycleStage = 0;
    const std::uint64_t endFrame = static_cast<std::uint64_t>(a_options.warmup) + a_options.frames;
    while (status == 0 && progress.renderedFrames < endFrame)
    {
        // 計測比較とは別の短い試験で、待機中の最小化・復帰・Resize・終了を確認する
        if (a_options.caseName == "new-waitable-lifecycle" || a_options.caseName == "new-swap-none-lifecycle")
        {
            const auto elapsed = std::chrono::steady_clock::now() - start;
            const auto window = FindWindowW(nullptr, L"CueEngine Performance Study");
            if (!window)
            {
                status = 10;
                break;
            }
            if (lifecycleStage == 0 && elapsed >= std::chrono::milliseconds(150))
            {
                ShowWindow(window, SW_MINIMIZE);
                ++lifecycleStage;
            }
            else if (lifecycleStage == 1 && elapsed >= std::chrono::milliseconds(300))
            {
                ShowWindow(window, SW_RESTORE);
                ++lifecycleStage;
            }
            else if (lifecycleStage == 2 && elapsed >= std::chrono::milliseconds(450))
            {
                if (!SetWindowPos(window, nullptr, 0, 0, 900, 700, SWP_NOZORDER | SWP_NOMOVE))
                {
                    status = 10;
                    break;
                }
                ++lifecycleStage;
            }
        }
        if (std::chrono::steady_clock::now() >= deadline)
        {
            std::fprintf(stderr, "measurement timed out\n");
            status = 3;
            break;
        }
        auto step = host.step();
        if (!step.has_value() || !*step.try_value())
        {
            std::fprintf(stderr, "Host step failed or Window closed\n");
            status = 4;
            break;
        }
        // 生の進行数だけを確認し、p95 の整列や ImGui Context Lock を観測 Loop に追加しない
        auto observed = host.frame_progress();
        if (!observed.has_value())
        {
            status = 5;
            break;
        }
        progress = *observed.try_value();
    }
    const auto wallSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    if ((a_options.caseName == "new-waitable-lifecycle" || a_options.caseName == "new-swap-none-lifecycle") &&
        lifecycleStage != 3)
    {
        status = 10;
    }
    if (initialized.has_value())
    {
        // 停止後に公開 API が無効になるため、表示 Data と転送回数だけは停止直前に保存する
        auto uiResult = host.ui_frame_info();
        auto transferResult = host.ui_transfer_info();
        if (uiResult.has_value())
        {
            ui = *uiResult.try_value();
        }
        if (transferResult.has_value())
        {
            transfer = *transferResult.try_value();
        }
    }
    auto stopped = host.shutdown();
    if (!stopped.has_value())
    {
        std::fprintf(stderr, "Host shutdown failed\n");
        return 6;
    }
    if (studyOptions.presentDispatch && !frameConfig.useWorkerThreads)
        cue::dispatch_probe::current_profile().release();
    if (!save_results(a_options, studyOptions, frameConfig, presentationConfig, progress, ui, transfer, wallSeconds,
                      status))
    {
        std::fprintf(stderr, "saving measurements failed\n");
        return 7;
    }
    std::printf("case=%s repeat=%u rendered=%llu wall=%.3fs dropped=%llu\n", a_options.caseName.c_str(),
                a_options.repeat, static_cast<unsigned long long>(progress.renderedFrames), wallSeconds,
                static_cast<unsigned long long>(cue::study::dropped_samples()));
    return status;
}
} // namespace

/// @brief 調査の条件を受け取り、一条件を測って終了する専用入口
int main(int a_count, char **a_arguments)
{
    try
    {
        ProbeOptions options;
        if (!parse_options(a_count, a_arguments, options))
        {
            std::fprintf(stderr, "Usage: CueEditorTimingProbe --case baseline|minimal|readout15|gpuoff|pixoff|tearing "
                                 "--warmup 120 --frames 600 --repeat 1 --output measurements.csv\n");
            return 1;
        }
        if (!verify_statistics())
        {
            std::fprintf(stderr, "statistics selection disagrees with sorted baseline\n");
            return 9;
        }
        // PID 指定 Capture の起動だけを待つ。Host 初期化前で、計測区間に File IO は入れない
        if (!options.captureReady.empty())
        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (!std::filesystem::exists(options.captureReady))
            {
                if (std::chrono::steady_clock::now() >= deadline)
                    return 11;
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        }
        return measure(options);
    }
    catch (const std::exception &a_error)
    {
        std::fprintf(stderr, "measurement exception: %s\n", a_error.what());
    }
    catch (...)
    {
        std::fprintf(stderr, "unexpected measurement exception\n");
    }
    return 8;
}
