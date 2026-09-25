#include <Cue/Platform/Diagnostics.h>

#if defined(CUE_DEBUG_OUTPUT)
#include <cstdio>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace cue
{
#if defined(CUE_DEBUG_OUTPUT)
namespace
{
/// @brief 診断の重大度をDebugger表示用の文字列へ変換する
const char* severity_name(DiagnosticSeverity a_severity) noexcept
{
    switch (a_severity)
    {
    case DiagnosticSeverity::Warning:
        return "Warning";
    case DiagnosticSeverity::Error:
        return "Error";
    case DiagnosticSeverity::Fatal:
        return "Fatal";
    }
    return "Unknown";
}
} // namespace
#endif

/// @brief WindowsではDebug構成に限りErrorをDebuggerへ出力する
void report_error(const char* a_source, const Error& a_error, DiagnosticSeverity a_severity) noexcept
{
#if defined(CUE_DEBUG_OUTPUT)
    // 診断不能な入力からの例外や未定義動作を避ける
    if (a_source == nullptr)
    {
        return;
    }
    // 固定長Bufferに収め、診断処理からAllocationや例外を発生させない
    char message[256]{};
    std::snprintf(message, sizeof(message), "%s [%s]: %s (nativeCode=%lld)\n", a_source,
                  severity_name(a_severity), a_error.operation.c_str(),
                  static_cast<long long>(a_error.nativeCode));
    OutputDebugStringA(message);
#else
    static_cast<void>(a_source);
    static_cast<void>(a_error);
    static_cast<void>(a_severity);
#endif
}

/// @brief WindowsではDebug構成に限り診断文をDebuggerへ出力する
void report_message(const char* a_source, const char* a_message, DiagnosticSeverity a_severity) noexcept
{
#if defined(CUE_DEBUG_OUTPUT)
    // Process 終了中でも呼べるよう、入力の借用だけで整形する
    if (a_source == nullptr || a_message == nullptr)
    {
        return;
    }
    char message[256]{};
    std::snprintf(message, sizeof(message), "%s [%s]: %s\n", a_source,
                  severity_name(a_severity), a_message);
    OutputDebugStringA(message);
#else
    static_cast<void>(a_source);
    static_cast<void>(a_message);
    static_cast<void>(a_severity);
#endif
}
} // namespace cue
