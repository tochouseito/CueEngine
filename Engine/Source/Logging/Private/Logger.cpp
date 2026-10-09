#include <Logging/Logger.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <functional>
#include <optional>
#include <string>
#include <utility>

namespace cue
{
namespace
{
thread_local const Logger *g_activeLogger = nullptr;

/// @brief Sink の再帰呼出しで Mutex を待たないよう、呼出 Thread の処理中 Logger を記録する
class ActiveLogger final
{
  public:
    /// @brief 直前の Logger を借用し、現在の配送先を設定する
    explicit ActiveLogger(const Logger *a_logger) : m_previous(g_activeLogger)
    {
        g_activeLogger = a_logger;
    }
    /// @brief 外側の配送先を復元する
    ~ActiveLogger()
    {
        g_activeLogger = m_previous;
    }

  private:
    const Logger *m_previous;
};

/// @brief 列挙範囲外の Level を拒否する
bool is_valid_level(LogLevel a_level) noexcept
{
    return a_level <= LogLevel::Fatal;
}

/// @brief 改行や NUL が一行の構造を壊さないよう、文字列を可逆な表記へ変換する
void append_text(std::string &a_output, std::string_view a_text)
{
    for (const char value : a_text)
    {
        switch (value)
        {
        case '\n':
            a_output += "\\n";
            break;
        case '\r':
            a_output += "\\r";
            break;
        case '\0':
            a_output += "\\0";
            break;
        case '\\':
            a_output += "\\\\";
            break;
        default:
            a_output += value;
            break;
        }
    }
}

/// @brief Platform の時刻変換へ依存せず UTC と診断 Metadata を一行に整形する
std::string format_record(const LogRecord &a_record)
{
    constexpr std::array names{"Trace", "Debug", "Info", "Warning", "Error", "Fatal"};
    const auto milliseconds = std::chrono::floor<std::chrono::milliseconds>(a_record.time);
    const auto day = std::chrono::floor<std::chrono::days>(milliseconds);
    const std::chrono::year_month_day date(day);
    const std::chrono::hh_mm_ss clock(milliseconds - day);
    char prefix[128]{};
    std::snprintf(prefix, sizeof(prefix), "%04d-%02u-%02uT%02lld:%02lld:%02lld.%03lldZ [%s] [thread=%zu] ",
                  static_cast<int>(date.year()), static_cast<unsigned>(date.month()), static_cast<unsigned>(date.day()),
                  static_cast<long long>(clock.hours().count()), static_cast<long long>(clock.minutes().count()),
                  static_cast<long long>(clock.seconds().count()), static_cast<long long>(clock.subseconds().count()),
                  names[static_cast<std::size_t>(a_record.level)], std::hash<std::thread::id>{}(a_record.threadId));
    std::string line(prefix);
    append_text(line, a_record.source);
    line += ": ";
    append_text(line, a_record.message);
    if (a_record.errorCategory)
    {
        line += " [errorCategory=" + std::to_string(static_cast<int>(*a_record.errorCategory)) + "]";
    }
    if (a_record.nativeCode)
    {
        line += " [nativeCode=" + std::to_string(*a_record.nativeCode) + "]";
    }
    if (a_record.location.line() != 0)
    {
        line += " [";
        append_text(line, a_record.location.file_name());
        line += ":" + std::to_string(a_record.location.line()) + " ";
        append_text(line, a_record.location.function_name());
        line += "]";
    }
    line += '\n';
    return line;
}
} // namespace

Logger::Logger(CreateToken, std::vector<std::unique_ptr<ILogSink>> a_sinks, LogLevel a_minimum)
    : m_sinks(std::move(a_sinks)), m_minimum(a_minimum)
{
}

Logger::~Logger()
{
    try
    {
        static_cast<void>(shutdown());
    }
    catch (...)
    { /* 明示 shutdown の結果確認を公開契約とする */
    }
}

Result<std::unique_ptr<Logger>> Logger::create(std::vector<std::unique_ptr<ILogSink>> a_sinks, LogLevel a_minimum)
{
    if (a_sinks.empty() || !is_valid_level(a_minimum))
    {
        return Result<std::unique_ptr<Logger>>::failure({ErrorCategory::InvalidArgument, "Logger.create"});
    }
    for (const auto &sink : a_sinks)
    {
        if (!sink)
        {
            return Result<std::unique_ptr<Logger>>::failure({ErrorCategory::InvalidArgument, "Logger.create.sink"});
        }
    }
    return Result<std::unique_ptr<Logger>>::success(
        std::make_unique<Logger>(CreateToken{}, std::move(a_sinks), a_minimum));
}

Result<void> Logger::write(const LogRecord &a_record)
{
    if (g_activeLogger != nullptr)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "Logger.write.reentry"});
    }
    ActiveLogger active(this);
    std::lock_guard lock(m_mutex);
    if (m_isStopped || !is_valid_level(a_record.level))
    {
        return Result<void>::failure(
            {m_isStopped ? ErrorCategory::InvalidState : ErrorCategory::InvalidArgument, "Logger.write"});
    }
    if (a_record.level < m_minimum)
    {
        return Result<void>::success();
    }
    const auto line = format_record(a_record);
    std::optional<Error> failure;
    for (const auto &sink : m_sinks)
    {
        try
        {
            auto result = sink->write(line);
            if (!result.has_value() && !failure)
            {
                failure = *result.try_error();
            }
        }
        catch (...)
        {
            if (!failure)
            {
                failure = Error{ErrorCategory::PlatformFailure, "Logger.sink.exception"};
            }
        }
    }
    return failure ? Result<void>::failure(std::move(*failure)) : Result<void>::success();
}

Result<void> Logger::flush()
{
    if (g_activeLogger != nullptr)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "Logger.flush.reentry"});
    }
    ActiveLogger active(this);
    std::lock_guard lock(m_mutex);
    if (m_isStopped)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "Logger.flush.stopped"});
    }
    std::optional<Error> failure;
    for (const auto &sink : m_sinks)
    {
        try
        {
            auto result = sink->flush();
            if (!result.has_value() && !failure)
            {
                failure = *result.try_error();
            }
        }
        catch (...)
        {
            if (!failure)
            {
                failure = Error{ErrorCategory::PlatformFailure, "Logger.sink.exception"};
            }
        }
    }
    return failure ? Result<void>::failure(std::move(*failure)) : Result<void>::success();
}

Result<void> Logger::shutdown()
{
    if (g_activeLogger != nullptr)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "Logger.shutdown.reentry"});
    }
    ActiveLogger active(this);
    std::lock_guard lock(m_mutex);
    std::optional<Error> failure;
    if (!m_isStopped)
    {
        for (const auto &sink : m_sinks)
        {
            try
            {
                auto result = sink->flush();
                if (!result.has_value() && !failure)
                {
                    failure = *result.try_error();
                }
            }
            catch (...)
            {
                if (!failure)
                {
                    failure = Error{ErrorCategory::PlatformFailure, "Logger.sink.exception"};
                }
            }
        }
    }
    m_isStopped = true;
    for (const auto &sink : m_sinks)
    {
        try
        {
            auto result = sink->close();
            if (!result.has_value() && !failure)
            {
                failure = *result.try_error();
            }
        }
        catch (...)
        {
            if (!failure)
            {
                failure = Error{ErrorCategory::PlatformFailure, "Logger.sink.exception"};
            }
        }
    }
    return failure ? Result<void>::failure(std::move(*failure)) : Result<void>::success();
}
} // namespace cue
