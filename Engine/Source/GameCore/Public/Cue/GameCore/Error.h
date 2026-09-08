#pragma once

#include <Cue/Foundation/Error.h>

#include <cstdint>
#include <string_view>

namespace cue
{
class AssertContext;
}

namespace cue::game_core
{
/// @brief Entity と Component Storage の回復可能な失敗を分類する Code
enum class GameCoreError : std::int64_t
{
    InvalidEntity = 1,
    ComponentAlreadyExists = 2,
    ComponentNotFound = 3,
    ComponentTypeConflict = 4,
    UnregisteredComponent = 5,
    CapacityExceeded = 6,
    InvalidQuery = 7,
    DependencyFailed = 8,
    InvalidCommandBuffer = 9,
    InvalidRuntimeState = 10,
    InvalidClockConfiguration = 11,
    InvalidClockState = 12,
    InvalidClockSample = 13,
    ClockOverflow = 14
};

/// @brief GameCore Error を診断 Summary と共に生成する
[[nodiscard]] Error make_game_core_error(
    const AssertContext &a_assertContext, GameCoreError a_code,
    std::string_view a_summary) noexcept;
} // namespace cue::game_core
