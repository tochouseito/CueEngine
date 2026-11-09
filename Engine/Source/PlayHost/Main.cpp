#include <charconv>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// Shell API は Win32 の型宣言後に Include する
#include <shellapi.h>

#include <EngineInstallation/EngineInstallation.h>
#include <Foundation/Windows/UtfConversion.h>
#include <GameRuntime/GameSession.h>
#include <Platform/Diagnostics.h>
#include <Platform/Windows/WindowsFileSystem.h>
#include <Platform/Windows/WindowsPlatform.h>
#include <Runtime/Components/TransformComponent.h>
#include <Runtime/FrameController.h>
#include <Runtime/World.h>
#include <Scene/Scene.h>
#include <SceneRender/SceneRenderer.h>
#include <ScriptPlay/PlayServer.h>
#include <ScriptRuntime/ScriptModule.h>
#include <WindowsHost/WindowsHost.h>

namespace
{
struct Options final
{
    std::string pipeName;
    cue::Path module;
    std::uint64_t session = 0;
    std::uint32_t maxTicks = 0;
    bool isDescribe = false;
    bool isHeadless = false;
    bool isEmbedded = false;
    bool isDebug = false;
};

/// @brief OS の Command Line 分割を利用し、未知引数・欠損値・重複を拒否する
cue::Result<Options> read_options()
{
    int count = 0;
    auto **arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!arguments)
        return cue::Result<Options>::failure({cue::ErrorCategory::PlatformFailure, "Play.arguments", GetLastError()});
    const auto release = [](wchar_t **a_values) { LocalFree(a_values); };
    std::unique_ptr<wchar_t *, decltype(release)> owned(arguments, release);
    Options options;
    bool hasMaxTicks = false;
    for (int index = 1; index < count; ++index)
    {
        auto argument = cue::utf16_to_utf8(arguments[index]);
        if (!argument.has_value())
            return cue::Result<Options>::failure(*argument.try_error());
        const auto &key = *argument.try_value();
        if (key == "--describe" && !options.isDescribe)
        {
            options.isDescribe = true;
            continue;
        }
        if (key == "--headless" && !options.isHeadless)
        {
            options.isHeadless = true;
            continue;
        }
        if (key == "--embedded" && !options.isEmbedded)
        {
            options.isEmbedded = true;
            continue;
        }
        if (key == "--debug-wait" && !options.isDebug)
        {
            options.isDebug = true;
            continue;
        }
        if (++index >= count)
            return cue::Result<Options>::failure({cue::ErrorCategory::InvalidArgument, "Play.argument_value"});
        auto value = cue::utf16_to_utf8(arguments[index]);
        if (!value.has_value())
            return cue::Result<Options>::failure(*value.try_error());
        const auto &text = *value.try_value();
        if (key == "--pipe" && options.pipeName.empty())
            options.pipeName = text;
        else if (key == "--module" && options.module.is_empty())
        {
            auto module = cue::Path::create(text);
            if (!module.has_value() || !module.try_value()->is_absolute())
                return cue::Result<Options>::failure({cue::ErrorCategory::InvalidArgument, "Play.module_path"});
            options.module = module.take_value();
        }
        else if (key == "--session" && options.session == 0)
        {
            const auto result = std::from_chars(text.data(), text.data() + text.size(), options.session);
            if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || !options.session)
                return cue::Result<Options>::failure({cue::ErrorCategory::InvalidArgument, "Play.session"});
        }
        else if (key == "--max-ticks" && !hasMaxTicks)
        {
            const auto result = std::from_chars(text.data(), text.data() + text.size(), options.maxTicks);
            if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || !options.maxTicks)
                return cue::Result<Options>::failure({cue::ErrorCategory::InvalidArgument, "Play.max_ticks"});
            hasMaxTicks = true;
        }
        else
            return cue::Result<Options>::failure({cue::ErrorCategory::InvalidArgument, "Play.argument"});
    }
    if (options.pipeName.empty() || options.module.is_empty() || !options.session ||
        (options.isDescribe && options.isDebug) || (options.isEmbedded && !options.isHeadless))
        return cue::Result<Options>::failure({cue::ErrorCategory::InvalidArgument, "Play.required_arguments"});
    return cue::Result<Options>::success(std::move(options));
}

