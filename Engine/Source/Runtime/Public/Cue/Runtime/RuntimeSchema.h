#pragma once

#include <Cue/Foundation/Result.h>
#include <Cue/Schema/Types.h>

namespace cue
{
class AssertContext;
}

namespace cue::schema
{
class SchemaRegistryBuilder;
}

namespace cue::runtime
{
/// @brief RuntimeWorldとScene実体化が共有するCore Schema TypeのStable Identity集合
struct RuntimeSchemaTypeIds final
{
    schema::TypeId transform;
    schema::TypeId sceneObjectState;
};

/// @brief Runtime Core TypeのCanonical Identityを検証済みValueへ変換する
[[nodiscard]] Result<RuntimeSchemaTypeIds> make_runtime_schema_type_ids(const AssertContext &a_assertContext) noexcept;

/// @brief Runtime Core Typeを未SealのSchema Registry Builderへ一度ずつ登録する
[[nodiscard]] Result<void> add_runtime_schema_types(schema::SchemaRegistryBuilder &a_builder,
                                                    const AssertContext &a_assertContext) noexcept;
} // namespace cue::runtime
