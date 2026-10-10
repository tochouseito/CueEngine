#include <WindowsHost/WindowsHost.h>

#include <atomic>
#include <chrono>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <DX12/DX12Backend.h>
#include <DX12/DX12CommandPool.h>
#include <DX12/DX12MainFrameGraph.h>
#include <DX12/DX12RenderDevice.h>
#include <DX12/DX12SwapChain.h>
#include <Foundation/ScopedFlag.h>
#include <Logging/FileLogSink.h>
#include <Logging/Logger.h>
#include <Platform/Diagnostics.h>
#include <Platform/WindowSystem.h>
#include <Platform/Windows/WindowsDebugLogSink.h>
#include <Platform/Windows/WindowsFileSystem.h>
#include <Platform/Windows/WindowsPlatform.h>
#include <RHI/BackendFactory.h>
#include <Runtime/Runtime.h>

namespace cue
{
namespace
{
/// @brief 起動ごとの新規 File と構成別の DebugSink を、Host の所有 Logger に組み立てる
Result<std::unique_ptr<Logger>> create_host_logger(IFileSystem &a_files, const StoragePaths &a_paths,
                                                   const HostLoggingConfig &a_config, Path &a_logFile)
{
    using loggerResult = Result<std::unique_ptr<Logger>>;
    if (a_config.minimumLevel > LogLevel::Fatal ||
        (a_config.debugOutput != DebugLogOutput::Automatic && a_config.debugOutput != DebugLogOutput::Enabled &&
         a_config.debugOutput != DebugLogOutput::Disabled))
    {
        return loggerResult::failure({ErrorCategory::InvalidArgument, "WindowsHost.logging.config"});
    }
    bool isDebugEnabled = a_config.debugOutput == DebugLogOutput::Enabled;
#if !defined(CUE_SHIPPING)
    isDebugEnabled = isDebugEnabled || a_config.debugOutput == DebugLogOutput::Automatic;
#endif
    std::vector<std::unique_ptr<ILogSink>> sinks;
    sinks.push_back(create_windows_debug_log_sink(isDebugEnabled));
    if (a_config.isFileEnabled)
    {
        // CreateNew と PID / 時刻 / 連番を合わせ、同時 Process と同じ ms の再起動でも旧ログを保全する
        static std::atomic<std::uint64_t> sequence = 0;
        bool wasCreated = false;
        for (int attempt = 0; attempt < 16; ++attempt)
        {
            const auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::system_clock::now().time_since_epoch())
                                       .count();
            auto path = a_paths.logs.join("CueEngine-" + std::to_string(timestamp) + "-" +
                                          std::to_string(GetCurrentProcessId()) + "-" +
                                          std::to_string(sequence.fetch_add(1)) + ".log");
            if (!path.has_value())
            {
                return loggerResult::failure(*path.try_error());
            }
            auto file = FileLogSink::create(a_files, *path.try_value());
            if (!file.has_value())
            {
                const auto &error = *file.try_error();
                if (error.nativeCode == ERROR_FILE_EXISTS || error.nativeCode == ERROR_ALREADY_EXISTS)
                {
                    continue;
                }
                return loggerResult::failure(error);
            }
            a_logFile = path.take_value();
            sinks.push_back(file.take_value());
            wasCreated = true;
            break;
        }
        if (!wasCreated)
        {
            return loggerResult::failure({ErrorCategory::PlatformFailure, "WindowsHost.logging.filename"});
        }
    }
    return Logger::create(std::move(sinks), a_config.minimumLevel);
}
} // namespace

