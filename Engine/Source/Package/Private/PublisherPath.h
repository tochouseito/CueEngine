#pragma once

#include <Cue/Foundation/Result.h>
#include <Cue/IO/Filesystem.h>

#include <string_view>

namespace cue
{
class AssertContext;
}

namespace cue::package_private
{
/// @brief 同じWindows Native Rootに属するAbsolute Locatorを検証済みRoot相対Pathへ変換する
[[nodiscard]] Result<RelativePath> make_project_relative_path(std::string_view a_projectRoot,
                                                              std::string_view a_absoluteLocator,
                                                              const AssertContext &a_assertContext) noexcept;
} // namespace cue::package_private
