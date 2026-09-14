#pragma once

#include <Cue/Foundation/Error.h>

#include <cstdint>
#include <string_view>

namespace cue
{
class AssertContext;
}

namespace cue::project_hub
{
/// @brief Project Hub Application Service の回復可能な失敗分類
enum class ProjectHubError : std::int64_t
{
    InvalidConfiguration = 1,
    InvalidLocator = 2,
    ProjectMissing = 3,
    ProjectBroken = 4,
    ProjectIdentityMismatch = 5,
    ProjectUnsupported = 6,
    ProjectNotFound = 7,
    InvalidSceneLocator = 8,
    PersistenceFailure = 9,
    InvalidTemplate = 10,
    EditorLaunchFailed = 11,
    EditorProcessFailed = 12,
    OpenRejectedViewDurabilityUnknown = 13,
    ProjectFolderOpenFailed = 14
};

/// @brief Project Hub Error を診断 Summary と共に生成する
[[nodiscard]] Error make_project_hub_error(const AssertContext &a_assertContext, ProjectHubError a_code,
                                           std::string_view a_summary) noexcept;

/// @brief 下位 Module の失敗を Project Hub Error へ再分類する
[[nodiscard]] Error reclassify_project_hub_error(const AssertContext &a_assertContext, ProjectHubError a_code,
                                                 std::string_view a_summary, Error &&a_cause) noexcept;
} // namespace cue::project_hub