class WindowsHost::State final
{
  public:
    // Backend、UI と Worker より長く生存させる
    std::unique_ptr<IFileSystem> files;
    StoragePaths paths;
    // 逆順破棄でも登録解除が Logger より先、Logger の File 回収が FileSystem より先になる
    std::unique_ptr<Logger> logger;
    std::unique_ptr<DiagnosticRegistration> diagnosticRegistration;
    Path logFile;
    std::unique_ptr<WindowSystem> system;
    std::unique_ptr<Window> window;
    std::unique_ptr<IBackend> backend;
    dx12::DX12Backend *dx12Backend = nullptr;
    std::unique_ptr<dx12::DX12MainFrameGraph> graph;
    WindowsThreadServices services;
    std::unique_ptr<Runtime> runtime;
    std::atomic<bool> isRenderStopped = false;
    std::atomic<bool> isPresentationSuspended = false;
    // 要求寸法と実際の表示寸法は Window を所有する Main Thread だけが操作する
    WindowSize requestedSize{};
    WindowSize presentationSize{};
    bool isCloseRequested = false;
    bool isDestroyed = false;
    bool hasWindowInitializationStarted = false;
    mutable std::mutex timingMutex;
    TimingSamples presentTimings;

    /// @brief 最新の ClientSize を保持し、旧寸法への追加提出を停止する
    void request_resize(WindowSize a_size)
    {
        requestedSize = a_size;
        isPresentationSuspended.store(a_size.width == 0 || a_size.height == 0 ||
                                      a_size.width != presentationSize.width ||
                                      a_size.height != presentationSize.height);
    }

    /// @brief 全 CPU Frame の完了後、Owner Thread でサイズ依存資源を再生成する
    [[nodiscard]] Result<void> prepare_presentation()
    {
        const auto size = requestedSize;
        if (size.width == 0 || size.height == 0 || isRenderStopped.load())
        {
            return Result<void>::success();
        }
        if (size.width == presentationSize.width && size.height == presentationSize.height)
        {
            return Result<void>::success();
        }
        const auto *resources = dx12Backend->get_resource_context();
        if (!resources || !graph)
        {
            return Result<void>::failure({ErrorCategory::InvalidState, "WindowsHost.resize.resources"});
        }
        // Graph は全枠の完了を待ち、旧 External Binding と View を外してから SwapChain を更新する
        auto result = graph->resize(*resources, size.width, size.height);
        if (!result.has_value())
        {
            return result;
        }
        presentationSize = size;
        isPresentationSuspended.store(false);
        return Result<void>::success();
    }

    /// @brief 固定 Graph の Command を Graphics Queue に提出して表示する
    [[nodiscard]] Result<void> render(
        std::uint64_t a_frameIndex, std::stop_token a_stopToken, std::uint32_t a_frameCount,
        const std::function<Result<void>(std::uint64_t, std::stop_token, const FrameCallback &)> &a_recordFrame)
    {
        bool wasSubmitted = false;
        FrameCallback record = [&](std::uint64_t, std::stop_token) -> Result<void>
        {
            // 表示を止める Frame も上位の Scope に通し、未記録 Snapshot を回収する
            if (a_stopToken.stop_requested() || isRenderStopped.load() || isPresentationSuspended.load())
            {
                return Result<void>::success();
            }
            const auto *execution = dx12Backend->get_execution_context();
            auto *swapChain = dx12Backend->get_swap_chain();
            if (!execution || !swapChain || !swapChain->graphics_queue() || !graph)
            {
                return Result<void>::failure({ErrorCategory::InvalidState, "WindowsHost.render.resources"});
            }
            const auto frameSlot = static_cast<std::uint32_t>(a_frameIndex % a_frameCount);
            auto executed = graph->execute(
                frameSlot, *execution, [&]()
                { return a_stopToken.stop_requested() || isRenderStopped.load() || isPresentationSuspended.load(); });
            if (!executed.has_value())
            {
                return Result<void>::failure(*executed.try_error());
            }
            wasSubmitted = *executed.try_value();
            return Result<void>::success();
        };
        auto recorded =
            a_recordFrame ? a_recordFrame(a_frameIndex, a_stopToken, record) : record(a_frameIndex, a_stopToken);
        if (!recorded.has_value())
        {
            return recorded;
        }
        // DXGI が Message Thread を待つ場合に備え、上位の Context 排他と Pin を残さない
        // 記録中に最小化や Close が届いた場合も、提出済み GPU 作業の寿命を残して Present だけ止める
        if (!wasSubmitted || a_stopToken.stop_requested() || isRenderStopped.load() || isPresentationSuspended.load())
        {
            return Result<void>::success();
        }
        const auto presentStart = std::chrono::steady_clock::now();
        auto presented = dx12Backend->get_swap_chain()->present();
        const auto duration =
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - presentStart);
        {
            std::lock_guard lock(timingMutex);
            presentTimings.add(duration);
        }
        return presented;
    }
};

