#pragma once

#include <cstddef>
#include <cstdint>

#include <Assets/ModelAsset.h>

namespace cue::detail
{
inline constexpr std::size_t k_modelHeaderSize = 96;
inline constexpr std::size_t k_maxModelSize = 64 * 1024 * 1024;

/// @brief 索引に必要な ID と Geometry の境界を固定長 Header から検証する
[[nodiscard]] Result<ModelAsset> decode_model_header(std::span<const std::byte> a_header, std::uint64_t a_fileSize,
                                                    Path a_source = {});

/// @brief Content 走査時に Model File の Header だけを読む
[[nodiscard]] Result<ModelAsset> read_model_header(IFileSystem &a_files, const Path &a_source);
} // namespace cue::detail
