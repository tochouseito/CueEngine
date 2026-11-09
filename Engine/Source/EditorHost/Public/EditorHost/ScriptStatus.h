#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include <Foundation/Error.h>

namespace cue
{
/// @brief Editor が進める IDE / Script 操作。外部 Process の状態と編集正本を混同しない
enum class EditorScriptState
{
    Idle,
    PreparingWorkspace,
    OpeningIde,
    BuildingScripts,
    ReloadingScripts,
    StartingDebug,
    AttachingDebugger,
    Debugging,
    Succeeded,
    Failed,
    Cancelled
};
/// @brief 次の Editor 操作まで借用する、IDE 連携の表示値
struct EditorScriptStatus final
{
    EditorScriptState state = EditorScriptState::Idle;
    bool isBusy = false;
    std::uint32_t processId = 0;
    std::string message;
    std::optional<Error> error;
};
} // namespace cue