/// @brief 起動設定と構築 Thread を固定する
WindowsHost::WindowsHost(WindowsHostConfig a_config)
    : m_config(std::move(a_config)), m_ownerId(std::this_thread::get_id())
{
}

IFileSystem *WindowsHost::file_system() const noexcept
{
    return std::this_thread::get_id() == m_ownerId && m_state ? m_state->files.get() : nullptr;
}

const StoragePaths *WindowsHost::storage_paths() const noexcept
{
    return std::this_thread::get_id() == m_ownerId && m_state && m_state->files ? &m_state->paths : nullptr;
}

ILogger *WindowsHost::logger() const noexcept
{
    return std::this_thread::get_id() == m_ownerId && m_state ? m_state->logger.get() : nullptr;
}

const Path *WindowsHost::log_file_path() const noexcept
{
    return std::this_thread::get_id() == m_ownerId && m_state && !m_state->logFile.is_empty() ? &m_state->logFile : nullptr;
}

/// @brief 明示停止がない場合も Window を回収する
WindowsHost::~WindowsHost()
{
    auto result = shutdown();
    if (!result.has_value())
    {
        report_log_error("CueWindowsHost cleanup", *result.try_error(), LogLevel::Error);
        // 借用解除が完了していない Window を暗黙破棄すると、上位 Owner の参照が失効する
        // 明示 shutdown は再試行できるが、Destructor では安全な回収を継続できない
        if (m_state && m_state->hasWindowInitializationStarted)
        {
            std::terminate();
        }
    }
}

