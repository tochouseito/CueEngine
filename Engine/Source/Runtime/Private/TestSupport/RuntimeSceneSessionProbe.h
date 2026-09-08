#pragma once

#include "RuntimeSceneSessionInternals.h"

#include <Cue/Runtime/RuntimeSceneSession.h>
#include <Cue/Schema/Types.h>

namespace cue::runtime::test_support
{
/// @brief TestだけがScene終了Operationを差し替えてProductionと同じSession経路を通す
class RuntimeSceneSessionProbe final
{
  public:
    /// @brief 注入Operationを使用してRuntimeSceneSessionの実Start経路を実行する
    [[nodiscard]] static Result<std::unique_ptr<RuntimeSceneSession>> start(
        const scene::SceneSnapshot &a_snapshot, game_core::WorldIdentitySource &a_identitySource,
        const schema::SchemaRegistry &a_schemaRegistry, schema::TypeId a_transformTypeId,
        schema::TypeId a_sceneObjectStateTypeId, const AssertContext &a_assertContext,
        const details::SceneEndOperation &a_endOperation) noexcept
    {
        return RuntimeSceneSession::start_with_operation(
            a_snapshot, a_identitySource, a_schemaRegistry, std::move(a_transformTypeId),
            std::move(a_sceneObjectStateTypeId), a_assertContext, a_endOperation);
    }
};
} // namespace cue::runtime::test_support
