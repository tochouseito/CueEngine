#pragma once

#include <ECS/ECSManager.h>
#include <Math/Projection.h>

namespace cue
{
/// @brief Camera の投影設定。位置・回転は同じ Entity の TransformComponent が所有する
struct CameraProjection final
{
    float verticalFovRadians = math::degrees_to_radians(60.0f);
    float nearClip = 0.1f;
    float farClip = 1000.0f;
};

/// @brief 透視投影の設定だけを所有する Component。Window / 描画先 / GPU Resource を持たない
/// Owner Thread で利用する。借用参照は Component の移動・削除・構造変更まで有効
class CameraComponent final : public ecs::IComponentTag
{
  public:
    /// @brief 有効な既定透視投影を持つ Camera を作る
    CameraComponent() = default;

    /// @brief Math で設定を検証してから置き換える。失敗時は旧設定を保持する
    [[nodiscard]] Result<void> set_projection(CameraProjection a_projection)
    {
        auto validated =
            math::perspective_rh_zo(a_projection.verticalFovRadians, 1.0f, a_projection.nearClip, a_projection.farClip);
        if (!validated.has_value())
            return Result<void>::failure(*validated.try_error());
        m_projection = a_projection;
        return Result<void>::success();
    }

    /// @brief Component が所有する投影設定を読み取り専用で借用する
    [[nodiscard]] const CameraProjection &projection() const noexcept
    {
        return m_projection;
    }

    /// @brief 表示先から渡された Aspect 比で Projection を計算する。Aspect は Component に保存しない
    [[nodiscard]] Result<math::float4x4> projection_matrix(float a_aspect) const
    {
        return math::perspective_rh_zo(m_projection.verticalFovRadians, a_aspect, m_projection.nearClip,
                                       m_projection.farClip);
    }

  private:
    CameraProjection m_projection{};
};
} // namespace cue