/// @brief Windows の Window、Renderer Backend、Runtime を順に構築する
Result<void> WindowsHost::initialize()
{
    // Win32 の Window は構築 Thread でしか操作できない
    if (std::this_thread::get_id() != m_ownerId)
    {
        return Result<void>::failure({ErrorCategory::WrongThread, "WindowsHost.initialize"});
    }
    // WindowsHost は一度だけ初期化する。失敗時も停止済みにする
    if (m_lifecycle != Lifecycle::Uninitialized)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "WindowsHost.initialize"});
    }

    // 初期化の途中で失敗した場合も、部分資源を破棄して停止済みにする
    m_lifecycle = Lifecycle::Stopped;

    // FrameController の許容先行数と Flip Model の最小 BackBuffer 数を表示前に検証する
    if (m_config.frame.maxFramesInFlight == 0 || m_config.frame.maxFramesInFlight > 2 ||
        m_config.presentation.bufferCount < 2)
    {
        // Graph 生成前の入力失敗でも Host が受け取った Pass を残さない
        m_config.graph = {};
        return Result<void>::failure({ErrorCategory::InvalidArgument, "WindowsHost.config"});
    }

    // 部分初期化の失敗時は元の Error を返し、Cleanup の失敗は別に診断する
    auto rollback = [this](Error a_error)
    {
        report_log_error("WindowsHost.initialize", a_error, LogLevel::Error);
        auto cleanupResult = shutdown();
        if (!cleanupResult.has_value())
        {
            report_log_error("CueWindowsHost cleanup", *cleanupResult.try_error(), LogLevel::Error);
        }
        return Result<void>::failure(std::move(a_error));
    };

    // 機能をまとめたStateを構築
    m_state = std::make_unique<State>();

    // FileSystemがなければWindowsFileSystemを作る
    auto files = m_config.fileSystem ? Result<std::unique_ptr<IFileSystem>>::success(std::move(m_config.fileSystem))
                                     : create_windows_file_system();
    if (!files.has_value())
    {
        return rollback(*files.try_error());
    }
    m_state->files = files.take_value();
    auto paths = resolve_storage_paths(*m_state->files, m_config.storage);
    if (!paths.has_value())
    {
        return rollback(*paths.try_error());
    }
    m_state->paths = paths.take_value();
    auto directories = create_storage_directories(*m_state->files, m_state->paths);
    if (!directories.has_value())
    {
        return rollback(*directories.try_error());
    }
    auto logger = create_host_logger(*m_state->files, m_state->paths, m_config.logging, m_state->logFile);
    if (!logger.has_value())
    {
        return rollback(*logger.try_error());
    }
    m_state->logger = logger.take_value();
    auto registration = register_diagnostic_logger(*m_state->logger);
    if (!registration.has_value())
    {
        return rollback(*registration.try_error());
    }
    m_state->diagnosticRegistration = registration.take_value();
    report_log("WindowsHost", "initializing");
    auto systemResult = create_windows_window_system();
    if (!systemResult.has_value())
    {
        return rollback(*systemResult.try_error());
    }
    m_state->system = systemResult.take_value();

    // Window を作成し、表示に失敗した場合も所有先から回収する
    auto windowResult = m_state->system->create_window(m_config.window);
    if (!windowResult.has_value())
    {
        return rollback(*windowResult.try_error());
    }
    m_state->window = windowResult.take_value();

    // Window に依存する上位 Host の機能を、表示と Message Pump より前に接続する
    if (m_config.callbacks.initializeWindow)
    {
        m_state->hasWindowInitializationStarted = true;
        auto hostResult = m_config.callbacks.initializeWindow(*m_state->window);
        if (!hostResult.has_value())
        {
            return rollback(*hostResult.try_error());
        }
    }

    // Backend が Device を所有し、Window より先に停止できる順序で保持する
    auto backendResult = create_backend(m_state->files.get());
    if (!backendResult.has_value())
    {
        return rollback(*backendResult.try_error());
    }
    m_state->backend = backendResult.take_value();
    m_state->dx12Backend = dynamic_cast<dx12::DX12Backend *>(m_state->backend.get());
    if (!m_state->dx12Backend)
    {
        return rollback({ErrorCategory::InvalidState, "WindowsHost.backend"});
    }

    // Window の Client Area と同じ大きさで Back Buffer と固定 Graph を用意する
    auto handleResult = borrow_windows_window_handle(*m_state->window);
    if (!handleResult.has_value())
    {
        return rollback(*handleResult.try_error());
    }
    m_state->presentationSize = m_state->window->client_size();
    m_state->request_resize(m_state->presentationSize);

    // Swap Chain の設定を構築する
    dx12::DX12SwapChainConfig swapConfig{};
    swapConfig.width = m_state->presentationSize.width;
    swapConfig.height = m_state->presentationSize.height;
    swapConfig.bufferCount = m_config.presentation.bufferCount;
    swapConfig.isVSyncEnabled = m_config.presentation.isVSyncEnabled;
    swapConfig.isTearingAllowed = m_config.presentation.isTearingAllowed;
    auto swapResult = m_state->dx12Backend->create_swap_chain(*handleResult.try_value(), swapConfig);
    if (!swapResult.has_value())
    {
        return rollback(*swapResult.try_error());
    }
    // Editor 等の GPU 接続を先に生成し、Graph の Pass が借用できる状態にする
    if (m_config.callbacks.initializeRenderer)
    {
        m_state->hasWindowInitializationStarted = true;
        auto hostResult = m_config.callbacks.initializeRenderer(*m_state->backend, m_config.frame.maxFramesInFlight);
        if (!hostResult.has_value())
        {
            return rollback(*hostResult.try_error());
        }
    }
    const auto *resources = m_state->dx12Backend->get_resource_context();
    if (!resources)
    {
        return rollback({ErrorCategory::InvalidState, "WindowsHost.graph.resources"});
    }

    // FrameGraph の構築は Backend と SwapChain が生存する間だけ有効で、Runtime より長く保持する
    dx12::DX12MainFrameGraphConfig graphConfig;
    graphConfig.frameCount = m_config.frame.maxFramesInFlight;
    graphConfig.clearColor = m_config.presentation.clearColor;
    // Host が選択した抽象 Pass を Graph に移し、DX12 層には Editor の具体型を伝えない
    graphConfig.configure = std::move(m_config.graph.configure);
    graphConfig.displayPass = std::move(m_config.graph.displayPass);
    graphConfig.displayPassFactory = std::move(m_config.graph.displayPassFactory);
    auto graphResult =
        dx12::DX12MainFrameGraph::create(*resources, *m_state->dx12Backend->get_swap_chain(), std::move(graphConfig));
    if (!graphResult.has_value())
    {
        return rollback(*graphResult.try_error());
    }
    m_state->graph = graphResult.take_value();

    // Runtime が借りる時間と Worker Service を、Runtime より長く生存させる
    auto servicesResult = create_windows_thread_services();
    if (!servicesResult.has_value())
    {
        return rollback(*servicesResult.try_error());
    }
    m_state->services = servicesResult.take_value();

    m_state->runtime = std::make_unique<Runtime>(m_config.frame, *m_state->services.clock, *m_state->services.waiter,
                                                 *m_state->services.threadFactory);
    State *state = m_state.get();
    const auto frameCount = m_config.frame.maxFramesInFlight;
    // Update は採用された Frame だけで実行し、UI と Worker の Thread 契約は上位 Host が検証する
    auto update = m_config.callbacks.update;
    if (!update)
    {
        update = [](std::uint64_t, std::stop_token) { return Result<void>::success(); };
    }
    auto runtimeResult = m_state->runtime->initialize(
        std::move(update),
        [state, frameCount, recordFrame = m_config.callbacks.recordFrame](std::uint64_t a_frameIndex,
                                                                          std::stop_token a_stopToken)
        { return state->render(a_frameIndex, a_stopToken, frameCount, recordFrame); },
        m_config.callbacks.main);
    if (!runtimeResult.has_value())
    {
        return rollback(*runtimeResult.try_error());
    }

    // GPU と Frame の初期化に成功した後で Window を表示する
    auto showResult = m_state->window->show();
    if (!showResult.has_value())
    {
        return rollback(*showResult.try_error());
    }

    m_lifecycle = Lifecycle::Running;
    report_log("WindowsHost", "initialized");
    return Result<void>::success();
}

