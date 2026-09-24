#pragma once

#include <Cue/Foundation/Result.h>
#include <Cue/Platform/Clock.h>
#include <Cue/Platform/Thread.h>
#include <Cue/Platform/Waiter.h>
#include <Cue/Platform/WindowSystem.h>

#include <memory>

namespace cue
{
/// @brief Windows用の時間・Thread実装を一括所有する
struct WindowsThreadServices final
{
    std::unique_ptr<Clock> clock;
    std::unique_ptr<Waiter> waiter;
    std::unique_ptr<ThreadFactory> threadFactory;
};

/// @brief Windowsの時計、待機点、Worker Factoryを生成する
///
/// Hostが一意所有し、Workerを停止・joinしてから破棄する。生成失敗では部分生成物を公開しない
[[nodiscard]] Result<WindowsThreadServices> create_windows_thread_services();

/// @brief Windows用WindowSystemを生成して呼出側へ一意所有権を渡す
///
/// 返却したSystemとWindowは呼出Threadで操作・破棄し、Windowを先に破棄する
/// 生成失敗では部分的なSystemを公開しない
[[nodiscard]] Result<std::unique_ptr<WindowSystem>> create_windows_window_system();
} // namespace cue
