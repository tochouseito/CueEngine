#pragma once

#include <cstdint>

// 手動調査用 Build だけが Include する。設定は Worker 起動前に固定し、停止まで変更しない
namespace cue::investigation
{
inline std::uint32_t options = 0;
inline constexpr std::uint32_t k_compactStatistics = 1;
inline constexpr std::uint32_t k_cacheFrequency = 2;
inline constexpr std::uint32_t k_ownerInput = 4;

/// @brief 実行中不変の実験条件を確認する。通常 Build はこの経路を持たない
[[nodiscard]] inline bool enabled(std::uint32_t a_option) noexcept
{
    return (options & a_option) != 0;
}
} // namespace cue::investigation
