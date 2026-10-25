#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace cue::study
{
inline constexpr auto k_noDescriptor = (std::numeric_limits<std::size_t>::max)();

/// @brief 調査用に GPU Handle の物理 Index から登録中の Descriptor を逆引きする
/// Map は Heap の容量と同数を保持し、予約・解放時だけ Context Lock 内で更新する
/// Native ID 自体には世代がないため、既存 Snapshot Pin と GPU Fence の寿命契約を別途維持する
template <typename T>
[[nodiscard]] std::size_t find_descriptor_index(std::uint64_t a_id, std::uint64_t a_base, std::uint32_t a_stride,
    std::span<const std::size_t> a_map, std::span<const T> a_descriptors) noexcept
{
    if (a_stride == 0 || a_id < a_base)
    {
        return k_noDescriptor;
    }
    const auto offset = a_id - a_base;
    if (offset % a_stride != 0 || offset / a_stride >= a_map.size())
    {
        return k_noDescriptor;
    }
    const auto index = a_map[static_cast<std::size_t>(offset / a_stride)];
    if (index >= a_descriptors.size())
    {
        return k_noDescriptor;
    }
    const auto &slot = a_descriptors[index];
    if (!slot.handle.is_valid() || slot.isReserved || slot.gpu.ptr != a_id)
    {
        return k_noDescriptor;
    }
    return index;
}
} // namespace cue::study
