#pragma once

#include <Cue/Foundation/Result.h>
#include <Cue/Package/Manifest.h>

#include <string_view>

namespace cue
{
class AssertContext;
}

namespace cue::package_private
{
/// @brief Manifest v2のNative Package Rootを完全列挙し未知または間接Entryを拒否する
[[nodiscard]] Result<void> verify_monolithic_package_tree(std::string_view a_packageRoot,
                                                          const package::PackageManifest &a_manifest,
                                                          const AssertContext &a_assertContext) noexcept;
} // namespace cue::package_private
