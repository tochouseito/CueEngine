#pragma once

#include <Cue/Foundation/Result.h>
#include <Cue/RuntimeHost/GameModuleQueryProvider.h>
#include <Cue/RuntimeHost/RuntimeHostStartup.h>

#include <cstdint>
#include <string_view>

namespace cue
{
class AssertContext;
}

namespace cue::runtime_host
{
/// @brief Static ProductへCompile時固定するPackage Trust Policy
enum class StaticRuntimeTrustMode : std::uint8_t
{
    UnsignedLocal,
    PublisherSigned
};

/// @brief Executable親DirectoryのMonolithic Packageを検証しStatic Query Entryから起動入力を構築する
///
/// Query Entryと期待Identityは呼出中だけ借用する。Manifest v2、完全File Inventory、Runtime Dataの
/// Canonical表現、Project、Release構成、Execution Model、Trust Policyを検証し、不一致時は起動入力を返さない。
/// PublisherSignedはDetached Signatureと外部Trust Anchorの検証が実装されるまで常に拒否する。
[[nodiscard]] Result<RuntimeHostStartup> load_static_runtime_package(GameModuleQueryFunction a_query,
                                                                     std::string_view a_expectedProjectId,
                                                                     StaticRuntimeTrustMode a_expectedTrustMode,
                                                                     std::string_view a_expectedPublisherKeyId,
                                                                     const AssertContext &a_assertContext) noexcept;
} // namespace cue::runtime_host
