#pragma once

#include <Cue/Foundation/Result.h>
#include <Cue/RuntimeHost/RuntimeHostStartup.h>

namespace cue
{
class AssertContext;
}

namespace cue::runtime_host
{
/// @brief Executable親DirectoryのModular Packageを検証しDynamic Query Provider経由で起動入力を構築する
[[nodiscard]] Result<RuntimeHostStartup> load_runtime_package(
    const AssertContext &a_assertContext) noexcept;
} // namespace cue::runtime_host
