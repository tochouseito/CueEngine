#include "DX12TextureManager.h"

#include "DX12ResourcePool.h"

#include <algorithm>
#include <utility>

namespace cue::detail
{
/// @brief Pool の所有権を移さずに Texture 操作だけを公開する
Result<std::unique_ptr<DX12TextureManager>> DX12TextureManager::create(DX12ResourcePool& a_resources)
{
    return Result<std::unique_ptr<DX12TextureManager>>::success(std::make_unique<DX12TextureManager>(a_resources));
}

/// @brief Pool の寿命内でのみ借用する
DX12TextureManager::DX12TextureManager(DX12ResourcePool& a_resources) noexcept : m_resources(&a_resources)
{
}

/// @brief Manager が自動作成した SRV を Resource Pool より先に返す
DX12TextureManager::~DX12TextureManager()
{
    for (const auto& [index, cached] : m_textureViews)
    {
        [[maybe_unused]] auto result = m_resources->destroy_view(cached.view);
    }
}

/// @brief 生成後の Handle は Pool が所有する
Result<GpuResourceHandle> DX12TextureManager::create_texture(GpuTextureDesc a_desc)
{
    // 名前付き Resource は Manager 内で一意にし、失敗時は Registry を変えない
    if (!a_desc.name.empty() && m_namedTextures.contains(a_desc.name))
    {
        return Result<GpuResourceHandle>::failure({ErrorCategory::InvalidArgument,
                                                    "DX12TextureManager.create_texture.name"});
    }
    const std::string name = a_desc.name;
    auto result = m_resources->create_texture(std::move(a_desc));
    if (result.has_value() && !name.empty())
    {
        m_namedTextures.emplace(name, *result.try_value());
    }
    return result;
}

/// @brief 登録名の一意性を維持しながら初期 Data を同期転送する
Result<GpuResourceHandle> DX12TextureManager::create_texture(
    GpuTextureDesc a_desc, std::span<const GpuTextureSubresourceData> a_initialData)
{
    if (!a_desc.name.empty() && m_namedTextures.contains(a_desc.name))
    {
        return Result<GpuResourceHandle>::failure({ErrorCategory::InvalidArgument,
                                                    "DX12TextureManager.create_texture.name"});
    }
    const std::string name = a_desc.name;
    auto result = m_resources->create_texture(std::move(a_desc), a_initialData);
    if (result.has_value() && !name.empty())
    {
        m_namedTextures.emplace(name, *result.try_value());
    }
    return result;
}

/// @brief Pool の世代付き Handle 検証に従って Descriptor を返す
Result<GpuTextureDesc> DX12TextureManager::get_texture_desc(GpuResourceHandle a_texture) const
{
    return m_resources->texture_desc(a_texture);
}

/// @brief Shader Heap の Slot を Texture の寿命内に固定する
Result<std::uint32_t> DX12TextureManager::get_texture_descriptor_index(GpuResourceHandle a_texture)
{
    using IndexResult = Result<std::uint32_t>;
    auto kind = m_resources->is_texture(a_texture);
    if (!kind.has_value() || !kind.take_value())
    {
        return IndexResult::failure({ErrorCategory::InvalidArgument,
                                      "DX12TextureManager.get_texture_descriptor_index"});
    }
    const auto cached = m_textureViews.find(a_texture.index);
    if (cached != m_textureViews.end() && cached->second.resourceGeneration == a_texture.generation)
    {
        return IndexResult::success(cached->second.view.index);
    }
    auto view = m_resources->create_view(a_texture, GpuViewKind::ShaderResource);
    if (!view.has_value())
    {
        return IndexResult::failure(*view.try_error());
    }
    const auto handle = view.take_value();
    m_textureViews[a_texture.index] = {a_texture.generation, handle};
    return IndexResult::success(handle.index);
}

/// @brief 旧 FrameGraph が共有 Texture を名前で参照できるようにする
Result<GpuResourceHandle> DX12TextureManager::get_texture(std::string_view a_name) const
{
    const auto it = m_namedTextures.find(std::string(a_name));
    if (it == m_namedTextures.end())
    {
        return Result<GpuResourceHandle>::failure({ErrorCategory::InvalidArgument,
                                                    "DX12TextureManager.get_texture"});
    }
    return Result<GpuResourceHandle>::success(it->second);
}

/// @brief Active View があれば Pool が失敗を返す
Result<void> DX12TextureManager::destroy_texture(GpuResourceHandle a_texture)
{
    auto kindResult = m_resources->is_texture(a_texture);
    if (!kindResult.has_value())
    {
        return Result<void>::failure(*kindResult.try_error());
    }
    if (!kindResult.take_value())
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "DX12TextureManager.destroy_texture"});
    }
    const auto cached = m_textureViews.find(a_texture.index);
    if (cached != m_textureViews.end() && cached->second.resourceGeneration == a_texture.generation)
    {
        auto viewResult = m_resources->destroy_view(cached->second.view);
        if (!viewResult.has_value())
        {
            return viewResult;
        }
        m_textureViews.erase(cached);
    }
    auto result = m_resources->destroy(a_texture);
    if (result.has_value())
    {
        // View が残り破棄に失敗した場合は検索可能な名前を維持する
        const auto it = std::find_if(m_namedTextures.begin(), m_namedTextures.end(),
                                     [a_texture](const auto& a_entry) {
                                         const auto& handle = a_entry.second;
                                         return handle.owner == a_texture.owner && handle.index == a_texture.index &&
                                                handle.generation == a_texture.generation;
                                     });
        if (it != m_namedTextures.end())
        {
            m_namedTextures.erase(it);
        }
    }
    return result;
}
} // namespace cue::detail