/// @brief 終了時は短い期限で応答を排出する。切断済みなら追加の待機をしない
void drain(cue::PlayServer &a_server, std::uint32_t a_timeoutMilliseconds = 500)
{
    const auto deadline = GetTickCount64() + a_timeoutMilliseconds;
    while (a_server.has_pending_output() && GetTickCount64() < deadline)
    {
        auto polled = a_server.poll();
        if (!polled.has_value())
            break;
        if (a_server.has_pending_output())
            Sleep(1);
    }
}

/// @brief Game / World の通常失敗を親へ返す。Editor の Process に例外や終了を伝播しない
int fail(cue::PlayServer &a_server, const cue::Error &a_error)
{
    auto queued = a_server.send_error(a_error.operation + " (" + std::to_string(a_error.nativeCode) + ")");
    (void)queued;
    drain(a_server);
    return 1;
}

/// @brief 更新確定後の描画値と既知 Object の姿勢を複写し、Game / World の参照を Process 外へ渡さない
cue::Result<cue::PlayViewSnapshot> capture_view(cue::GameSession &a_session, const cue::Scene &a_scene)
{
    auto render = a_session.snapshot();
    if (!render.has_value())
        return cue::Result<cue::PlayViewSnapshot>::failure(*render.try_error());
    cue::PlayViewSnapshot view;
    view.tick = a_session.completed_ticks();
    view.render = render.take_value();
    auto *world = a_session.world();
    if (!world)
        return cue::Result<cue::PlayViewSnapshot>::failure({cue::ErrorCategory::InvalidState, "Play.view.world"});
    // 現行の Script API が扱う開始 Scene の Object だけを識別し、削除済み Object は公開しない
    for (const auto &object : a_scene.descriptor().objects)
    {
        if (!object.transform)
            continue;
        auto entity = world->find_by_id(a_session.instance(), object.objectId);
        if (!entity.has_value())
            continue;
        auto transform = world->get_component<cue::TransformComponent>(*entity.try_value());
        if (transform.has_value())
            view.transforms.push_back({object.objectId, (*transform.try_value())->local()});
    }
    return cue::Result<cue::PlayViewSnapshot>::success(std::move(view));
}

/// @brief Play の更新境界で姿勢を変更する。拒否できる入力失敗では実行 World を変更しない
cue::Result<void> set_play_transform(cue::GameSession &a_session, const cue::PlayCommand &a_command)
{
    auto *world = a_session.world();
    if (!world)
        return cue::Result<void>::failure({cue::ErrorCategory::InvalidState, "Play.transform.world"});
    auto entity = world->find_by_id(a_session.instance(), a_command.objectId);
    if (!entity.has_value())
        return cue::Result<void>::failure(*entity.try_error());
    auto transform = world->get_component<cue::TransformComponent>(*entity.try_value());
    if (!transform.has_value())
        return cue::Result<void>::failure(*transform.try_error());
    return (*transform.try_value())->set_local(a_command.transform);
}

/// @brief 親の開始許可と Debugger 接続を両方満たすまで、Game DLL を読まずに待つ
/// 待機中も IPC の Stop / 親切断を処理する。Scene は値で保持し、外部の World を作らない
cue::Result<std::optional<std::string>> wait_for_debugger(cue::PlayServer &a_server)
{
    using WaitResult = cue::Result<std::optional<std::string>>;
    auto announced = a_server.send_debug_waiting();
    if (!announced.has_value())
        return WaitResult::failure(*announced.try_error());
    const auto deadline = GetTickCount64() + 120000;
    std::optional<std::string> scene;
    while (GetTickCount64() < deadline || IsDebuggerPresent())
    {
        auto commands = a_server.poll();
        if (!commands.has_value())
            return WaitResult::failure(*commands.try_error());
        for (auto &command : *commands.try_value())
        {
            if (command.kind == cue::PlayCommand::Kind::Stop)
                return WaitResult::success(std::nullopt);
            if (command.kind == cue::PlayCommand::Kind::Start && !scene)
                scene = std::move(command.sceneText);
            else if (command.kind == cue::PlayCommand::Kind::ContinueDebug && scene && IsDebuggerPresent())
                return WaitResult::success(std::move(scene));
            else
                return WaitResult::failure({cue::ErrorCategory::InvalidState, "Play.debug_command"});
        }
        Sleep(1);
    }
    return WaitResult::failure({cue::ErrorCategory::InvalidState, "Play.debugger_timeout"});
}

