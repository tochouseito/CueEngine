#pragma once

#include <Cue/Foundation/Error.h>

namespace cue
{
enum class DiagnosticSeverity
{
    Warning,
    Error,
    Fatal,
};

/// @brief Errorの重大度を添えて現在のPlatformの診断先へ報告する
///
/// Errorとsourceは呼出中だけ借用する。任意Threadから呼べ、再入可能とする
/// 診断失敗は伝播せず、Resultの伝播やProcessの終了判断は呼出側が行う
void report_error(const char* a_source, const Error& a_error, DiagnosticSeverity a_severity) noexcept;

/// @brief Errorを伴わない診断文を現在のPlatformの診断先へ報告する
///
/// sourceとmessageは呼出中だけ借用する。任意Threadから呼べ、再入可能とする
/// 診断失敗は伝播せず、例外やProcessの終了判断は呼出側が行う
void report_message(const char* a_source, const char* a_message, DiagnosticSeverity a_severity) noexcept;
} // namespace cue
