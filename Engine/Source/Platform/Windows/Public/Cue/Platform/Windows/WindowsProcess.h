#pragma once

#include <Cue/Platform/Process.h>

#include <cstdint>
#include <memory>

namespace cue
{
class AssertContext;

/// @brief Windows Child Process境界の安定した失敗分類
enum class WindowsProcessError : std::int64_t
{
    InvalidRequest = 1,
    PipeCreationFailed,
    JobCreationFailed,
    InheritanceConfigurationFailed,
    ProcessCreationFailed,
    ProcessAssignmentFailed,
    ProcessResumeFailed,
    ProcessWaitFailed,
    ProcessTerminationFailed,
    ExitCodeQueryFailed,
    OutputCaptureFailed
};

/// @brief Job ObjectでProcess Treeを所有するWindows Child Process Runnerを生成する
///
/// AssertContextとその参照先はRunnerおよび進行中の`run`より長く生存させる。
[[nodiscard]] Result<std::unique_ptr<ChildProcessRunner>> create_windows_child_process_runner(
    const AssertContext &a_assertContext) noexcept;
} // namespace cue
