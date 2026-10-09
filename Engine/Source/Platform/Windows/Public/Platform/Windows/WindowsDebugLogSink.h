#pragma once

#include <memory>

#include <Foundation/Logging.h>

namespace cue
{
/// @brief UTF-8 の整形済み行を OutputDebugStringW へ配送する Sink を生成する
/// 呼出側が一意所有し、全操作を直列化する。再入しない。無効時は出力せず成功する
/// Native API に成功応答はなく、Debugger が受信したことは保証しない
[[nodiscard]] std::unique_ptr<ILogSink> create_windows_debug_log_sink(bool a_isEnabled = true);
} // namespace cue