/// @brief Windows Event を消費して Window の終了要求を返す
Result<bool> WindowsHost::step()
{
    if (std::this_thread::get_id() != m_ownerId)
    {
        return Result<bool>::failure({ErrorCategory::WrongThread, "WindowsHost.step"});
    }
    if (m_lifecycle != Lifecycle::Running || m_isStepping)
    {
        return Result<bool>::failure({ErrorCategory::InvalidState, "WindowsHost.step"});
    }

    ScopedFlag stepping(m_isStepping);
    // 開始期限 / 空き枠の待機後に入力を取り直し、採取済みの UI を FPS 待機で古くしない
    bool isFrameReady = false;
    if (!m_state->isPresentationSuspended.load())
    {
        auto ready = m_state->runtime->wait_for_frame();
        if (!ready.has_value())
        {
            return Result<bool>::failure(*ready.try_error());
        }
        isFrameReady = *ready.try_value();
    }
    // Win32 Message を処理し、Queue 上の終了通知も同じ周回で反映する
    auto pumpResult = m_state->system->pump_events();
    if (!pumpResult.has_value())
    {
        return Result<bool>::failure(*pumpResult.try_error());
    }
    WindowEvent event{};
    while (m_state->window->try_pop_event(event))
    {
        if (event.type == WindowEventType::CloseRequested)
        {
            m_state->isCloseRequested = true;
            m_state->isRenderStopped.store(true);
        }
        else if (event.type == WindowEventType::Destroyed)
        {
            m_state->isDestroyed = true;
            m_state->isRenderStopped.store(true);
        }
        else if (event.type == WindowEventType::Minimized || event.type == WindowEventType::Resized ||
                 event.type == WindowEventType::Restored)
        {
            // 最新寸法にまとめ、既に投入済みの Frame は Snapshot を回収しながら完了させる
            m_state->request_resize(event.clientSize);
        }
    }

    // Window が生存したままの WM_QUIT は所有契約に反する
    if (*pumpResult.try_value() == PumpStatus::QuitRequested && !m_state->isDestroyed)
    {
        return Result<bool>::failure({ErrorCategory::InvalidState, "WindowsHost.quit"});
    }
    if (m_state->isCloseRequested || m_state->isDestroyed || *pumpResult.try_value() == PumpStatus::QuitRequested)
    {
        return Result<bool>::success(false);
    }

    if (m_state->isPresentationSuspended.load())
    {
        // 新規 Frame を投入せず、全 Update / Render / Present と ImGui Snapshot の消費を待つ
        // Worker の失敗もここで返し、未完了 Frame を永久に待たない
        auto idleResult = m_state->runtime->is_idle();
        if (!idleResult.has_value())
        {
            return Result<bool>::failure(*idleResult.try_error());
        }
        if (!*idleResult.try_value() || m_state->requestedSize.width == 0 || m_state->requestedSize.height == 0)
        {
            // Main を join や GPU 待機で塞がず、次の step でも Win32 Message を処理する
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            return Result<bool>::success(true);
        }
        // CPU 側が静止した後にだけ GPU 完了を待つ。ImGui の Context 排他は保持しない
        auto resizeResult = m_state->prepare_presentation();
        if (!resizeResult.has_value())
        {
            return Result<bool>::failure(*resizeResult.try_error());
        }
        // GPU / Resize 待機中の入力も次の Pump で反映してから UI を再開する
        return Result<bool>::success(true);
    }

    if (!isFrameReady)
    {
        return Result<bool>::success(true);
    }
    // 入力処理後は Callback を開始する。開始条件は維持されるが非同期失敗を再確認する
    auto frameResult = m_state->runtime->step();
    if (!frameResult.has_value())
    {
        return Result<bool>::failure(*frameResult.try_error());
    }
    return Result<bool>::success(true);
}

