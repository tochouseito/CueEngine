#pragma once

#include <Cue/Renderer/RHI/GpuResources.h>
#include <Cue/Renderer/RHI/SlotUploader.h>

#include <limits>
#include <string_view>
#include <vector>

namespace cue
{
/// @brief Buffer の作成と CPU 転送を担う契約
/// @details Handle の実所有者は実装側の Resource Pool。Render Thread から直列に呼ぶ
class IBufferManager
{
public:
    virtual ~IBufferManager() = default;

    /// @brief 指定 Memory に Buffer を生成する
    [[nodiscard]] virtual Result<GpuResourceHandle> create_buffer(GpuBufferDesc a_desc) = 0;

    /// @brief 名前付きで作成した Buffer の現行 Handle を返す
    [[nodiscard]] virtual Result<GpuResourceHandle> get_buffer(std::string_view a_name) const = 0;

    /// @brief GPU 完了後に View を持たない Buffer を解放する
    [[nodiscard]] virtual Result<void> destroy_buffer(GpuResourceHandle a_buffer) = 0;

    /// @brief Upload Buffer へ CPU Data を書く
    [[nodiscard]] virtual Result<void> write_buffer(GpuResourceHandle a_buffer, std::uint64_t a_offset,
                                                     const void* a_data, std::uint64_t a_size) = 0;

    /// @brief GPU 完了済み Readback Buffer から CPU Data を読む
    [[nodiscard]] virtual Result<void> read_buffer(GpuResourceHandle a_buffer, std::uint64_t a_offset,
                                                    void* a_data, std::uint64_t a_size) = 0;

    /// @brief 永続 Map 済み Upload Slice を Buffer の寿命内だけ貸す
    [[nodiscard]] virtual Result<GpuBufferCpuView> get_upload_buffer_view(GpuResourceHandle a_buffer) = 0;

    /// @brief GPU 完了後に永続 Map 済み Readback Slice を貸す
    [[nodiscard]] virtual Result<GpuBufferCpuView> get_readback_buffer_view(GpuResourceHandle a_buffer) = 0;

    /// @brief Upload Slice ごとに SlotUploader を構築して返す
    template <typename T>
    [[nodiscard]] Result<std::vector<SlotUploader<T>>> create_slot_uploaders(
        GpuResourceHandle a_buffer, std::uint32_t a_bufferCount)
    {
        using UploadersResult = Result<std::vector<SlotUploader<T>>>;
        auto viewResult = get_upload_buffer_view(a_buffer);
        if (!viewResult.has_value())
        {
            return UploadersResult::failure(*viewResult.try_error());
        }
        const auto view = viewResult.take_value();
        if (a_bufferCount == 0 || view.mappedData.size() != a_bufferCount ||
            view.stride != sizeof(T) || view.alignment == 0 || view.elementCount == 0 ||
            view.byteSize > std::numeric_limits<std::size_t>::max())
        {
            return UploadersResult::failure({ErrorCategory::InvalidArgument,
                                             "IBufferManager.create_slot_uploaders"});
        }
        std::vector<SlotUploader<T>> uploaders;
        uploaders.reserve(a_bufferCount);
        for (auto* mapped : view.mappedData)
        {
            auto uploaderResult = SlotUploader<T>::create(view.elementCount, view.alignment,
                mapped, static_cast<std::size_t>(view.byteSize));
            if (!uploaderResult.has_value())
            {
                return UploadersResult::failure(*uploaderResult.try_error());
            }
            uploaders.push_back(uploaderResult.take_value());
        }
        return UploadersResult::success(std::move(uploaders));
    }
};
} // namespace cue
