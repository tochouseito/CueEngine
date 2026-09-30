#pragma once

#include <Cue/Renderer/RHI/TextureManager.h>

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

namespace cue::detail
{
class DX12ResourcePool;

/// @brief Texture 操作を DX12 Resource Pool へ接続する
class DX12TextureManager final : public ITextureManager
{
public:
    ~DX12TextureManager() override;
    /// @brief Pool が Manager より長く生存する条件で借用する
    [[nodiscard]] static Result<std::unique_ptr<DX12TextureManager>> create(DX12ResourcePool& a_resources);

    /// @brief Pool へ Texture 作成を委譲する
    [[nodiscard]] Result<GpuResourceHandle> create_texture(GpuTextureDesc a_desc) override;

    /// @brief Pool の即時転送経路で初期 Data を持つ Texture を作る
    [[nodiscard]] Result<GpuResourceHandle> create_texture(
        GpuTextureDesc a_desc, std::span<const GpuTextureSubresourceData> a_initialData) override;

    /// @brief Texture の現行 Descriptor を Pool から取得する
    [[nodiscard]] Result<GpuTextureDesc> get_texture_desc(GpuResourceHandle a_texture) const override;

    /// @brief Texture ごとの SRV を遅延作成して Descriptor Index を返す
    [[nodiscard]] Result<std::uint32_t> get_texture_descriptor_index(GpuResourceHandle a_texture) override;

    /// @brief Manager が登録した名前から現行 Texture を探す
    [[nodiscard]] Result<GpuResourceHandle> get_texture(std::string_view a_name) const override;

    /// @brief Pool へ Texture 破棄を委譲する
    [[nodiscard]] Result<void> destroy_texture(GpuResourceHandle a_texture) override;

    /// @brief static create が設定した Pool の借用を保持する
    explicit DX12TextureManager(DX12ResourcePool& a_resources) noexcept;

private:
    struct CachedView final
    {
        std::uint64_t resourceGeneration = 0;
        GpuViewHandle view;
    };

    DX12ResourcePool* m_resources = nullptr;
    std::unordered_map<std::string, GpuResourceHandle> m_namedTextures;
    std::unordered_map<std::uint32_t, CachedView> m_textureViews;
};
} // namespace cue::detail
