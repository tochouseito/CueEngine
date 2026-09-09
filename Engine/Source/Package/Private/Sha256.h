#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace cue::package_private
{
using Sha256Digest = std::array<std::uint8_t, 32U>;

/// @brief 任意Byte列のSHA-256 DigestをAllocationなしで計算する
[[nodiscard]] Sha256Digest compute_sha256(std::span<const std::byte> a_bytes) noexcept;
} // namespace cue::package_private
