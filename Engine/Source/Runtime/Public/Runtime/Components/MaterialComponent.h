#pragma once

#include <new>
#include <string>
#include <string_view>
#include <utility>

#include <Assets/AssetCatalog.h>
#include <ECS/ECSManager.h>
#include <Foundation/Result.h>

namespace cue
{
/// @brief Object と Material の永続参照を結ぶ。Owner Thread 上でのみ読み書きする
class MaterialComponent final : public ecs::IComponentTag
{
  public:
    /// @brief Asset ID を新しい割り当てとして所有する。失敗時は既存値を保持する
    [[nodiscard]] Result<void> set_material_id(std::string_view a_assetId)
    {
        if (!is_valid_asset_id(a_assetId))
            return Result<void>::failure({ErrorCategory::InvalidArgument, "MaterialComponent.asset"});
        try
        {
            std::string candidate(a_assetId);
            m_assetId = std::move(candidate);
            return Result<void>::success();
        }
        catch (const std::bad_alloc &)
        {
            return Result<void>::failure({ErrorCategory::PlatformFailure, "MaterialComponent.allocation"});
        }
    }

    /// @brief 次の変更・移動・削除まで ID を借用する。未割り当ての初期値は空
    [[nodiscard]] std::string_view material_id() const noexcept
    {
        return m_assetId;
    }

  private:
    std::string m_assetId;
};
} // namespace cue