/// @brief Snapshot と Game Module の所有を子 Process に閉じて実行する
int run(const Options &a_options, cue::PlayServer &a_server)
{
    // Metadata 子と Headless 子も含め、ゲーム DLL より先に Engine の利用権を確保する
    auto installationFiles = cue::create_windows_file_system();
    auto developmentRoot = cue::Path::create(CUE_REPOSITORY_ROOT);
    if (!installationFiles.has_value() || !developmentRoot.has_value())
        return fail(a_server,
                    !installationFiles.has_value() ? *installationFiles.try_error() : *developmentRoot.try_error());
    auto installation = cue::resolve_engine_installation(**installationFiles.try_value(), *developmentRoot.try_value());
    if (!installation.has_value())
        return fail(a_server, *installation.try_error());
    auto engineLease = cue::acquire_engine_use_lease(**installationFiles.try_value(), *installation.try_value());
    if (!engineLease.has_value())
        return fail(a_server, *engineLease.try_error());
    std::optional<std::string> debugScene;
    if (a_options.isDebug)
    {
        // LoadLibrary の静的初期化や Module 登録も接続後に置き、開始時の Breakpoint を取り逃さない
        auto waiting = wait_for_debugger(a_server);
        if (!waiting.has_value())
            return fail(a_server, *waiting.try_error());
        debugScene = waiting.take_value();
        if (!debugScene)
            return 0;
    }
    auto module = cue::RuntimeGameModule::load(a_options.module);
    if (!module.has_value())
        return fail(a_server, *module.try_error());
    auto described = a_server.send_types((*module.try_value())->types());
    if (!described.has_value())
        return fail(a_server, *described.try_error());
    if (a_options.isDescribe)
    {
        // 初回 Editor 起動の GPU 準備中も、大きい Metadata を途中で切断しない
        drain(a_server, 12000);
        return a_server.has_pending_output() ? 1 : 0;
    }
    // 未保存 Scene を親から受け取る。Game DLL を読み込むのはこの子 Process だけ
    std::optional<cue::Scene> scene;
    if (debugScene)
    {
        auto parsed = cue::Scene::parse(*debugScene);
        if (!parsed.has_value())
            return fail(a_server, *parsed.try_error());
        scene = parsed.take_value();
    }
    const auto deadline = GetTickCount64() + 15000;
    while (!scene && GetTickCount64() < deadline)
    {
        auto commands = a_server.poll();
        if (!commands.has_value())
            return fail(a_server, *commands.try_error());
        for (const auto &command : *commands.try_value())
        {
            if (command.kind == cue::PlayCommand::Kind::Stop)
                return 0;
            if (command.kind != cue::PlayCommand::Kind::Start)
                return fail(a_server, {cue::ErrorCategory::InvalidArgument, "Play.expected_scene"});
            auto parsed = cue::Scene::parse(command.sceneText);
            if (!parsed.has_value())
                return fail(a_server, *parsed.try_error());
            scene = parsed.take_value();
        }
        if (!scene)
            Sleep(1);
    }
    if (!scene)
        return fail(a_server, {cue::ErrorCategory::InvalidState, "Play.scene_timeout"});
    auto createdSession =
        cue::GameSession::create(*scene, module.take_value(),
                                 [](std::uint32_t a_severity, std::string_view a_message)
                                 {
                                     const auto level = a_severity == 0   ? cue::LogLevel::Info
                                                        : a_severity == 1 ? cue::LogLevel::Warning
                                                        : a_severity == 2 ? cue::LogLevel::Error
                                                                          : cue::LogLevel::Fatal;
                                     cue::report_log("GameScript", std::string(a_message).c_str(), level);
                                 });
    if (!createdSession.has_value())
        return fail(a_server, *createdSession.try_error());
    auto session = createdSession.take_value();

    bool shouldStop = false;
    std::uint64_t ticks = 0;
    std::optional<std::uint64_t> pendingView;
    // IPC は World の更新中に適用せず、Owner Thread 上の次の更新前で確定する
    const auto processCommands = [&]() -> cue::Result<void>
    {
        auto commands = a_server.poll();
        if (!commands.has_value())
            return cue::Result<void>::failure(*commands.try_error());
        for (const auto &command : *commands.try_value())
        {
            if (command.kind == cue::PlayCommand::Kind::Stop)
            {
                shouldStop = true;
                continue;
            }
            if (command.kind == cue::PlayCommand::Kind::RequestView)
            {
                if (!a_options.isEmbedded || pendingView)
                    return cue::Result<void>::failure({cue::ErrorCategory::InvalidState, "Play.view.request"});
                pendingView = command.requestId;
                continue;
            }
            if (command.kind == cue::PlayCommand::Kind::SetTransform)
            {
                auto changed = set_play_transform(*session, command);
                cue::PlayTransformAck ack;
                ack.requestId = command.requestId;
                ack.objectId = command.objectId;
                ack.transform = command.transform;
                ack.isAccepted = changed.has_value();
                if (!changed.has_value())
                    ack.error = changed.try_error()->operation;
                auto queued = a_server.send_transform_ack(ack);
                if (!queued.has_value())
                    return queued;
                continue;
            }
            if (command.kind != cue::PlayCommand::Kind::SetProperty)
                return cue::Result<void>::failure({cue::ErrorCategory::InvalidArgument, "Play.unexpected_start"});
            cue::PlayPropertyAck ack;
            ack.requestId = command.requestId;
            ack.objectId = command.objectId;
            ack.scriptInstanceId = command.scriptInstanceId;
            ack.value = command.value;
            auto applied = session->set_property(command.objectId, command.scriptInstanceId, command.value);
            ack.isAccepted = applied.has_value();
            if (!applied.has_value())
                ack.error = applied.try_error()->operation;
            auto queued = a_server.send_ack(ack);
            if (!queued.has_value())
                return queued;
            // Setter 内の異常で部分変更された Session を継続しない
            if (!applied.has_value() && !session->world())
                return applied;
        }
        return cue::Result<void>::success();
    };

    if (a_options.isHeadless)
    {
        // Window のない Editor Play でも FPS 制御は共通 FrameController に任せる
        // 単一 Owner 上で更新・IPC・抽出を直列化し、Debugger 停止後の大量追い付きは行わない
        auto createdServices = cue::create_windows_thread_services();
        if (!createdServices.has_value())
            return fail(a_server, *createdServices.try_error());
        auto services = createdServices.take_value();
        cue::FrameControllerDesc frame{1, false, a_options.isEmbedded ? 60u : 0u};
        cue::FrameController controller(frame, *services.clock, *services.waiter, *services.threadFactory);
        auto registered = controller.register_callbacks(
            [&](std::uint64_t, std::stop_token)
            {
                auto updated = session->update(std::chrono::nanoseconds(16'666'667));
                if (updated.has_value())
                    ++ticks;
                return updated;
            },
            [](std::uint64_t, std::stop_token) { return cue::Result<void>::success(); });
        if (!registered.has_value())
            return fail(a_server, *registered.try_error());
        auto started = controller.start();
        if (!started.has_value())
            return fail(a_server, *started.try_error());
        auto ready = a_server.send_ready();
        if (!ready.has_value())
            return fail(a_server, *ready.try_error());
        while (!shouldStop && (!a_options.maxTicks || ticks < a_options.maxTicks))
        {
            auto commands = processCommands();
            if (!commands.has_value())
                return fail(a_server, *commands.try_error());
            if (shouldStop)
                break;
            auto stepped = controller.step();
            if (!stepped.has_value())
                return fail(a_server, *stepped.try_error());
            // 要求一件に対して更新確定後の最新値だけを返し、遅い Editor 向けの蓄積を作らない
            if (*stepped.try_value() && pendingView)
            {
                auto snapshot = capture_view(*session, *scene);
                if (!snapshot.has_value())
                    return fail(a_server, *snapshot.try_error());
                auto queued = a_server.send_view(*pendingView, *snapshot.try_value());
                if (!queued.has_value())
                    return fail(a_server, *queued.try_error());
                pendingView.reset();
            }
            if (!a_options.isEmbedded)
                Sleep(1);
        }
        auto stopped = controller.stop();
        if (!stopped.has_value())
            return fail(a_server, *stopped.try_error());
    }
    else
    {
        // 最初の実装は Main と Update を同じ Owner に固定する。Render は所有 Snapshot だけ読む
        std::unique_ptr<cue::ISceneRenderer> renderer;
        cue::WindowsHostConfig config;
        config.window = {"CueEngine Play", {1280, 720}};
        config.frame = {1, false, 60};
        config.gpu.resourceFrameCount = 1;
        config.gpu.maxFramesInFlight = 1;
        config.simulation.frequencyHz = 60;
        config.storage.mode =
            installation.try_value()->isPackaged ? cue::StorageMode::Product : cue::StorageMode::Development;
        config.storage.repositoryRoot = installation.try_value()->root;
        config.storage.applicationName = "CueEnginePlay-" + installation.try_value()->version;
        config.logging.isFileEnabled = false;
        config.callbacks.initializeRenderer = [&](cue::IBackend &a_backend, std::uint32_t, std::uint32_t)
        {
            auto created = cue::create_scene_renderer(a_backend);
            if (!created.has_value())
                return cue::Result<void>::failure(*created.try_error());
            renderer = created.take_value();
            return cue::Result<void>::success();
        };
        config.graph.configure = [&](cue::FrameGraph &a_graph, cue::FrameGraphResourceHandle)
        {
            auto pass = renderer->create_pass();
            if (!pass.has_value())
                return cue::Result<void>::failure(*pass.try_error());
            auto registered = a_graph.add_pass(pass.take_value());
            return registered.has_value() ? cue::Result<void>::success()
                                          : cue::Result<void>::failure(*registered.try_error());
        };
        config.callbacks.simulationUpdate = [&](const cue::SimulationStep &a_step, std::stop_token)
        {
            if (shouldStop)
                return cue::Result<void>::success();
            ++ticks;
            return session->update(std::chrono::duration_cast<std::chrono::nanoseconds>(a_step.deltaTime));
        };
        config.callbacks.prepareFrame = [&](const cue::FrameSimulationInfo &a_frame, std::stop_token)
        {
            auto snapshot = session->snapshot();
            if (!snapshot.has_value())
                return cue::Result<void>::failure(*snapshot.try_error());
            return renderer->publish_frame(a_frame.frameIndex, snapshot.take_value());
        };
        config.callbacks.recordFrame =
            [&](std::uint64_t a_frame, std::stop_token a_stop, const cue::FrameCallback &a_record)
        { return renderer->render_frame(a_frame, [&]() { return a_record(a_frame, a_stop); }); };
        config.callbacks.reconfigureRenderer = [&](cue::IBackend &, std::uint32_t, std::uint32_t)
        { return renderer->discard_pending(); };
        config.callbacks.shutdownWindow = [&]()
        { return renderer ? renderer->shutdown() : cue::Result<void>::success(); };
        cue::WindowsHost host(std::move(config));
        auto started = host.initialize();
        if (!started.has_value())
            return fail(a_server, *started.try_error());
        auto ready = a_server.send_ready();
        if (!ready.has_value())
            return fail(a_server, *ready.try_error());
        while (!shouldStop && (!a_options.maxTicks || ticks < a_options.maxTicks))
        {
            auto commands = processCommands();
            if (!commands.has_value())
                return fail(a_server, *commands.try_error());
            if (shouldStop)
                break;
            auto stepped = host.step();
            if (!stepped.has_value())
                return fail(a_server, *stepped.try_error());
            if (!*stepped.try_value())
                break;
        }
        auto stopped = host.shutdown();
        if (!stopped.has_value())
            return fail(a_server, *stopped.try_error());
    }
    auto stoppedWorld = session->shutdown();
    if (!stoppedWorld.has_value())
        return fail(a_server, *stoppedWorld.try_error());
    drain(a_server);
    return 0;
}
} // namespace

/// @brief 専用子 Process のみ Game Code を実行し、通常失敗を親へ返す
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    // Game Code の Native Crash で Modal を表示せず、親が終了 Code を監視する
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    std::unique_ptr<cue::PlayServer> server;
    try
    {
        auto options = read_options();
        if (!options.has_value())
            return 2;
        auto connected = cue::PlayServer::connect(options.try_value()->pipeName, options.try_value()->session,
                                                  options.try_value()->isDebug);
        if (!connected.has_value())
            return 2;
        server = connected.take_value();
        return run(*options.try_value(), *server);
    }
    catch (...)
    {
        return server ? fail(*server, {cue::ErrorCategory::Fatal, "Play.unexpected_exception"}) : 2;
    }
}
