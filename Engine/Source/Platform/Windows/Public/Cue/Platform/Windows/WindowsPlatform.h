#pragma once

#include <Cue/Foundation/Result.h>
#include <Cue/Platform/WindowSystem.h>

#include <memory>

namespace cue
{
/// @brief Windows用WindowSystemを生成して呼出側へ一意所有権を渡す
///
/// 返却したSystemとWindowは呼出Threadで操作・破棄し、Windowを先に破棄する
/// 生成失敗では部分的なSystemを公開しない
[[nodiscard]] Result<std::unique_ptr<WindowSystem>> create_windows_window_system();
} // namespace cue
