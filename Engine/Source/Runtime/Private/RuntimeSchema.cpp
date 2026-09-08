#include <Cue/Runtime/RuntimeSchema.h>

#include <Cue/Schema/Descriptor.h>
#include <Cue/Schema/Registry.h>

#include <string_view>
#include <utility>
#include <vector>

namespace
{
constexpr std::string_view k_transformTypeId = "50000000-0000-4000-8000-000000000005";
constexpr std::string_view k_sceneObjectStateTypeId = "10000000-0000-4000-8000-000000000001";

/// @brief Fieldを持たないM14 Runtime Core Type Descriptorを生成する
[[nodiscard]] cue::Result<cue::schema::TypeDescriptor> make_type_descriptor(
    cue::schema::TypeId a_typeId, std::string_view a_name, const cue::AssertContext &a_assertContext) noexcept
{
    cue::Result<cue::schema::SchemaVersion> version = cue::schema::SchemaVersion::create(1U, a_assertContext);
    if (!version)
    {
        return cue::Result<cue::schema::TypeDescriptor>::failure(std::move(*version.try_error()));
    }

    std::vector<cue::schema::FieldDescriptor> fields;
    std::vector<cue::schema::FieldId> reserved;
    return cue::schema::create_type_descriptor(std::move(a_typeId), a_name, std::move(*version.try_value()),
                                               std::move(fields), std::move(reserved), a_assertContext);
}
} // namespace

namespace cue::runtime
{
Result<RuntimeSchemaTypeIds> make_runtime_schema_type_ids(const AssertContext &a_assertContext) noexcept
{
    Result<schema::TypeId> transform = schema::TypeId::parse(k_transformTypeId, a_assertContext);
    if (!transform)
    {
        return Result<RuntimeSchemaTypeIds>::failure(std::move(*transform.try_error()));
    }
    Result<schema::TypeId> sceneObjectState = schema::TypeId::parse(k_sceneObjectStateTypeId, a_assertContext);
    if (!sceneObjectState)
    {
        return Result<RuntimeSchemaTypeIds>::failure(std::move(*sceneObjectState.try_error()));
    }
    return Result<RuntimeSchemaTypeIds>::success(
        {std::move(*transform.try_value()), std::move(*sceneObjectState.try_value())});
}

Result<void> add_runtime_schema_types(schema::SchemaRegistryBuilder &a_builder,
                                      const AssertContext &a_assertContext) noexcept
{
    Result<RuntimeSchemaTypeIds> typeIds = make_runtime_schema_type_ids(a_assertContext);
    if (!typeIds)
    {
        return Result<void>::failure(std::move(*typeIds.try_error()));
    }

    Result<schema::TypeDescriptor> transform =
        make_type_descriptor(std::move(typeIds.try_value()->transform), "Cue.Core.Transform", a_assertContext);
    if (!transform)
    {
        return Result<void>::failure(std::move(*transform.try_error()));
    }
    Result<void> addedTransform = a_builder.add_type(std::move(*transform.try_value()));
    if (!addedTransform)
    {
        return addedTransform;
    }

    Result<schema::TypeDescriptor> sceneObjectState = make_type_descriptor(
        std::move(typeIds.try_value()->sceneObjectState), "Cue.Scene.SceneObjectState", a_assertContext);
    if (!sceneObjectState)
    {
        return Result<void>::failure(std::move(*sceneObjectState.try_error()));
    }
    return a_builder.add_type(std::move(*sceneObjectState.try_value()));
}
} // namespace cue::runtime
