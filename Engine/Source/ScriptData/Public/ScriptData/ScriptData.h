#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <Foundation/Result.h>

namespace cue
{
/// @brief Game Module の構成。C ABI の既存 configuration 値と対応し、Host の構成とは独立して選択する
enum class ScriptConfiguration : std::uint32_t
{
    Debug = 1,
    Development = 2,
    Release = 3
};

/// @brief 保存値や起動引数から受け取った構成が、既知の Game Module 構成か確認する
[[nodiscard]] constexpr bool is_valid_script_configuration(ScriptConfiguration a_configuration) noexcept
{
    return a_configuration == ScriptConfiguration::Debug || a_configuration == ScriptConfiguration::Development ||
           a_configuration == ScriptConfiguration::Release;
}

/// @brief 最初の Inspector / ABI が共有する値種別。型を変更するときは Schema を更新する
enum class ScriptPropertyKind : std::uint32_t
{
    Number = 1,
    Boolean = 2
};

/// @brief 永続 Property ID と型付き設定値。Runtime の Pointer / Handle を含めない
struct ScriptPropertyValue final
{
    std::string propertyId;
    ScriptPropertyKind kind = ScriptPropertyKind::Number;
    double number = 0;
    bool boolean = false;
    /// @brief 保存値を厳密比較し、編集履歴の不要な追加を防ぐ
    [[nodiscard]] bool operator==(const ScriptPropertyValue &) const = default;
};

/// @brief 一つの Script 割当の所有値。未知の型も Scene が保持し、実行前に解決する
struct ScriptDescriptor final
{
    std::string instanceId;
    std::string typeId;
    std::uint32_t schemaVersion = 1;
    bool isEnabled = true;
    std::vector<ScriptPropertyValue> properties;
    /// @brief 同一割当の設定を比較する。実行中 Instance の同一性とは別に扱う
    [[nodiscard]] bool operator==(const ScriptDescriptor &) const = default;
};

/// @brief Game Module から複写する公開 Property の不変 Metadata
struct ScriptPropertyDescriptor final
{
    std::string propertyId;
    std::string displayName;
    ScriptPropertyKind kind = ScriptPropertyKind::Number;
    double defaultNumber = 0;
    bool defaultBoolean = false;
};

/// @brief Editor が Game Code を実行せず利用できる型情報。Callback や DLL 参照は持たない
struct ScriptTypeDescriptor final
{
    std::string typeId;
    std::string displayName;
    std::uint32_t schemaVersion = 1;
    std::vector<ScriptPropertyDescriptor> properties;
};

inline constexpr std::size_t k_maxScriptsPerObject = 32;
inline constexpr std::size_t k_maxScriptProperties = 32;
inline constexpr std::size_t k_maxScriptTypes = 256;

/// @brief Schema の識別子を検証する。Type ID だけ Namespace の dot を許可する
[[nodiscard]] bool is_valid_script_id(std::string_view a_id, bool a_isType = false) noexcept;
/// @brief 型の解決と独立して保存設定を検証する。Owner / Thread に依存しない
[[nodiscard]] Result<void> validate_script(const ScriptDescriptor &a_script);
/// @brief DLL / 外部 Metadata の容量・型・重複・既定値を検証する
[[nodiscard]] Result<void> validate_script_type(const ScriptTypeDescriptor &a_type);
/// @brief 型と保存値の互換性を検証する。省略 Property は既定値を利用できる
[[nodiscard]] Result<void> validate_script_binding(const ScriptDescriptor &a_script,
                                                   const ScriptTypeDescriptor &a_type);
} // namespace cue
