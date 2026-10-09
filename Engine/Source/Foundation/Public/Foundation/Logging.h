#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <source_location>
#include <string_view>
#include <thread>

#include <Foundation/Result.h>

namespace cue
{
enum class LogLevel : std::uint8_t
{
    Trace,
    Debug,
    Info,
    Warning,
    Error,
    Fatal
};

/// @brief 呼出中だけ文字列を借用し、発生時刻と Thread を値で保持する
struct LogRecord final
{
    LogLevel level = LogLevel::Info;
    std::string_view source;
    std::string_view message;
    std::optional<ErrorCategory> errorCategory;
    std::optional<std::int64_t> nativeCode;
    std::chrono::system_clock::time_point time = std::chrono::system_clock::now();
    std::thread::id threadId = std::this_thread::get_id();
    std::source_location location;
};

/// @brief 出力先を隠し、診断と Result の失敗伝播を分離する
/// Owner は利用者より長く生存させる。Thread / 再入契約は具体実装に従う
class ILogger
{
  public:
    /// @brief 出力実装を構築する
    ILogger() = default;
    /// @brief 全利用者の停止後に実装を回収する
    virtual ~ILogger() = default;
    ILogger(const ILogger &) = delete;
    ILogger &operator=(const ILogger &) = delete;
    /// @brief Record を同期で借用し、出力失敗を返す。部分配送後に失敗する場合がある
    [[nodiscard]] virtual Result<void> write(const LogRecord &a_record) = 0;
    /// @brief 全出力先の Buffer を Flush し、失敗を呼出側へ返す
    [[nodiscard]] virtual Result<void> flush() = 0;
    /// @brief 呼出元の位置・時刻・Thread を含む通常ログを作る
    [[nodiscard]] Result<void> log(LogLevel a_level, std::string_view a_source, std::string_view a_message,
                                   std::source_location a_location = std::source_location::current())
    {
        LogRecord record{a_level, a_source, a_message};
        record.location = a_location;
        return write(record);
    }
    /// @brief Error の処理名・分類・Native 値を記録する。元の Error の伝播は呼出側が行う
    [[nodiscard]] Result<void> log_error(LogLevel a_level, std::string_view a_source, const Error &a_error,
                                         std::source_location a_location = std::source_location::current())
    {
        LogRecord record{a_level, a_source, a_error.operation, a_error.category, a_error.nativeCode};
        record.location = a_location;
        return write(record);
    }
};

/// @brief 整形済み UTF-8 の一行を同期で出力する
/// Logger が一意所有し全操作を直列化する。入力は呼出中だけ借用し、Logger へ再入しない
class ILogSink
{
  public:
    /// @brief 出力先を構築する
    ILogSink() = default;
    /// @brief 明示 close 後に出力先を回収する
    virtual ~ILogSink() = default;
    ILogSink(const ILogSink &) = delete;
    ILogSink &operator=(const ILogSink &) = delete;
    /// @brief 終端 LF を含む一行を出力する。失敗時に一部だけ書かれる場合がある
    [[nodiscard]] virtual Result<void> write(std::string_view a_line) = 0;
    /// @brief 出力先の Buffer を Flush する
    [[nodiscard]] virtual Result<void> flush() = 0;
    /// @brief 出力先を閉じる。成功後の再呼出しも成功する
    [[nodiscard]] virtual Result<void> close() = 0;
};
} // namespace cue
