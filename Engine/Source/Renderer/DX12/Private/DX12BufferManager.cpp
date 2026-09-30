#include "DX12BufferManager.h"

#include "DX12ResourcePool.h"

#include <algorithm>
#include <utility>

namespace cue::detail
{
/// @brief Pool の所有権を移さずに Buffer 操作だけを公開する
Result<std::unique_ptr<DX12BufferManager>> DX12BufferManager::create(DX12ResourcePool& a_resources)
{
    return Result<std::unique_ptr<DX12BufferManager>>::success(std::make_unique<DX12BufferManager>(a_resources));
}

/// @brief Pool の寿命内でのみ借用する
DX12BufferManager::DX12BufferManager(DX12ResourcePool& a_resources) noexcept : m_resources(&a_resources)
{
}

/// @brief 生成後の Handle は Pool が所有する
Result<GpuResourceHandle> DX12BufferManager::create_buffer(GpuBufferDesc a_desc)
{
    // 名前付き Resource は Manager 内で一意にし、失敗時は Registry を変えない
    if (!a_desc.name.empty() && m_namedBuffers.contains(a_desc.name))
    {
        return Result<GpuResourceHandle>::failure({ErrorCategory::InvalidArgument,
                                                    "DX12BufferManager.create_buffer.name"});
    }
    const std::string name = a_desc.name;
    auto result = m_resources->create_buffer(std::move(a_desc));
    if (result.has_value() && !name.empty())
    {
        m_namedBuffers.emplace(name, *result.try_value());
    }
    return result;
}

/// @brief 旧 FrameGraph が共有 Resource を名前で参照できるようにする
Result<GpuResourceHandle> DX12BufferManager::get_buffer(std::string_view a_name) const
{
    const auto it = m_namedBuffers.find(std::string(a_name));
    if (it == m_namedBuffers.end())
    {
        return Result<GpuResourceHandle>::failure({ErrorCategory::InvalidArgument,
                                                    "DX12BufferManager.get_buffer"});
    }
    return Result<GpuResourceHandle>::success(it->second);
}

/// @brief Active View があれば Pool が失敗を返す
Result<void> DX12BufferManager::destroy_buffer(GpuResourceHandle a_buffer)
{
    auto kindResult = m_resources->is_texture(a_buffer);
    if (!kindResult.has_value())
    {
        return Result<void>::failure(*kindResult.try_error());
    }
    if (kindResult.take_value())
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "DX12BufferManager.destroy_buffer"});
    }
    auto result = m_resources->destroy(a_buffer);
    if (result.has_value())
    {
        // View が残り破棄に失敗した場合は検索可能な名前を維持する
        const auto it = std::find_if(m_namedBuffers.begin(), m_namedBuffers.end(),
                                     [a_buffer](const auto& a_entry) {
                                         const auto& handle = a_entry.second;
                                         return handle.owner == a_buffer.owner && handle.index == a_buffer.index &&
                                                handle.generation == a_buffer.generation;
                                     });
        if (it != m_namedBuffers.end())
        {
            m_namedBuffers.erase(it);
        }
    }
    return result;
}

/// @brief Upload Buffer だけを Pool が受け付ける
Result<void> DX12BufferManager::write_buffer(GpuResourceHandle a_buffer, std::uint64_t a_offset,
                                              const void* a_data, std::uint64_t a_size)
{
    return m_resources->write_buffer(a_buffer, a_offset, a_data, a_size);
}

/// @brief Readback Buffer の GPU 完了を Pool が確認する
Result<void> DX12BufferManager::read_buffer(GpuResourceHandle a_buffer, std::uint64_t a_offset,
                                             void* a_data, std::uint64_t a_size)
{
    return m_resources->read_buffer(a_buffer, a_offset, a_data, a_size);
}

/// @brief Pool が管理する永続 Map の Pointer は Buffer 解放で失効する
Result<GpuBufferCpuView> DX12BufferManager::get_upload_buffer_view(GpuResourceHandle a_buffer)
{
    return m_resources->buffer_cpu_view(a_buffer, GpuMemory::Upload);
}

/// @brief Pool に GPU 完了を確認させて Readback 領域を返す
Result<GpuBufferCpuView> DX12BufferManager::get_readback_buffer_view(GpuResourceHandle a_buffer)
{
    return m_resources->buffer_cpu_view(a_buffer, GpuMemory::Readback);
}
} // namespace cue::detail
