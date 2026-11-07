#pragma once

#include <memory>
#include <source_location>

#include <Foundation/Logging.h>

namespace cue
{
class DiagnosticRegistration;

/// @brief 既存診断 API の配送先を、一つの Host の明示的な所有期間だけ登録する
/// Process 内で同時に一つだけ登録できる。Logger は登録より長く維持する
/// 任意 Thread から登録できるが、Sink / 診断の処理中は登録・解除しない
[[nodiscard]] Result<std::unique_ptr<DiagnosticRegistration>> register_diagnostic_logger(ILogger &a_logger);

/// @brief Logger を所有せず、診断経路の登録解除を所有する
/// 破棄は配送中の呼出し完了を待つ。破棄後の報告は緊急出力へ戻る
class DiagnosticRegistration final
{
    struct CreateToken final
    {
    };
    friend Result<std::unique_ptr<DiagnosticRegistration>> register_diagnostic_logger(ILogger &a_logger);

  public:
    /// @brief Factory が検証した Logger を同期配送の間だけ借用する
    DiagnosticRegistration(CreateToken, ILogger &a_logger) noexcept;
    /// @brief 処理中の配送終了を待ち、Logger の借用を解除する
    ~DiagnosticRegistration();
    DiagnosticRegistration(const DiagnosticRegistration &) = delete;
    DiagnosticRegistration &operator=(const DiagnosticRegistration &) = delete;

  private:
    ILogger *m_logger;
    bool m_isRegistered = false;
};

/// @brief 登録 Logger へ通常ログを配送する。登録前・再入・出力失敗は緊急出力へ退避する
/// 任意 Thread から呼べる。文字列は同期借用。例外は外へ出さず、業務処理の Result は変えない
/// Fatal は配送後に Flush を試みる。緊急出力は Debug / Development のみ
void report_log(const char *a_source, const char *a_message, LogLevel a_level = LogLevel::Info,
                std::source_location a_location = std::source_location::current()) noexcept;
/// @brief Error の分類・Native 値・元の処理名を Logger へ配送する
/// report_log と同じ Thread / 借用 / 例外契約。元の Error の伝播と終了判断は呼出側が行う
void report_log_error(const char *a_source, const Error &a_error, LogLevel a_level,
                      std::source_location a_location = std::source_location::current()) noexcept;

// 外部利用者の移行用。Engine 内の呼出しは上のログ API を使用する
enum class DiagnosticSeverity
{
    Warning,
    Error,
    Fatal
};
/// @brief 旧診断 API を同じ Logger 経路へ接続する
void report_error(const char *a_source, const Error &a_error, DiagnosticSeverity a_severity,
                  std::source_location a_location = std::source_location::current()) noexcept;
/// @brief 旧診断文を同じ Logger 経路へ接続する
void report_message(const char *a_source, const char *a_message, DiagnosticSeverity a_severity,
                    std::source_location a_location = std::source_location::current()) noexcept;
} // namespace cue
