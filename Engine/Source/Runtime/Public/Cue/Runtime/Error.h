#pragma once

#include <Cue/Foundation/Error.h>

#include <cstdint>
#include <string_view>

namespace cue
{
class AssertContext;
}

namespace cue::runtime
{
/// @brief Runtime Session所有境界の回復可能な失敗を分類するCode
enum class RuntimeError : std::int64_t
{
    InvalidSceneSessionState = 1,
    SceneSessionStartFailed = 2,
    SceneCleanupFailed = 3,
    RuntimeWorldShutdownFailed = 4
};

/// @brief Runtime Errorを診断Summaryと共に生成する
[[nodiscard]] Error make_runtime_error(const AssertContext &a_assertContext, RuntimeError a_code,
                                       std::string_view a_summary) noexcept;
} // namespace cue::runtime
