#pragma once

#include <Cue/Foundation/Result.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cue
{
/// @brief Child ProcessのCapture元Stream
enum class ChildProcessStream : std::uint8_t
{
    StandardOutput,
    StandardError
};

/// @brief stdout／stderrから観測したByte列とProcess全体の順序情報
struct ChildProcessOutputChunk final
{
    std::uint64_t sequence = 0U;
    ChildProcessStream stream = ChildProcessStream::StandardOutput;
    std::string bytes;
};

/// @brief Native Process終了と利用者Cancel／Timeoutを区別する完了種別
enum class ChildProcessOutcome : std::uint8_t
{
    Exited,
    Cancelled,
    TimedOut
};

/// @brief 一回のChild Process実行が所有する完了状態、Exit Code、Capture Log
class ChildProcessResult final
{
  public:
    /// @brief Native Process終了結果を構築する
    [[nodiscard]] static ChildProcessResult exited(std::uint32_t a_exitCode,
                                                   std::vector<ChildProcessOutputChunk> a_output) noexcept;
    /// @brief 利用者Cancel結果を構築する
    [[nodiscard]] static ChildProcessResult cancelled(std::vector<ChildProcessOutputChunk> a_output) noexcept;
    /// @brief Timeout結果を構築する
    [[nodiscard]] static ChildProcessResult timed_out(std::vector<ChildProcessOutputChunk> a_output) noexcept;

    /// @brief 完了種別を返す
    [[nodiscard]] ChildProcessOutcome outcome() const noexcept;
    /// @brief Exited時だけNative Exit Codeを返す
    [[nodiscard]] std::optional<std::uint32_t> exit_code() const noexcept;
    /// @brief Capture時の全Stream観測順を保持したChunk列を返す
    [[nodiscard]] const std::vector<ChildProcessOutputChunk> &output() const noexcept;

  private:
    /// @brief 整合した完了状態を構築する
    ChildProcessResult(ChildProcessOutcome a_outcome, std::optional<std::uint32_t> a_exitCode,
                       std::vector<ChildProcessOutputChunk> a_output) noexcept;

    ChildProcessOutcome m_outcome;
    std::optional<std::uint32_t> m_exitCode;
    std::vector<ChildProcessOutputChunk> m_output;
};

/// @brief Childへ明示公開する一件のEnvironment値
struct ChildProcessEnvironmentEntry final
{
    std::string name;
    std::string value;
};

/// @brief Shellを介さないExecutable、Argument Vector、実行Context
class ChildProcessRequest final
{
  public:
    /// @brief Childへ必要情報だけを渡すProcess要求を構築する
    ///
    /// EnvironmentはAllowlistそのものであり、Host Environmentを暗黙継承しない。Timeout省略時は時間制限を設けない。
    ChildProcessRequest(std::string a_executable, std::vector<std::string> a_arguments, std::string a_workingDirectory,
                        std::vector<ChildProcessEnvironmentEntry> a_environmentAllowlist,
                        std::optional<std::chrono::milliseconds> a_timeout) noexcept;

    /// @brief 検証前UTF-8 Absolute Executable Pathを返す
    [[nodiscard]] std::string_view executable() const noexcept;
    /// @brief Shell解釈しないArgument Vectorを返す
    [[nodiscard]] const std::vector<std::string> &arguments() const noexcept;
    /// @brief 検証前UTF-8 Absolute Working Directoryを返す
    [[nodiscard]] std::string_view working_directory() const noexcept;
    /// @brief Childへ明示公開するEnvironment Allowlistを返す
    [[nodiscard]] const std::vector<ChildProcessEnvironmentEntry> &environment_allowlist() const noexcept;
    /// @brief 任意の実行Timeoutを返す
    [[nodiscard]] std::optional<std::chrono::milliseconds> timeout() const noexcept;

  private:
    std::string m_executable;
    std::vector<std::string> m_arguments;
    std::string m_workingDirectory;
    std::vector<ChildProcessEnvironmentEntry> m_environmentAllowlist;
    std::optional<std::chrono::milliseconds> m_timeout;
};

/// @brief 別Threadから一方向に通知できる一回のProcess Cancel状態
///
/// `run`呼出しとCancel通知が終了するまで生存させる。Process終了後の通知は次回実行へ持ち越さない。
class ChildProcessCancellation final
{
  public:
    /// @brief 未Cancel状態を構築する
    ChildProcessCancellation() noexcept = default;
    ChildProcessCancellation(const ChildProcessCancellation &) = delete;
    ChildProcessCancellation &operator=(const ChildProcessCancellation &) = delete;
    ChildProcessCancellation(ChildProcessCancellation &&) = delete;
    ChildProcessCancellation &operator=(ChildProcessCancellation &&) = delete;
    /// @brief Cancel状態だけを解放する
    ~ChildProcessCancellation() = default;

    /// @brief 任意ThreadからCancelを通知する
    void request_cancel() noexcept;
    /// @brief Cancelが通知済みか取得する
    [[nodiscard]] bool is_cancel_requested() const noexcept;

  private:
    std::atomic<bool> m_requested = false;
};

/// @brief Platform固有Child ProcessをShell非依存の同期操作として隔離する境界
///
/// 同じInstanceの`run`は直列に呼び、OwnerはShutdown前にCancelを通知して実行ThreadをJoinする。
class ChildProcessRunner
{
  public:
    ChildProcessRunner(const ChildProcessRunner &) = delete;
    ChildProcessRunner &operator=(const ChildProcessRunner &) = delete;
    /// @brief Platform固有Resourceを解放する
    virtual ~ChildProcessRunner() = default;

    /// @brief Child Process Treeを起動し、終了、Cancel、TimeoutまでLogをCaptureする
    [[nodiscard]] virtual Result<ChildProcessResult> run(const ChildProcessRequest &a_request,
                                                         const ChildProcessCancellation &a_cancellation) noexcept = 0;

  protected:
    /// @brief Platform実装だけがRunnerを構築できる状態にする
    ChildProcessRunner() noexcept = default;
};
} // namespace cue
