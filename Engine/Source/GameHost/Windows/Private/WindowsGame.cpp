#include <GameHost/Windows/WindowsGame.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <DX12/DX12Backend.h>
#include <DX12/DX12RenderDevice.h>
#include <Foundation/Windows/UtfConversion.h>
#include <GameRuntime/GameSession.h>
#include <GameRuntime/GameStartup.h>
#include <Platform/Diagnostics.h>
#include <Platform/Windows/WindowsFileSystem.h>
#include <Scene/Scene.h>
#include <SceneRender/SceneRenderer.h>
#include <ScriptRuntime/ScriptModule.h>
#include <WindowsHost/WindowsHost.h>

#include "AppValidation.h"

namespace cue
{
namespace
{
/// @brief 配布 File の欠落を Repository への Fallback で隠さず、起動前に拒否する
Result<void> validate_resources(IFileSystem &a_files, const Path &a_directory)
{
    constexpr std::array<const char *, 4> required{"dxcompiler.dll", "dxil.dll",
                                                   "EngineResources/Shader/Hlsl/StaticMesh.hlsl",
                                                   "EngineResources/Shader/Hlsl/FullscreenTriangle.hlsl"};
    for (const auto *name : required)
    {
        auto file = a_directory.join(name);
        if (!file.has_value())
            return Result<void>::failure(*file.try_error());
        auto info = a_files.stat(*file.try_value());
        if (!info.has_value())
            return Result<void>::failure(*info.try_error());
        if (!*info.try_value() || (**info.try_value()).type != FileType::Regular)
            return Result<void>::failure(
                {ErrorCategory::InvalidArgument, "GameApp.resource_missing:" + file.try_value()->utf8()});
    }
    return Result<void>::success();
}

/// @brief 起動・実行失敗を共通診断へ送り、通常起動の失敗だけ Native Dialog で示す
void report_failure(const Error &a_error, bool a_showDialog) noexcept
{
    report_log_error("GameApp", a_error, LogLevel::Error);
    std::fprintf(stderr, "GameApp: %s (%lld)\n", a_error.operation.c_str(),
                 static_cast<long long>(a_error.nativeCode));
    if (a_showDialog)
    {
        try
        {
            auto text = utf8_to_utf16("Game could not run.\n" + a_error.operation);
            MessageBoxW(nullptr, text.has_value() ? text.try_value()->c_str() : L"Game could not run.",
                        L"CueEngine Game", MB_OK | MB_ICONERROR);
        }
        catch (...)
        {
            MessageBoxW(nullptr, L"Game could not run.", L"CueEngine Game", MB_OK | MB_ICONERROR);
        }
    }
}

/// @brief Script 診断は Host の配送先へ明示的に接続し、Runtime に Native 出力を持たせない
void report_script(std::uint32_t a_severity, std::string_view a_message)
{
    const auto level = a_severity == 0   ? LogLevel::Info
                       : a_severity == 1 ? LogLevel::Warning
                       : a_severity == 2 ? LogLevel::Error
                                         : LogLevel::Fatal;
    report_log("GameScript", std::string(a_message).c_str(), level);
}

/// @brief Platform Host の Frame 制御へ実行 Session と所有 Render Snapshot を接続する
Result<void> run_frames(GameSession &a_session, const GameStartupConfig &a_startup,
                        const AppValidationOptions &a_options, AppValidationMetrics &a_metrics)
{
    std::unique_ptr<ISceneRenderer> renderer;
    WindowsHostConfig config;
    config.window = {a_startup.windowTitle, {a_startup.windowWidth, a_startup.windowHeight}};
    // 初期の製品 Host は World Owner を Main と Update で共用し、Thread 移譲を行わない
    config.frame = {1, false, a_startup.targetFramesPerSecond};
    config.gpu.resourceFrameCount = 1;
    config.gpu.maxFramesInFlight = 1;
    config.simulation.frequencyHz = 60;
    config.storage.mode = StorageMode::Product;
    config.storage.applicationName = a_startup.applicationName;
    if (a_options.isRequested)
    {
        config.logging.isFileEnabled = false;
        config.logging.debugOutput = DebugLogOutput::Disabled;
    }
    if (a_options.isWarp)
    {
        config.callbacks.createBackend = [](IFileSystem *a_files, Clock &a_clock)
        {
            auto backend = dx12::DX12Backend::create(dx12::AdapterSelection::Warp, {}, a_files, &a_clock);
            return backend.has_value() ? Result<std::unique_ptr<IBackend>>::success(backend.take_value())
                                       : Result<std::unique_ptr<IBackend>>::failure(*backend.try_error());
        };
    }
    config.callbacks.initializeRenderer = [&](IBackend &a_backend, std::uint32_t, std::uint32_t)
    {
        auto created = create_scene_renderer(a_backend);
        if (!created.has_value())
            return Result<void>::failure(*created.try_error());
        renderer = created.take_value();
        return Result<void>::success();
    };
    config.graph.configure = [&](FrameGraph &a_graph, FrameGraphResourceHandle)
    {
        auto pass = renderer->create_pass();
        if (!pass.has_value())
            return Result<void>::failure(*pass.try_error());
        auto registered = a_graph.add_pass(pass.take_value());
        return registered.has_value() ? Result<void>::success() : Result<void>::failure(*registered.try_error());
    };
    config.callbacks.simulationUpdate = [&](const SimulationStep &a_step, std::stop_token)
    { return a_session.update(std::chrono::duration_cast<std::chrono::nanoseconds>(a_step.deltaTime)); };
    config.callbacks.prepareFrame = [&](const FrameSimulationInfo &a_frame, std::stop_token)
    {
        auto snapshot = a_session.snapshot();
        if (!snapshot.has_value())
            return Result<void>::failure(*snapshot.try_error());
        observe_app_snapshot(a_metrics, *snapshot.try_value());
        return renderer->publish_frame(a_frame.frameIndex, snapshot.take_value());
    };
    config.callbacks.recordFrame = [&](std::uint64_t a_frame, std::stop_token a_stop, const FrameCallback &a_record)
    { return renderer->render_frame(a_frame, [&]() { return a_record(a_frame, a_stop); }); };
    config.callbacks.reconfigureRenderer = [&](IBackend &, std::uint32_t, std::uint32_t)
    { return renderer->discard_pending(); };
    config.callbacks.shutdownWindow = [&]() { return renderer ? renderer->shutdown() : Result<void>::success(); };
    WindowsHost host(std::move(config));
    auto initialized = host.initialize();
    if (!initialized.has_value())
        return initialized;
    std::optional<Error> failure;
    while (true)
    {
        auto stepped = host.step();
        if (!stepped.has_value())
        {
            failure = *stepped.try_error();
            break;
        }
        auto progress = host.frame_progress();
        if (!progress.has_value())
        {
            failure = *progress.try_error();
            break;
        }
        a_metrics.completedRenders = progress.try_value()->renderedFrames;
        if (!*stepped.try_value() || (a_options.maxFrames != 0 && a_metrics.completedRenders >= a_options.maxFrames))
            break;
    }
    // Worker / GPU の停止を先に完了し、後で Script と World を停止する
    auto stopped = host.shutdown();
    if (!stopped.has_value())
    {
        if (failure)
            report_failure(*stopped.try_error(), false);
        else
            failure = *stopped.try_error();
    }
    return failure ? Result<void>::failure(*failure) : Result<void>::success();
}
} // namespace

int run_windows_game(CueScriptGetModuleFunction a_entry, const WindowsGameBuildIdentity &a_identity) noexcept
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    try
    {
        auto options = read_app_validation_options();
        if (!options.has_value())
        {
            report_failure(*options.try_error(), false);
            return 1;
        }
        auto files = create_windows_file_system();
        if (!files.has_value())
        {
            report_failure(*files.try_error(), !options.try_value()->isRequested);
            return 1;
        }
        AppValidationMetrics metrics;
        auto executed = [&]() -> Result<void>
        {
            auto directory = (*files.try_value())->executable_directory();
            if (!directory.has_value())
                return Result<void>::failure(*directory.try_error());
            auto startup = load_game_startup(**files.try_value(), *directory.try_value());
            if (!startup.has_value())
                return Result<void>::failure(*startup.try_error());
            if (startup.try_value()->platform != a_identity.platform ||
                startup.try_value()->architecture != a_identity.architecture ||
                startup.try_value()->configuration != a_identity.configuration)
                return Result<void>::failure({ErrorCategory::InvalidArgument, "GameApp.build_identity"});
            auto resources = validate_resources(**files.try_value(), *directory.try_value());
            if (!resources.has_value())
                return resources;
            auto sceneFile = directory.try_value()->join(startup.try_value()->startupScene);
            if (!sceneFile.has_value())
                return Result<void>::failure(*sceneFile.try_error());
            auto scene = Scene::load(**files.try_value(), *sceneFile.try_value());
            if (!scene.has_value())
                return Result<void>::failure(*scene.try_error());
            auto module = RuntimeGameModule::from_static(a_entry);
            if (!module.has_value())
                return Result<void>::failure(*module.try_error());
            metrics.isStaticRegistration = true;
            auto created = GameSession::create(*scene.try_value(), module.take_value(), report_script);
            if (!created.has_value())
                return Result<void>::failure(*created.try_error());
            auto session = created.take_value();
            auto initial = session->snapshot();
            if (!initial.has_value())
                return Result<void>::failure(*initial.try_error());
            observe_app_snapshot(metrics, *initial.try_value());
            auto played = options.try_value()->isValidateOnly
                              ? Result<void>::success()
                              : run_frames(*session, *startup.try_value(), *options.try_value(), metrics);
            metrics.completedSimulationTicks = session->completed_ticks();
            auto stopped = session->shutdown();
            metrics.hasNormalShutdown = played.has_value() && stopped.has_value();
            if (!played.has_value())
            {
                if (!stopped.has_value())
                    report_failure(*stopped.try_error(), false);
                return played;
            }
            return stopped;
        }();
        auto report = write_app_validation_report(**files.try_value(), *options.try_value(), metrics);
        if (!report.has_value())
        {
            report_failure(*report.try_error(), !options.try_value()->isRequested);
            if (executed.has_value())
                return 1;
        }
        if (!executed.has_value())
        {
            report_failure(*executed.try_error(), !options.try_value()->isRequested);
            return 1;
        }
        return 0;
    }
    catch (...)
    {
        report_failure({ErrorCategory::Fatal, "GameApp.unexpected_exception"}, false);
        return 2;
    }
}
} // namespace cue
