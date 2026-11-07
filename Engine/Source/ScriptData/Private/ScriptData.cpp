#include <ScriptData/ScriptData.h>

#include <algorithm>
#include <cmath>

#include <Foundation/Utf8.h>
#include <Foundation/Uuid.h>

namespace cue
{
namespace
{
/// @brief 表示文字列の容量と制御文字を検証し、通信 / File 境界で同じ規則を使う
bool valid_label(std::string_view a_value) noexcept
{
    return !a_value.empty() && a_value.size() <= 256 && is_valid_utf8_text(a_value) &&
           std::none_of(a_value.begin(), a_value.end(),
                        [](unsigned char a_char) { return a_char < 0x20 || a_char == 0x7f; });
}
/// @brief 外部入力の Schema 失敗を同じ分類へ揃える
Result<void> invalid(const char *a_operation)
{
    return Result<void>::failure({ErrorCategory::InvalidArgument, a_operation});
}
} // namespace

bool is_valid_script_id(std::string_view a_id, bool a_isType) noexcept
{
    return !a_id.empty() && a_id.size() <= 128 &&
           std::all_of(a_id.begin(), a_id.end(),
                       [a_isType](unsigned char a_char)
                       {
                           return (a_char >= 'a' && a_char <= 'z') || (a_char >= 'A' && a_char <= 'Z') ||
                                  (a_char >= '0' && a_char <= '9') || a_char == '_' || a_char == '-' ||
                                  (a_isType && a_char == '.');
                       });
}

Result<void> validate_script(const ScriptDescriptor &a_script)
{
    if (!is_valid_uuid_text(a_script.instanceId) || !is_valid_script_id(a_script.typeId, true) ||
        a_script.schemaVersion == 0 || a_script.properties.size() > k_maxScriptProperties)
        return invalid("Script.descriptor");
    for (std::size_t index = 0; index < a_script.properties.size(); ++index)
    {
        const auto &property = a_script.properties[index];
        if (!is_valid_script_id(property.propertyId) ||
            (property.kind != ScriptPropertyKind::Number && property.kind != ScriptPropertyKind::Boolean) ||
            !std::isfinite(property.number))
            return invalid("Script.property");
        for (std::size_t previous = 0; previous < index; ++previous)
            if (a_script.properties[previous].propertyId == property.propertyId)
                return invalid("Script.property_duplicate");
    }
    return Result<void>::success();
}

Result<void> validate_script_type(const ScriptTypeDescriptor &a_type)
{
    if (!is_valid_script_id(a_type.typeId, true) || !valid_label(a_type.displayName) || a_type.schemaVersion == 0 ||
        a_type.properties.size() > k_maxScriptProperties)
        return invalid("Script.type");
    for (std::size_t index = 0; index < a_type.properties.size(); ++index)
    {
        const auto &property = a_type.properties[index];
        if (!is_valid_script_id(property.propertyId) || !valid_label(property.displayName) ||
            (property.kind != ScriptPropertyKind::Number && property.kind != ScriptPropertyKind::Boolean) ||
            !std::isfinite(property.defaultNumber))
            return invalid("Script.type_property");
        for (std::size_t previous = 0; previous < index; ++previous)
            if (a_type.properties[previous].propertyId == property.propertyId)
                return invalid("Script.type_property_duplicate");
    }
    return Result<void>::success();
}

Result<void> validate_script_binding(const ScriptDescriptor &a_script, const ScriptTypeDescriptor &a_type)
{
    auto valid = validate_script(a_script);
    if (!valid.has_value())
        return valid;
    if (a_script.typeId != a_type.typeId || a_script.schemaVersion != a_type.schemaVersion)
        return invalid("Script.schema_mismatch");
    for (const auto &property : a_script.properties)
    {
        const auto found = std::find_if(a_type.properties.begin(), a_type.properties.end(),
                                        [&](const auto &a_value) { return a_value.propertyId == property.propertyId; });
        if (found == a_type.properties.end() || found->kind != property.kind)
            return invalid("Script.property_mismatch");
    }
    return Result<void>::success();
}
} // namespace cue