/// @brief 実行中だけ CPU Frame の進行を構築 Thread に公開する
Result<FrameProgress> WindowsHost::frame_progress() const
{
    if (std::this_thread::get_id() != m_ownerId)
    {
        return Result<FrameProgress>::failure({ErrorCategory::WrongThread, "WindowsHost.frame_progress"});
    }
    if (m_lifecycle != Lifecycle::Running || !m_state || !m_state->runtime)
    {
        return Result<FrameProgress>::failure({ErrorCategory::InvalidState, "WindowsHost.frame_progress"});
    }
    return m_state->runtime->progress();
}

/// @brief Owner と Runtime の寿命を検証し、固定容量の CPU 統計を取得する
Result<FrameTimingInfo> WindowsHost::frame_timing_info() const
{
    if (std::this_thread::get_id() != m_ownerId)
    {
        return Result<FrameTimingInfo>::failure({ErrorCategory::WrongThread, "WindowsHost.frame_timing_info"});
    }
    if (m_lifecycle != Lifecycle::Running || !m_state || !m_state->runtime)
    {
        return Result<FrameTimingInfo>::failure({ErrorCategory::InvalidState, "WindowsHost.frame_timing_info"});
    }
    return m_state->runtime->timing_info();
}

/// @brief GPU 完了済み計測値と Present の CPU 経過時間を所有 Snapshot にまとめる
Result<MainFrameGraphPerformance> WindowsHost::graph_performance() const
{
    if (std::this_thread::get_id() != m_ownerId)
    {
        return Result<MainFrameGraphPerformance>::failure(
            {ErrorCategory::WrongThread, "WindowsHost.graph_performance"});
    }
    if (m_lifecycle != Lifecycle::Running || !m_state || !m_state->graph)
    {
        return Result<MainFrameGraphPerformance>::failure(
            {ErrorCategory::InvalidState, "WindowsHost.graph_performance"});
    }
    try
    {
        auto performance = m_state->graph->performance();
        TimingSamples present;
        {
            std::lock_guard lock(m_state->timingMutex);
            present = m_state->presentTimings;
        }
        performance.present = present.statistics();
        return Result<MainFrameGraphPerformance>::success(std::move(performance));
    }
    catch (const std::bad_alloc &)
    {
        return Result<MainFrameGraphPerformance>::failure(
            {ErrorCategory::PlatformFailure, "WindowsHost.graph_performance.allocation"});
    }
}

