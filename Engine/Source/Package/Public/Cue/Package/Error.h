#pragma once

#include <Cue/Foundation/Error.h>

#include <cstdint>
#include <string_view>

namespace cue
{
class AssertContext;
}

namespace cue::package
{
/// @brief Minimal Runtime Data Publisherの回復可能Error分類
enum class PackageError : std::int64_t
{
    MissingStartupScene = 1,
    StartupSceneIdentityMismatch = 2,
    UnsupportedRuntimeSceneData = 3,
    RuntimeDataResourceLimitExceeded = 4,
    InvalidRuntimeData = 5
};

/// @brief Package Domainの回復可能Errorを生成する
[[nodiscard]] Error make_package_error(const AssertContext &a_assertContext, PackageError a_error,
                                       std::string_view a_summary) noexcept;
} // namespace cue::package
