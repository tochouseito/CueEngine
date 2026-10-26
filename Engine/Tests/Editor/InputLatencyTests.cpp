#include <EditorHost/EditorHost.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <limits>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <imgui.h>

#include <Platform/Windows/WindowsPlatform.h>
#include <WindowsHost/InputLatency.h>

#include "../Support/TemporaryFiles.h"

namespace
{
/// @brief 時刻取得の迂回を検出するため、実時計と異なる時間軸を全 Thread へ返す
class OffsetClock final : public cue::Clock
{
  public:
    /// @brief 経過速度を維持したまま原点を一日ずらす
    [[nodiscard]] std::chrono::steady_clock::time_point now() const noexcept override
    {
        samples.fetch_add(1, std::memory_order_relaxed);
        return cue::get_default_clock().now() + std::chrono::hours(24);
    }

    mutable std::atomic<std::uint64_t> samples = 0;
};

/// @brief 実 Clock に依存せず入力イベント単位の分布、無効値と非表示 Frame を検証する
int test_samples()
{
    using namespace std::chrono_literals;
    cue::InputLatencySamples samples;
    const auto origin = std::chrono::steady_clock::time_point{};
    const std::array<cue::InputEventTiming, 2> events{{{10, origin + 1ms}, {11, origin + 2ms}}};
    samples.assigned(2, 2);
    samples.presented(events, 5, 7, origin + 3ms, origin + 4ms, origin + 5ms, origin + 6ms, origin + 8ms);
    auto info = samples.info();
    if (info.toPresentStart.sampleCount != 2 || info.toPresentStart.averageDuration != 3500us ||
        info.toPresentStart.p95Duration != 4ms || info.toCpuComplete.maxDuration != 7ms || info.observedEvents != 2 ||
        info.presentedEvents != 2 || info.lastEventId != 11 || info.lastFrame != 5 || info.lastPresent != 7)
    {
        return __LINE__;
    }
    samples.presented({}, 6, 8, origin, origin, origin, origin, origin);
    const std::array<cue::InputEventTiming, 1> invalid{{{12, origin + 20ms}}};
    samples.presented(invalid, 6, 8, origin, origin, origin, origin, origin);
    samples.skipped(3);
    info = samples.info();
    if (info.toCpuComplete.sampleCount != 2 || info.invalidEvents != 1 || info.skippedEvents != 3 ||
        info.lastFrame != 5)
    {
        return __LINE__;
    }
    return 0;
}

/// @brief ImGui が Press / Release を別 Frame に持ち越しても到達時刻と Event ID を保つ
int test_trickled_input()
{
    auto created = cue::create_windows_window_system();
    if (!created.has_value())
    {
        return __LINE__;
    }
    auto system = created.take_value();
    auto made = system->create_window({"Cue Input Trickle Test", {320, 240}});
    if (!made.has_value())
    {
        return __LINE__;
    }
    auto window = made.take_value();
    auto handle = cue::borrow_windows_window_handle(*window);
    cue::ImGuiManagerConfig config;
    config.settingsFile.clear();
    auto madeUi = cue::ImGuiManager::create(*window, std::move(config));
    if (!handle.has_value() || !madeUi.has_value())
    {
        return __LINE__;
    }
    auto ui = madeUi.take_value();
    const auto hwnd = static_cast<HWND>(*handle.try_value());
    SendMessageW(hwnd, WM_SETFOCUS, 0, 0);
    if (!ui->build_frame({}).has_value())
    {
        return __LINE__;
    }
    std::vector<cue::InputEventTiming> raw;
    window->take_input_events(raw);
    SendMessageW(hwnd, WM_KEYDOWN, 'A', 1);
    SendMessageW(hwnd, WM_KEYUP, 'A', 1);
    window->take_input_events(raw);
    if (raw.size() != 2 || raw[0].eventId >= raw[1].eventId || raw[0].received > raw[1].received)
    {
        return __LINE__;
    }
    std::vector<cue::InputEventTiming> consumed;
    bool pressed = false, released = false;
    if (!ui->build_frame(
               [&]()
               {
                   pressed = ImGui::IsKeyPressed(ImGuiKey_A, false);
                   return cue::Result<void>::success();
               })
             .has_value() ||
        !ui->take_input_events(consumed).has_value() || consumed.size() != 1 || consumed[0].eventId != raw[0].eventId ||
        consumed[0].received != raw[0].received || !pressed)
    {
        return __LINE__;
    }
    if (!ui->build_frame(
               [&]()
               {
                   released = ImGui::IsKeyReleased(ImGuiKey_A);
                   return cue::Result<void>::success();
               })
             .has_value() ||
        !ui->take_input_events(consumed).has_value() || consumed.size() != 1 || consumed[0].eventId != raw[1].eventId ||
        consumed[0].received != raw[1].received || !released)
    {
        return __LINE__;
    }
    if (!ui->take_input_events(consumed).has_value() || !consumed.empty() || !ui->shutdown().has_value() ||
        !window->destroy().has_value() || !system->pump_events().has_value())
    {
        return __LINE__;
    }
    return 0;
}

/// @brief 実 EditorHost の各 Buffering / Waitable 経路で入力を反映した Frame と Present を確認する
int test_host(std::uint32_t a_buffers, bool a_waitable, bool a_rendersUi = true)
{
    OffsetClock clock;
    cue::tests::TemporaryFiles temporary;
    if (!temporary.isCreated)
    {
        return __LINE__;
    }
    cue::EditorHostConfig config;
    config.clock = &clock;
    config.window = {"Cue Input Latency Host Test", {320, 240}};
    config.imgui.settingsFile.clear();
    config.storage.dataRootOverride = temporary.root;
    config.frame.maxFps = 0;
    config.gpu = {a_buffers, a_buffers};
    config.presentation.isWaitableEnabled = a_waitable;
    config.presentation.isTearingAllowed = true;
    if (!a_rendersUi)
    {
        config.graph.displayPassFactory = []() { return std::make_unique<cue::PresentToSwapChainPass>(); };
    }
    std::uint64_t uiFrames = 0;
    std::uint64_t releaseFrame = (std::numeric_limits<std::uint64_t>::max)();
    config.buildUi = [&]()
    {
        if (ImGui::IsKeyReleased(ImGuiKey_A))
        {
            releaseFrame = uiFrames;
        }
        ++uiFrames;
        ImGui::Begin("Input Test");
        ImGui::Text("Frame %llu", static_cast<unsigned long long>(uiFrames));
        ImGui::End();
        return cue::Result<void>::success();
    };
    cue::EditorHost host(std::move(config));
    if (!host.initialize().has_value())
    {
        return __LINE__;
    }
    const auto hwnd = FindWindowW(nullptr, L"Cue Input Latency Host Test");
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!hwnd || pid != GetCurrentProcessId())
    {
        return __LINE__;
    }
    const auto warmup = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (uiFrames < 30 && std::chrono::steady_clock::now() < warmup)
    {
        auto step = host.step();
        if (!step.has_value() || !*step.try_value())
        {
            return __LINE__;
        }
    }
    SendMessageW(hwnd, WM_SETFOCUS, 0, 0);
    SendMessageW(hwnd, WM_KEYDOWN, 'A', 1);
    SendMessageW(hwnd, WM_KEYUP, 'A', 1);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    bool measured = false;
    while (std::chrono::steady_clock::now() < deadline)
    {
        auto step = host.step();
        auto info = host.input_latency_info();
        if (!step.has_value() || !*step.try_value() || !info.has_value())
        {
            return __LINE__;
        }
        const auto &value = *info.try_value();
        if (!a_rendersUi && value.skippedEvents >= 2)
        {
            return value.presentedEvents == 0 && value.toCpuComplete.sampleCount == 0 && host.shutdown().has_value()
                       ? 0
                       : __LINE__;
        }
        if (a_rendersUi && value.presentedEvents >= 2 && value.lastFrame == releaseFrame)
        {
            if (value.invalidEvents != 0 || value.toPresentStart.sampleCount < 2 ||
                value.toPresentStart.lastDuration > value.toPresentReturn.lastDuration ||
                value.toPresentReturn.lastDuration > value.toCpuComplete.lastDuration || value.lastPresent == 0 ||
                value.toCpuComplete.lastDuration >= std::chrono::seconds(5) || clock.samples.load() == 0)
            {
                return __LINE__;
            }
            std::printf("buffers=%u waitable=%d events=%llu frame=%llu present=%llu input_cpu_ms=%.3f\n", a_buffers,
                        a_waitable, static_cast<unsigned long long>(value.presentedEvents),
                        static_cast<unsigned long long>(value.lastFrame),
                        static_cast<unsigned long long>(value.lastPresent),
                        std::chrono::duration<double, std::milli>(value.toCpuComplete.lastDuration).count());
            measured = true;
            break;
        }
    }
    return measured && host.shutdown().has_value() ? 0 : __LINE__;
}
/// @brief 入力 Consumer のない Host は受信件数だけを数え、架空の入力遅延を公開しない
int test_without_consumer()
{
    cue::tests::TemporaryFiles temporary;
    if (!temporary.isCreated)
    {
        return __LINE__;
    }
    cue::WindowsHostConfig config;
    config.window = {"Cue Input Without Consumer Test", {320, 240}};
    config.storage.dataRootOverride = temporary.root;
    config.frame = {1, false, 0};
    config.gpu = {1, 1};
    config.presentation.isWaitableEnabled = false;
    config.presentation.isTearingAllowed = true;
    config.callbacks.initializeWindow = [](cue::Window &a_window)
    {
        auto handle = cue::borrow_windows_window_handle(a_window);
        if (!handle.has_value())
        {
            return cue::Result<void>::failure(*handle.try_error());
        }
        const auto hwnd = static_cast<HWND>(*handle.try_value());
        SendMessageW(hwnd, WM_KEYDOWN, 'B', 1);
        SendMessageW(hwnd, WM_KEYUP, 'B', 1);
        return cue::Result<void>::success();
    };
    cue::WindowsHost host(std::move(config));
    if (!host.initialize().has_value())
    {
        return __LINE__;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline)
    {
        auto step = host.step();
        auto timing = host.input_latency_info();
        if (!step.has_value() || !*step.try_value() || !timing.has_value())
        {
            return __LINE__;
        }
        if (timing.try_value()->observedEvents >= 2)
        {
            return timing.try_value()->assignedEvents == 0 && timing.try_value()->toCpuComplete.sampleCount == 0 &&
                           host.shutdown().has_value()
                       ? 0
                       : __LINE__;
        }
    }
    return __LINE__;
}
} // namespace

/// @brief 数値、入力の持越しと実 Host の Main / Worker 両経路を検証する
int main()
{
    if (const auto result = test_samples(); result != 0)
    {
        return result;
    }
    if (const auto result = test_trickled_input(); result != 0)
    {
        return result;
    }
    for (const auto buffers : {1u, 2u, 3u})
    {
        for (const bool waitable : {false, true})
        {
            if (const auto result = test_host(buffers, waitable); result != 0)
            {
                return result;
            }
        }
    }
    if (const auto result = test_host(2, false, false); result != 0)
    {
        return result;
    }
    return test_without_consumer();
}
