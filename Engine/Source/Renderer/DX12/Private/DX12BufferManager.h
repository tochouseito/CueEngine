#pragma once

#include <Cue/Renderer/RHI/BufferManager.h>

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

namespace cue::detail
{
class DX12ResourcePool;

/// @brief Buffer 操作を DX12 Resource Pool へ接続する
class DX12BufferManager final : public IBufferManager
{
public:
    /// @brief Pool が Manager より長く生存する条件で借用する
    [[nodiscard]] static Result<std::unique_ptr<DX12BufferManager>> create(DX12ResourcePool& a_resources);

    /// @brief Pool へ Buffer 作成を委譲する
    [[nodiscard]] Result<GpuResourceHandle> create_buffer(GpuBufferDesc a_desc) override;

    /// @brief Manager が登録した名前から現行 Buffer を探す
    [[nodiscard]] Result<GpuResourceHandle> get_buffer(std::string_view a_name) const override;

    /// @brief Pool へ Buffer 破棄を委譲する
    [[nodiscard]] Result<void> destroy_buffer(GpuResourceHandle a_buffer) override;

    /// @brief Pool へ Upload 書込を委譲する
    [[nodiscard]] Result<void> write_buffer(GpuResourceHandle a_buffer, std::uint64_t a_offset,
                                             const void* a_data, std::uint64_t a_size) override;

    /// @brief Pool へ Readback 読出を委譲する
    [[nodiscard]] Result<void> read_buffer(GpuResourceHandle a_buffer, std::uint64_t a_offset,
                                            void* a_data, std::uint64_t a_size) override;

    /// @brief Upload Heap の Map 済み Slice を借用する
    [[nodiscard]] Result<GpuBufferCpuView> get_upload_buffer_view(GpuResourceHandle a_buffer) override;

    /// @brief GPU 完了後の Readback Heap Slice を借用する
    [[nodiscard]] Result<GpuBufferCpuView> get_readback_buffer_view(GpuResourceHandle a_buffer) override;

    /// @brief static create が設定した Pool の借用を保持する
    explicit DX12BufferManager(DX12ResourcePool& a_resources) noexcept;

private:
    DX12ResourcePool* m_resources = nullptr;
    std::unordered_map<std::string, GpuResourceHandle> m_namedBuffers;
};
} // namespace cue::detail
