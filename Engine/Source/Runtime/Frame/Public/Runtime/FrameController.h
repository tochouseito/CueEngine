#pragma once

#include <Foundation/Result.h>
#include <Platform/Clock.h>
#include <Platform/Thread.h>
#include <Platform/Waiter.h>
#include <Profiler/Profiler.h>
#include <Runtime/FramePacing.h>
#include <Runtime/Simulation.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>

namespace cue
{
/// @brief FPS 期限の追い込み方法を指定する
enum class FrameWaitMode
{
    Sleep,
    Hybrid,
};

/// @brief Frame の先行数、Worker 利用、開始頻度の上限を指定する
struct FrameControllerDesc final
{
    std::uint32_t maxFramesInFlight = 2;
    bool useWorkerThreads = true;
    /// Render / Present 後の周期待機に使う FPS 上限。0 は上限なし
    std::uint32_t maxFps = 60;
    // 未指定は既存の容量から導出する。0 は同じ Frame の Update / Render を直列化する
    std::optional<std::uint32_t> maxUpdateLeadFrames;
    FrameWaitMode waitMode = FrameWaitMode::Hybrid;
    std::chrono::nanoseconds spinDuration = std::chrono::microseconds(500);
    FrameSyncMode syncMode = FrameSyncMode::FixedPeriod;
    std::chrono::nanoseconds predictionSlack = std::chrono::milliseconds(1);

