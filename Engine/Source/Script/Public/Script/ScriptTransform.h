#pragma once

#include <cstdint>

#include <Foundation/Result.h>
#include <Math/Transform.h>
#include <Script/Abi.h>

namespace cue
{
class Behaviour;

/// @brief Behaviour の Entity にある Local Transform を同期操作する窓口
/// World の Owner Thread の Script Callback 中に利用する。ECS の Component を借用しない
class ScriptTransform final
{
  public:
    class Vector;

    /// @brief 一軸の変更をその場で適用する。演算子での失敗は現在の Callback に記録する
    class Axis final
    {
      public:
        /// @brief 軸の窓口を Script Callback の外へコピーさせない
        Axis(const Axis &) = delete;
        /// @brief 軸の所属を変更させない
        Axis &operator=(const Axis &) = delete;
        /// @brief 現在の座標を所有値として返す
        [[nodiscard]] Result<float> get() const;
        /// @brief この軸だけを検証して置き換える
        [[nodiscard]] Result<void> set(float a_value) const;
        /// @brief 現在値への加算を一度の ABI 呼出で反映する
        [[nodiscard]] Result<void> add(float a_value) const;
        /// @brief 現在値に倍率を掛けて検証する
        [[nodiscard]] Result<void> multiply(float a_factor) const;
        /// @brief 設定の失敗を Callback に記録する
        void operator=(float a_value) const;
        /// @brief 加算の失敗を Callback に記録する
        void operator+=(float a_value) const;
        /// @brief 減算の失敗を Callback に記録する
        void operator-=(float a_value) const;
        /// @brief 倍率変更の失敗を Callback に記録する
        void operator*=(float a_factor) const;

      private:
        friend class Vector;
        /// @brief 所属するベクトルと軸を固定する
        Axis(const Vector &a_vector, std::uint32_t a_axis) noexcept;
        /// @brief 固定した軸にだけ指定の操作を適用する
        [[nodiscard]] Result<void> change(std::uint32_t a_operation, float a_value) const;

        const Vector &m_vector;
        std::uint32_t m_axis;
    };

    /// @brief 位置または Scale の三軸を公開する
    class Vector final
    {
      public:
        /// @brief 位置と Scale の窓口を別の Behaviour へコピーさせない
        Vector(const Vector &) = delete;
        /// @brief ベクトルの所属を変更させない
        Vector &operator=(const Vector &) = delete;
        /// @brief 数学関数に渡せる三軸の所有値を返す
        [[nodiscard]] Result<math::float3> get() const;

        Axis x;
        Axis y;
        Axis z;

      private:
        friend class ScriptTransform;
        friend class Axis;
        /// @brief 対象となる Transform とフィールドを固定する
        Vector(const ScriptTransform &a_transform, std::uint32_t a_field) noexcept;

        const ScriptTransform &m_transform;
        std::uint32_t m_field;
    };

    /// @brief 所属 Entity の位置・回転・Scale を所有値として取得する
    [[nodiscard]] Result<math::floatTransform> get() const;
    /// @brief 全項目を検証して一度に置き換える。失敗時は旧値を保持する
    [[nodiscard]] Result<void> set(const math::floatTransform &a_value) const;
    /// @brief 指定軸の相対移動を一度に反映する。省略軸は移動しない
    [[nodiscard]] Result<void> translate(float a_x, float a_y = 0.0f, float a_z = 0.0f) const;
    /// @brief 移動ベクトルを検証し、全軸を一度に反映する
    [[nodiscard]] Result<void> translate(math::float3 a_offset) const;

    Vector position;
    Vector scale;

  private:
    friend class Behaviour;
    /// @brief Behaviour ごとに窓口を生成する。Entity は Callback が決める
    explicit ScriptTransform(Behaviour &a_owner) noexcept;
    /// @brief 借用中の GameObject を使い、一度の呼出で変更を適用する
    [[nodiscard]] Result<void> change(const CueScriptTransformChange &a_change) const;
    /// @brief 演算子の失敗を現在の Callback の終了結果へ伝える
    void remember(Result<void> a_result) const;

    Behaviour &m_owner;
};
} // namespace cue