/// @brief Runtime、Backend、Window を順に停止して Destroyed と Quit を処理する
Result<void> WindowsHost::shutdown()
{
    if (std::this_thread::get_id() != m_ownerId)
    {
        return Result<void>::failure({ErrorCategory::WrongThread, "WindowsHost.shutdown"});
    }
    if (m_isStepping)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "WindowsHost.shutdown"});
    }
    m_lifecycle = Lifecycle::Stopped;
    // Window や Backend の生成前に失敗した場合も未使用の Pass と Callback を回収する
    m_config.graph = {};
    if (!m_state)
    {
        return Result<void>::success();
    }

    // Worker の Callback を止めてから GPU と Window の所有先を解放する
    m_state->isRenderStopped.store(true);
    std::optional<Error> failure;
    if (m_state->runtime)
    {
        auto runtimeResult = m_state->runtime->shutdown();
        if (!runtimeResult.has_value())
        {
            failure = *runtimeResult.try_error();
        }
        m_state->runtime.reset();
    }
    if (m_state->graph)
    {
        auto graphResult = m_state->graph->shutdown();
        if (!graphResult.has_value() && !failure)
        {
            failure = *graphResult.try_error();
        }
        m_state->graph.reset();
    }
    // Pass が借用する UI とその Message Handler を、GPU 完了後かつ Device / Window の破棄前に止める
    if (m_state->hasWindowInitializationStarted && m_config.callbacks.shutdownWindow)
    {
        auto hostResult = m_config.callbacks.shutdownWindow();
        if (!hostResult.has_value())
        {
            // 上位機能が Window / Device の借用をまだ解除できない可能性がある
            // 下位 Owner は保持し、Stopped 状態からの shutdown 再試行を許可する
            report_log_error("WindowsHost.shutdown", failure ? *failure : *hostResult.try_error(), LogLevel::Error);
            return Result<void>::failure(failure ? *failure : *hostResult.try_error());
        }
        m_state->hasWindowInitializationStarted = false;
    }
    if (m_state->backend)
    {
        auto backendResult = m_state->backend->shutdown();
        if (!backendResult.has_value() && !failure)
        {
            failure = *backendResult.try_error();
        }
        m_state->backend.reset();
    }
    if (m_state->window && !m_state->isDestroyed)
    {
        auto destroyResult = m_state->window->destroy();
        if (!destroyResult.has_value())
        {
            if (!failure)
            {
                failure = *destroyResult.try_error();
            }
        }
        if (destroyResult.has_value() && m_state->system)
        {
            auto pumpResult = m_state->system->pump_events();
            if (!pumpResult.has_value() && !failure)
            {
                failure = *pumpResult.try_error();
            }
            WindowEvent event{};
            while (m_state->window->try_pop_event(event))
            {
                if (event.type == WindowEventType::Destroyed)
                {
                    m_state->isDestroyed = true;
                }
            }
            if (pumpResult.has_value() &&
                (*pumpResult.try_value() != PumpStatus::QuitRequested || !m_state->isDestroyed) && !failure)
            {
                failure = Error{ErrorCategory::InvalidState, "WindowsHost.shutdown.quit"};
            }
        }
    }
    // System が借用する Window の参照を先に失効させる
    m_state->window.reset();
    m_state->system.reset();
    m_state->services = {};
    if (failure)
    {
        report_log_error("WindowsHost.shutdown", *failure, LogLevel::Error);
    }
    if (m_state->diagnosticRegistration)
    {
        report_log("WindowsHost", "stopped");
    }
    // 配送中の呼出しを待って借用解除し、Close の緊急診断が自分の File へ再帰しないようにする
    m_state->diagnosticRegistration.reset();
    if (m_state->logger)
    {
        auto stopped = m_state->logger->shutdown();
        if (!stopped.has_value())
        {
            report_log_error("WindowsHost.logging.shutdown", *stopped.try_error(), LogLevel::Error);
            if (!failure)
            {
                failure = *stopped.try_error();
            }
            // File の Close が失敗した場合も所有先を残し、次の shutdown で回収を再試行する
            return Result<void>::failure(*failure);
        }
        m_state->logger.reset();
    }
    m_state.reset();
    return failure ? Result<void>::failure(*failure) : Result<void>::success();
}
} // namespace cue
