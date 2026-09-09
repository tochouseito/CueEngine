#pragma once

#include <Cue/Foundation/Error.h>

#include <cstdint>
#include <string_view>

namespace cue
{
class AssertContext;

/// @brief Project Descriptor の失敗を呼び出し側が分類するための Code
enum class ProjectError : std::int64_t
{
    InvalidFormat = 1,
    UnsupportedSchemaVersion = 2,
    InvalidProjectId = 3,
    InvalidDisplayName = 4,
    InvalidEngineCompatibility = 5,
    InvalidRoots = 6,
    IoFailure = 7,
    InvalidProjectName = 8,
    InvalidWorkspaceFormat = 9,
    UnsupportedWorkspaceVersion = 10,
    DuplicateProjectId = 11,
    ProjectNotRegistered = 12,
    InvalidProjectLocator = 13,
    ProjectLocatorConflict = 14,
    InvalidCompatibilityInput = 15,
    InvalidPinOrder = 16,
    InvalidDefaultScene = 17
};

/// @brief Project Error を診断 Summary と共に生成する
[[nodiscard]] Error make_project_error(const AssertContext &a_assertContext, ProjectError a_code,
                                       std::string_view a_summary) noexcept;
} // namespace cue