    /// @brief 設定値の変更有無を所有値だけで比較する
    bool operator==(const FrameControllerDesc &) const = default;
};

/// @brief 実行 Thread に依存せず、期限の追い込みに使用する実効 Spin 時間を返す
[[nodiscard]] inline std::chrono::nanoseconds effective_spin_duration(const FrameControllerDesc &a_desc) noexcept
{
    if (a_desc.maxFps == 0 || a_desc.waitMode != FrameWaitMode::Hybrid)
    {
        return std::chrono::nanoseconds::zero();
    }
    const auto period = std::chrono::nanoseconds((1'000'000'000ULL + a_desc.maxFps - 1) / a_desc.maxFps);
    return (std::min)(a_desc.spinDuration, period / 8);
}

/// @brief Main Threadが進行状況を参照するためのSnapshot
struct FrameProgress final
{
    std::uint64_t submittedFrames = 0;
    std::uint64_t updatedFrames = 0;
    std::uint64_t renderedFrames = 0;
    // CPU 完了を先に公開し、周期待機が終わった件数を独立して保持する
    std::uint64_t completedCycles = 0;
    std::uint64_t lastUpdateFrame = 0;
    std::uint64_t lastRenderFrame = 0;
    std::thread::id updateThreadId{};
    std::thread::id renderThreadId{};
    std::thread::id mainThreadId{};
    std::chrono::nanoseconds lastMainDuration{};
    std::chrono::nanoseconds lastUpdateDuration{};
    std::chrono::nanoseconds lastRenderDuration{};
    std::chrono::nanoseconds lastFrameInterval{};
    std::chrono::nanoseconds lastLimitWaitDuration{};
    std::chrono::nanoseconds lastCycleInterval{};
    FrameSyncMode effectiveSyncMode = FrameSyncMode::FixedPeriod;
    FrameSyncFallback syncFallback = FrameSyncFallback::None;
    std::uint64_t simulationTicks = 0;
    std::chrono::nanoseconds droppedSimulationTime{};
};

/// @brief 経過時間と FPS 待機を分け、直近 120 Frame の分布を公開する
struct FrameTimingInfo final
{
    TimingStatistics main;
    TimingStatistics update;
    TimingStatistics render;
    TimingStatistics limitWait;
    TimingStatistics frameInterval;
    TimingStatistics cycleInterval;
    // Main が入力を採取して Frame を採用した時点から測る。物理入力機器の遅延は含まない
    TimingStatistics inputToRenderStart;
    TimingStatistics inputToRenderComplete;
    TimingStatistics presentationWait;
};

using FrameCallback = std::function<Result<void>(std::uint64_t, std::stop_token)>;

/// @brief Main、Update、RenderのFrame順序とWorker寿命を管理する
///
/// Runtimeが一意所有し、借用するClock、Waiter、ThreadFactoryより先に破棄する
/// start、wait_for_frame、advance、step、stop、破棄は構築 Thread でのみ行い、再入しない
/// progressはThread-safe。WorkerはWindowや可変Worldを直接参照しない
class FrameController final
{
public:
    /// @brief 借用Serviceを受け取り、開始前のControllerを作る
  FrameController(FrameControllerDesc a_desc, Clock &a_clock, Waiter &a_waiter, ThreadFactory &a_threadFactory,
                  SimulationConfig a_simulation = {}, Profiler *a_profiler = nullptr);

  /// @brief Callbackを構築時に渡す既存の入口
  FrameController(FrameControllerDesc a_desc, Clock &a_clock, Waiter &a_waiter, ThreadFactory &a_threadFactory,
                  FrameCallback a_update, FrameCallback a_render, SimulationConfig a_simulation = {},
                  Profiler *a_profiler = nullptr);

  /// @brief Workerを停止・joinしてから資源を解放する
  ~FrameController();

  FrameController(const FrameController &) = delete;
  FrameController &operator=(const FrameController &) = delete;

  /// @brief Runtime から渡された Main、Update と Render の処理を開始前に一度だけ登録する
  ///
  /// Callbackの借用先はstop完了まで有効に保つ。構築Threadから呼び、失敗時は未登録のままにする
  /// 任意の Main は投入枠確保後、Worker へ公開する前に構築 Thread で呼ぶ
  [[nodiscard]] Result<void> register_callbacks(FrameCallback a_update, FrameCallback a_render,
                                                FrameCallback a_main = {});

  /// @brief Host の表示同期機能を開始前に登録する。借用先は停止完了まで有効にする
  [[nodiscard]] Result<void> register_pacing_source(FramePacingSource a_source);

  /// @brief 開始前にゲーム更新と Snapshot 抽出を登録する。借用先は停止完了まで有効にする
  /// 固定更新の後に既存 Update、最後に Snapshot を実行する。Snapshot は補間用の前後状態を複製する
  [[nodiscard]] Result<void> register_simulation_callbacks(simulationCallback a_update,
                                                           frameSnapshotCallback a_snapshot = {});

  /// @brief 設定を検証してWorkerを開始する
  ///
  /// 失敗時に開始済みWorkerを停止・joinし、再試行可能な開始前状態へ戻す
  [[nodiscard]] Result<void> start();

  /// @brief 開始時刻と空き枠を短時間待ち、Frame を開始可能なら true を返す
  ///
  /// 構築 Thread 専用。1 回の待機は最大 1 ms の指定時間と OS の実際の遅延まで
  /// false でも Host は入力処理を続ける。Callback を実行せず、枠や開始時刻を予約しない
  /// true の後に入力を処理して advance を呼ぶ。Worker の失敗は Result に返す
  [[nodiscard]] Result<bool> wait_for_frame();

  /// @brief 開始時刻と空き枠が揃えば Frame を 1 件投入し、投入時は true を返す
  ///
  /// FPS / 容量待機は行わない。単一 Thread 構成では Update と Render を同期実行する
  [[nodiscard]] Result<bool> advance();

  /// @brief 開始時刻と空き枠を短時間待って Frame を進め、未準備なら false を返す
  [[nodiscard]] Result<bool> step();

  /// @brief 停止を要求してWorkerをjoinし、失敗を呼出側へ返す
  ///
  /// 複数回呼出可能。失敗後もWorkerは残さない
  [[nodiscard]] Result<void> stop();

  /// @brief 新規投入停止中の CPU Frame を FPS 待機なしで排出する。Owner Thread 専用
  void request_drain();

  /// @brief CPU 静止状態で Worker を join する。Frame ID と Callback を維持する
  [[nodiscard]] Result<void> pause();

  /// @brief pause 後に設定を適用し、同じ Frame ID から再開する。失敗時は停止状態
  [[nodiscard]] Result<void> resume(FrameControllerDesc a_desc, std::optional<SimulationConfig> a_simulation = {});

  /// @brief 完了数、直近のFrame番号、実行Thread、処理時間を取得する
  [[nodiscard]] FrameProgress progress() const;

  /// @brief 追加時に維持した正確な統計だけを共有 Lock 内で複写する
  [[nodiscard]] FrameTimingInfo timing_info() const;

  /// @brief 成功した Update が確定した Frame の補間情報を返す。Thread-safe、最新 8 Frame のみ保持
  [[nodiscard]] Result<FrameSimulationInfo> simulation_info(std::uint64_t a_frame) const;
  /// @brief 次の更新で採取時刻だけ再設定する。Owner が Frame 投入を停止した境界で要求する
  void reset_simulation_sample_time();

  /// @brief 新規投入せず、投入済み CPU Frame の完了と Worker の失敗を確認する
  ///
  /// GPU 完了は表さない。呼出側が advance / step を止めている間だけ静止判定に使える
  [[nodiscard]] Result<bool> is_idle() const;

private:
  /// @brief Update Workerが投入済みFrameを順番に処理する
  [[nodiscard]] Result<void> update_loop(std::stop_token a_stopToken);

  /// @brief Simulation Tick、既存 Update と不変 Snapshot を順に成功確定する
  [[nodiscard]] Result<void> perform_update(std::uint64_t a_frame, std::stop_token a_token);

  /// @brief Update完了済みFrameをRender Workerが順番に処理する
  [[nodiscard]] Result<void> render_loop(std::stop_token a_stopToken);

  /// @brief 最初のWorker失敗を保存して他の待機Threadを起こす
  void record_failure(Error a_error);

  /// @brief 開始条件と非同期失敗を確認し、FPS による開始待ちの計測点を保持する
  [[nodiscard]] Result<bool> frame_ready();

  /// @brief Render 後に周期の残り時間を待ち、停止していなければ次の投入を許可する
  void finish_cycle(std::chrono::steady_clock::time_point a_renderCompletion, std::stop_token a_stopToken);

  /// @brief 共通の待機方針で期限へ近づけ、Main は最大 1 ms の区間で Pump へ戻す
  void wait_for_deadline(std::chrono::steady_clock::time_point a_deadline, std::stop_token a_token, bool a_isMain);

  /// @brief 要求された同期方針を表示能力と統計に照らして解決する
  [[nodiscard]] Result<FramePacingPlan> cycle_plan(std::chrono::steady_clock::time_point a_completion);

  FrameControllerDesc m_desc;
  Clock &m_clock;
  Waiter &m_waiter;
  ThreadFactory &m_threadFactory;
  FrameCallback m_main;
  FrameCallback m_update;
  FrameCallback m_render;
  std::thread::id m_ownerId;
  mutable std::mutex m_mutex;
  FrameProgress m_progress;
  // 単独利用では自身が所有し、Host 指定時は同じ Registry に計測を集約する
  Profiler m_ownedProfiler;
  Profiler &m_profiler;
  ProfileMetric m_mainTimings;
  ProfileMetric m_updateTimings;
  ProfileMetric m_renderTimings;
  ProfileMetric m_limitTimings;
  ProfileMetric m_intervalTimings;
  ProfileMetric m_cycleTimings;
  ProfileMetric m_inputToRenderStartTimings;
  ProfileMetric m_inputToRenderCompleteTimings;
  ProfileMetric m_presentationWaitTimings;
  std::array<std::chrono::steady_clock::time_point, 8> m_frameInputTimes{};
  FramePacingSource m_pacingSource;
  SimulationConfig m_simulationConfig;
  SimulationStepper m_simulation;
  simulationCallback m_simulationUpdate;
  frameSnapshotCallback m_snapshotUpdate;
  std::array<std::optional<FrameSimulationInfo>, 8> m_simulationFrames;
  std::atomic<bool> m_resetsSimulationSample = false;
  FramePacingPlan m_singlePlan;
  std::optional<std::chrono::steady_clock::time_point> m_singleFpsCompletion;
  std::uint64_t m_nextUpdateFrame = 0;
  std::uint64_t m_nextRenderFrame = 0;
  // Worker の周期開始は Render だけが更新する。投入許可は共有 Lock で公開する
  std::uint64_t m_startPermits = 0;
  std::chrono::steady_clock::time_point m_cycleStarted{};
  // 単一 Thread は期限だけ保持し、長い Sleep で Message Pump を止めない
  std::chrono::steady_clock::time_point m_nextFrameTime{};
  std::optional<std::chrono::steady_clock::time_point> m_limitWaitStarted;
  std::chrono::steady_clock::time_point m_lastRenderCompletion{};
    bool m_hasRenderCompletionSample = false;
    bool m_isFirstFrameOfRun = true;
  std::optional<Error> m_failure;
  std::unique_ptr<Thread> m_updateThread;
  std::unique_ptr<Thread> m_renderThread;
  bool m_isStarted = false;
  bool m_hasStarted = false;
  bool m_stopRequested = false;
  // Main Callback から同じ Controller を進行・停止し、未公開 Frame を変更する再入を拒否する
  bool m_isExecutingMain = false;
  // Main の Snapshot / Texture 待機も Worker 失敗時に解除する
  std::stop_source m_mainStopSource;
  // 排出だけの要求は Callback の停止 Token を取消さない
  std::stop_source m_cycleStopSource;
};
} // namespace cue
