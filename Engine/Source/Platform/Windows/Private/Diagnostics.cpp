#include <Platform/Diagnostics.h>

#include <algorithm>
#include <cstdio>
#include <exception>
#include <mutex>
#include <shared_mutex>

#if defined(CUE_DEBUG_OUTPUT)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace cue
{
namespace
{
// 所有先は Host の登録 Token と Logger。橋渡しに汎用 Service の検索機能を持たせない
std::shared_mutex g_routeMutex;
ILogger *g_logger = nullptr;
thread_local bool g_isReporting = false;

/// @brief File / Logger の故障中も Allocation と再帰を使わず Debugger へ報告する
void emergency_output(const LogRecord &a_record) noexcept
{
#if defined(CUE_DEBUG_OUTPUT)
    char text[1024]{};
    std::snprintf(text, sizeof(text), "[CueEngine emergency level=%u] %.*s: %.*s (nativeCode=%lld)\n",
                  static_cast<unsigned>(a_record.level),
                  static_cast<int>(std::min(a_record.source.size(), std::size_t(128))),
                  a_record.source.empty() ? "" : a_record.source.data(),
                  static_cast<int>(std::min(a_record.message.size(), std::size_t(700))),
                  a_record.message.empty() ? "" : a_record.message.data(),
                  static_cast<long long>(a_record.nativeCode.value_or(0)));
    wchar_t wide[1024]{};
    if (MultiByteToWideChar(CP_UTF8, 0, text, -1, wide, 1024) != 0)
    {
        OutputDebugStringW(wide);
    }
#else
    static_cast<void>(a_record);
#endif
}

/// @brief File / Sink からの診断が Logger へ再帰しないよう Scope を明示する
class ReportingScope final
{
  public:
    /// @brief 外側の診断だけが登録経路を借用する
    ReportingScope() noexcept
    {
        g_isReporting = true;
    }
    /// @brief 通常の報告を再び許可する
    ~ReportingScope()
    {
        g_isReporting = false;
    }
};

/// @brief 登録解除との競合中も Logger を借用し、失敗後は緊急出力へ戻る
void dispatch(const LogRecord &a_record) noexcept
{
    if (g_isReporting)
    {
        emergency_output(a_record);
        return;
    }
    ReportingScope reporting;
    try
    {
        std::shared_lock lock(g_routeMutex);
        if (g_logger)
        {
            auto result = g_logger->write(a_record);
            if (result.has_value())
            {
                if (a_record.level == LogLevel::Fatal)
                {
                    auto flushed = g_logger->flush();
                    if (!flushed.has_value())
                    {
                        emergency_output(a_record);
                    }
                }
                return;
            }
            emergency_output(a_record);
            LogRecord failure{LogLevel::Error, "Logger", result.try_error()->operation};
            failure.nativeCode = result.try_error()->nativeCode;
            emergency_output(failure);
            return;
        }
    }
    catch (...)
    {
        // Logger の整形・配送例外を業務処理の主原因へ上書きしない
    }
    emergency_output(a_record);
}

/// @brief 移行用の重大度を通常の LogLevel に対応させる
LogLevel convert_level(DiagnosticSeverity a_severity) noexcept
{
    switch (a_severity)
    {
    case DiagnosticSeverity::Warning:
        return LogLevel::Warning;
    case DiagnosticSeverity::Error:
        return LogLevel::Error;
    case DiagnosticSeverity::Fatal:
        return LogLevel::Fatal;
    }
    return LogLevel::Error;
}
} // namespace

DiagnosticRegistration::DiagnosticRegistration(CreateToken, ILogger &a_logger) noexcept : m_logger(&a_logger)
{
}

DiagnosticRegistration::~DiagnosticRegistration()
{
    if (m_isRegistered)
    {
        // Sink からの解除は自分自身の配送完了を待つため、公開契約で禁止する
        if (g_isReporting)
        {
            std::terminate();
        }
        std::unique_lock lock(g_routeMutex);
        if (g_logger == m_logger)
        {
            g_logger = nullptr;
        }
    }
}

Result<std::unique_ptr<DiagnosticRegistration>> register_diagnostic_logger(ILogger &a_logger)
{
    using registrationResult = Result<std::unique_ptr<DiagnosticRegistration>>;
    if (g_isReporting)
    {
        return registrationResult::failure({ErrorCategory::InvalidState, "Diagnostics.register.reentry"});
    }
    auto registration = std::make_unique<DiagnosticRegistration>(DiagnosticRegistration::CreateToken{}, a_logger);
    std::unique_lock lock(g_routeMutex);
    if (g_logger)
    {
        return registrationResult::failure({ErrorCategory::InvalidState, "Diagnostics.register.occupied"});
    }
    g_logger = &a_logger;
    registration->m_isRegistered = true;
    return registrationResult::success(std::move(registration));
}

void report_log(const char *a_source, const char *a_message, LogLevel a_level, std::source_location a_location) noexcept
{
    if (a_source && a_message)
    {
        LogRecord record{a_level, a_source, a_message};
        record.location = a_location;
        dispatch(record);
    }
}

void report_log_error(const char *a_source, const Error &a_error, LogLevel a_level,
                      std::source_location a_location) noexcept
{
    if (a_source)
    {
        LogRecord record{a_level, a_source, a_error.operation, a_error.category, a_error.nativeCode};
        record.location = a_location;
        dispatch(record);
    }
}

void report_error(const char *a_source, const Error &a_error, DiagnosticSeverity a_severity,
                  std::source_location a_location) noexcept
{
    report_log_error(a_source, a_error, convert_level(a_severity), a_location);
}

void report_message(const char *a_source, const char *a_message, DiagnosticSeverity a_severity,
                    std::source_location a_location) noexcept
{
    report_log(a_source, a_message, convert_level(a_severity), a_location);
}
} // namespace cue
