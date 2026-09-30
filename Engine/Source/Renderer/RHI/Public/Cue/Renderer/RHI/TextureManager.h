#pragma once

#include <Cue/Renderer/RHI/GpuResources.h>

#include <span>
#include <string_view>

namespace cue
{
/// @brief Texture の作成と破棄を担う契約
/// @details Handle の実所有者は実装側の Resource Pool。Render Thread から直列に呼ぶ
class ITextureManager
{
public:
    virtual ~ITextureManager() = default;

    /// @brief Color または Depth の 2D Texture を生成する
    [[nodiscard]] virtual Result<GpuResourceHandle> create_texture(GpuTextureDesc a_desc) = 0;

    /// @brief 初期 Subresource を同期転送して Texture を作成する
    [[nodiscard]] virtual Result<GpuResourceHandle> create_texture(
        GpuTextureDesc a_desc, std::span<const GpuTextureSubresourceData> a_initialData) = 0;

    /// @brief 作成時に確定した Texture 記述子を返す
    [[nodiscard]] virtual Result<GpuTextureDesc> get_texture_desc(GpuResourceHandle a_texture) const = 0;

    /// @brief Manager が保持する Texture SRV の Shader Heap Index を返す
    [[nodiscard]] virtual Result<std::uint32_t> get_texture_descriptor_index(GpuResourceHandle a_texture) = 0;

    /// @brief 名前付きで作成した Texture の現行 Handle を返す
    [[nodiscard]] virtual Result<GpuResourceHandle> get_texture(std::string_view a_name) const = 0;

    /// @brief GPU 完了後に View を持たない Texture を解放する
    [[nodiscard]] virtual Result<void> destroy_texture(GpuResourceHandle a_texture) = 0;
};
} // namespace cue
