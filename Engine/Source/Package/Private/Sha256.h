#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace cue::package_private
{
using Sha256Digest = std::array<std::uint8_t, 32U>;

/// @brief 大容量Fileを全保持せずSHA-256へ逐次供給するState
class Sha256 final
{
  public:
    /// @brief SHA-256初期Stateを構築する
    Sha256() noexcept;

    /// @brief 次のByte列を取り込み、SHA-256 bit length上限超過時はfalseを返す
    [[nodiscard]] bool update(std::span<const std::byte> a_bytes) noexcept;
    /// @brief Paddingを適用してDigestを返す
    [[nodiscard]] Sha256Digest finish() noexcept;

  private:
    std::array<std::uint32_t, 8U> m_state;
    std::array<std::byte, 64U> m_buffer{};
    std::size_t m_bufferSize = 0U;
    std::uint64_t m_totalBytes = 0U;
    bool m_finished = false;
};

/// @brief 任意Byte列のSHA-256 DigestをAllocationなしで計算する
[[nodiscard]] Sha256Digest compute_sha256(std::span<const std::byte> a_bytes) noexcept;
} // namespace cue::package_private
