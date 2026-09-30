#pragma once

#include <Cue/Foundation/Result.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cue
{
/// @brief Map 済み Upload 領域へスロット単位の更新をまとめて書き込む
/// @details 領域は非所有であり、Uploader と GPU の読取が終わるまで呼出側が保持する。同時呼出と再入は行わない
template <typename T>
class SlotUploader final
{
    static_assert(std::is_trivially_copyable_v<T>);

public:
    /// @brief 整列後の必要バイト数と借用領域を検証して作成する
    [[nodiscard]] static Result<SlotUploader> create(std::size_t a_capacity, std::size_t a_alignment,
                                                      std::byte* a_mappedData, std::size_t a_mappedBytes)
    {
        using UploaderResult = Result<SlotUploader>;
        if (a_capacity == 0 || a_alignment == 0 || !a_mappedData ||
            sizeof(T) > std::numeric_limits<std::size_t>::max() - (a_alignment - 1))
        {
            return UploaderResult::failure({ErrorCategory::InvalidArgument, "SlotUploader.create"});
        }
        const auto stride = ((sizeof(T) + a_alignment - 1) / a_alignment) * a_alignment;
        if (a_capacity > a_mappedBytes / stride)
        {
            return UploaderResult::failure({ErrorCategory::InvalidArgument, "SlotUploader.create.capacity"});
        }
        SlotUploader uploader;
        uploader.m_capacity = a_capacity;
        uploader.m_stride = stride;
        uploader.m_mappedData = a_mappedData;
        uploader.m_pending.reserve(a_capacity);
        return UploaderResult::success(std::move(uploader));
    }

    /// @brief 前 Frame の更新要求を捨て、次 Frame の入力を受け付ける
    void begin_frame() noexcept { m_pending.clear(); }

    /// @brief スロットの書込要求を追加し、同一スロットの最終要求を優先する
    [[nodiscard]] Result<void> push(std::uint32_t a_slot, const T& a_value)
    {
        if (a_slot >= m_capacity)
        {
            return Result<void>::failure({ErrorCategory::InvalidArgument, "SlotUploader.push"});
        }
        m_pending.push_back({a_value, a_slot});
        return Result<void>::success();
    }

    /// @brief 重複を畳み、連続スロットをまとめて Map 済み領域へコピーする
    [[nodiscard]] Result<void> commit()
    {
        if (!m_mappedData)
        {
            return Result<void>::failure({ErrorCategory::InvalidState, "SlotUploader.commit"});
        }
        std::vector<UploadData> unique;
        unique.reserve(m_pending.size());
        std::unordered_map<std::uint32_t, std::size_t> positions;
        positions.reserve(m_pending.size());
        for (const auto& entry : m_pending)
        {
            const auto [position, isNew] = positions.try_emplace(entry.slot, unique.size());
            if (isNew)
            {
                unique.push_back(entry);
            }
            else
            {
                unique[position->second].value = entry.value;
            }
        }
        std::sort(unique.begin(), unique.end(), [](const UploadData& a_left, const UploadData& a_right)
        {
            return a_left.slot < a_right.slot;
        });
        for (std::size_t first = 0; first < unique.size();)
        {
            std::size_t end = first + 1;
            while (end < unique.size() && unique[end].slot == unique[end - 1].slot + 1)
            {
                ++end;
            }
            const auto bytes = (end - first) * m_stride;
            m_staging.assign(bytes, std::byte{});
            for (std::size_t index = first; index < end; ++index)
            {
                std::memcpy(m_staging.data() + (index - first) * m_stride,
                            std::addressof(unique[index].value), sizeof(T));
            }
            std::memcpy(m_mappedData + static_cast<std::size_t>(unique[first].slot) * m_stride,
                        m_staging.data(), bytes);
            first = end;
        }
        return Result<void>::success();
    }

    /// @brief 一要素が Map 済み領域で占める整列後のバイト数を返す
    [[nodiscard]] std::size_t stride() const noexcept { return m_stride; }

private:
    SlotUploader() = default;

    struct UploadData final
    {
        T value;
        std::uint32_t slot = 0;
    };

    std::size_t m_capacity = 0;
    std::size_t m_stride = 0;
    std::byte* m_mappedData = nullptr;
    std::vector<UploadData> m_pending;
    std::vector<std::byte> m_staging;
};
} // namespace cue
