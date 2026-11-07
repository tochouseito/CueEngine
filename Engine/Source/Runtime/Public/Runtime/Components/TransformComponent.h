#pragma once

#include <ECS/ECSManager.h>
#include <Math/Transform.h>

namespace cue
{
class World;

/// @brief 親なし Entity の Local Transform と、最後に同期した World 行列を所有する
/// 取得した参照は Component の移動・削除・Pool の構造変更まで有効。Owner Thread で利用する
class TransformComponent final : public ecs::IComponentTag
{
  public:
    /// @brief 原点・単位回転・単位 Scale と単位行列で初期化する
    TransformComponent() = default;
    /// @brief 変更前に数学的な有効性を検証する。失敗時は Local / 行列を保持する
    [[nodiscard]] Result<void> set_local(const math::floatTransform &a_local)
    {
        auto matrix = a_local.to_matrix();
        if (!matrix.has_value())
            return Result<void>::failure(*matrix.try_error());
        m_local = a_local;
        m_isDirty = true;
        return Result<void>::success();
    }
    /// @brief 所有している Local の正本を借用する
    [[nodiscard]] const math::floatTransform &local() const noexcept
    {
        return m_local;
    }
    /// @brief 最後の World 同期時点の行列を借用する。Dirty 中は直前の同期値となる
    [[nodiscard]] const math::float4x4 &world_matrix() const noexcept
    {
        return m_world;
    }
    /// @brief 次の World 同期で行列を更新する必要があるか返す
    [[nodiscard]] bool is_dirty() const noexcept
    {
        return m_isDirty;
    }

  private:
    friend class World;

    /// @brief Local から行列を更新する。計算失敗では既存行列を変更しない
    [[nodiscard]] Result<void> synchronize()
    {
        if (!m_isDirty)
            return Result<void>::success();
        auto matrix = m_local.to_matrix();
        if (!matrix.has_value())
            return Result<void>::failure(*matrix.try_error());
        m_world = *matrix.try_value();
        m_isDirty = false;
        return Result<void>::success();
    }

    math::floatTransform m_local{};
    math::float4x4 m_world{};
    bool m_isDirty = true;
};
} // namespace cue
