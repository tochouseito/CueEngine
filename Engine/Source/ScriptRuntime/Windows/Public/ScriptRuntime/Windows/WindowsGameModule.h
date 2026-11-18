#pragma once

#include <Foundation/Path.h>
#include <ScriptRuntime/ScriptModule.h>

namespace cue
{
/// @brief 絶対 Path の DLL をロードし、選択した構成の Game Module を所有値として返す
/// Play 子 Process の Owner Thread で呼ぶ。失敗時はロードした DLL を解放し、部分値を公開しない
[[nodiscard]] Result<std::unique_ptr<RuntimeGameModule>> load_windows_game_module(
    const Path &a_file, std::optional<ScriptConfiguration> a_configuration = std::nullopt);
} // namespace cue
