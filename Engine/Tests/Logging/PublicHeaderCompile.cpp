#include <Foundation/Logging.h>
#include <Logging/FileLogSink.h>
#include <Logging/Logger.h>

#ifdef _WINDOWS_
#error Logging の公開 Header は Windows SDK を漏らしてはならない
#endif

/// @brief 抽象 File API だけで Logger の公開 Header を利用できることを確認する
int main()
{
    return static_cast<int>(cue::LogLevel::Trace);
}
