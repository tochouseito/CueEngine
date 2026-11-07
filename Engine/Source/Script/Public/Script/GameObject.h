#pragma once

#include <cstdint>
#include <string_view>

#include <Foundation/Result.h>
#include <Math/Transform.h>
#include <Script/Abi.h>

namespace cue
{
/// @brief Module 境界の Status を SDK 内の所有 Error に変換する
[[nodiscard]] Result<void> script_result(CueScriptStatus a_status);
/// @brief Module 境界へ渡す固定長 Status を作る。例外処理中も確保しない
[[nodiscard]] CueScriptStatus script_status(const Error &a_error) noexcept;
/// @brief ABI 境界の未処理例外を確保なしの Fatal に変換する
[[nodiscard]] CueScriptStatus script_exception_status() noexcept;

/// @brief Entity の非所有窓口。Callback 中だけ EngineApi を借用し、Component の生 Pointer を渡さない
/// 同じ World の Owner Thread でだけ利用する。Callback 後に保持して再利用してはならない
class GameObject final
{
  public:
    /// @brief SDK の Module Adapter が検証済み API と Handle を束ねる
    GameObject(const CueScriptEngineApi &a_api, CueScriptEntity a_entity) noexcept;
    /// @brief 保存用 Identity と異なる、Process 内の世代付き参照値を返す
    [[nodiscard]] CueScriptEntity handle() const noexcept;
    /// @brief World に照会して Handle の失効を調べる
    [[nodiscard]] Result<bool> is_alive() const;
    /// @brief ECS が所有する Transform の値を複写する
    [[nodiscard]] Result<math::floatTransform> transform() const;
    /// @brief Transform の有限性を検証して World の正本へ反映する
    [[nodiscard]] Result<void> set_transform(const math::floatTransform &a_transform) const;
    /// @brief 指定 SceneInstance 内で名前を検索する。欠損・複数一致は失敗とする
    [[nodiscard]] Result<GameObject> find_by_name(CueScriptSceneInstance a_instance, std::string_view a_name) const;
    /// @brief Host の診断窓口へ通知する。ファイル出力先を Script 側で決めない
    [[nodiscard]] Result<void> report(std::uint32_t a_severity, std::string_view a_text) const;

  private:
    const CueScriptEngineApi &m_api;
    CueScriptEntity m_entity;
};
} // namespace cue
