#pragma once

#include <new>
#include <string>
#include <string_view>
#include <utility>

#include <Assets/BuiltinMesh.h>
#include <ECS/ECSManager.h>

namespace cue
{
/// @brief CPU Asset Identity だけを所有し、描画先の MeshHandle / GPU Resource は所有しない
/// Owner Thread で利用する。現段階は組み込み Cube のみ対応し、未知 ID を黙って置き換えない
class MeshRendererComponent final : public ecs::IComponentTag
{
  public:
    /// @brief 組み込み Cube を参照する Component を作る
    MeshRendererComponent() = default;

    /// @brief 対応する ID を検証して所有する。失敗時は旧参照を保持する
    [[nodiscard]] Result<void> set_mesh_id(std::string_view a_assetId)
    {
        if (a_assetId != k_cubeMeshId)
            return Result<void>::failure({ErrorCategory::InvalidArgument, "MeshRendererComponent.asset"});
        try
        {
            std::string candidate(a_assetId);
            m_assetId = std::move(candidate);
            return Result<void>::success();
        }
        catch (const std::bad_alloc &)
        {
            return Result<void>::failure({ErrorCategory::PlatformFailure, "MeshRendererComponent.allocation"});
        }
    }

    /// @brief 所有 ID を Component の次の変更・移動・削除まで借用する
    [[nodiscard]] std::string_view mesh_id() const noexcept
    {
        return m_assetId;
    }

  private:
    std::string m_assetId{k_cubeMeshId};
};
} // namespace cue
