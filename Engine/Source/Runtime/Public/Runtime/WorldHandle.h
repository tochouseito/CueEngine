#pragma once

#include <cstdint>

namespace cue
{
/// @brief World 内の Entity を参照する非所有値。World に照会して世代と生存を検証する
/// Process 内だけ有効で、永続 Object ID として保存しない。所有 World の寿命は延長しない
struct EntityHandle final
{
    std::uint64_t world = 0;
    std::uint32_t index = 0;
    std::uint64_t generation = 0;

    /// @brief Handle の値が一致するか返す。生存判定とは異なる
    [[nodiscard]] constexpr bool operator==(const EntityHandle &) const noexcept = default;
};

/// @brief 同じ Scene 定義から作った各実体を区別する、世代付きの非所有値
struct SceneInstanceHandle final
{
    std::uint64_t world = 0;
    std::uint32_t index = 0;
    std::uint64_t generation = 0;

    /// @brief Handle の値が一致するか返す。生存判定とは異なる
    [[nodiscard]] constexpr bool operator==(const SceneInstanceHandle &) const noexcept = default;
};
} // namespace cue
