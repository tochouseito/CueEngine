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
    LogLevel minimumLevel = LogLevel::Info;
    bool isFileEnabled = true;
    // Automatic は Debug / Development で有効、Release で無効
    DebugLogOutput debugOutput = DebugLogOutput::Automatic;
};
} // namespace cue
