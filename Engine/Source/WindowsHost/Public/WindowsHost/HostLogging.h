#pragma once

#include <Foundation/Logging.h>

namespace cue
{
enum class DebugLogOutput
{
    Automatic,
    Enabled,
    Disabled
};
/// @brief Host が所有するログ出力の起動設定
struct HostLoggingConfig final
{
    // この重要度より低いログを出力対象から除外する
    LogLevel minimumLevel = LogLevel::Info;
    // File 出力を有効にし、Host が生成した保存先へ記録する
    bool isFileEnabled = true;
    // Automatic は Debug / Development で有効、Release で無効
    DebugLogOutput debugOutput = DebugLogOutput::Automatic;
};
} // namespace cue
