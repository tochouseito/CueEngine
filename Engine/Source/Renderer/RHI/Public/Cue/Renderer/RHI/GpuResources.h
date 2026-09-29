#pragma once

#include <Cue/Foundation/Result.h>

#include <cstdint>

namespace cue
{
enum class GpuMemory : std::uint8_t
{
    Device,
    Upload,
    Readback
};

enum class GpuTextureFormat : std::uint8_t
{
    Rgba8Unorm,
    Depth32Float
};

enum class GpuViewKind : std::uint8_t
{
    RenderTarget,
    DepthStencil,
    ShaderResource,
    ConstantBuffer,
    UnorderedAccess
};

struct GpuBufferDesc final
{
    std::uint64_t size = 0;
    GpuMemory memory = GpuMemory::Device;
    bool allowUnorderedAccess = false;
};

struct GpuTextureDesc final
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    GpuTextureFormat format = GpuTextureFormat::Rgba8Unorm;
    bool allowUnorderedAccess = false;
};

/// @brief Buffer View は Byte 範囲と要素 Stride を指定し、Texture View は全体を対象にする
/// @details stride が 0 の SRV／UAV Buffer は 32 bit Raw View として扱う
struct GpuViewDesc final
{
    GpuViewKind kind = GpuViewKind::ShaderResource;
    std::uint64_t bufferOffset = 0;
    std::uint64_t bufferSize = 0;
    std::uint32_t structureStride = 0;
};

class IGpuResources;

/// @brief Owner の破棄または世代更新で失効する GPU 資源の識別子
struct GpuResourceHandle final
{
    std::uint32_t index = 0;
    std::uint64_t generation = 0;
    const IGpuResources* owner = nullptr;
};

/// @brief Owner の破棄または世代更新で失効する View の識別子
struct GpuViewHandle final
{
    GpuViewKind kind = GpuViewKind::RenderTarget;
    std::uint32_t index = 0;
    std::uint64_t generation = 0;
    const IGpuResources* owner = nullptr;
};

/// @brief GPU 資源と View の所有契約。Device と実行基盤より短く生存させる
/// @details Render Thread に専有し、同時呼出や再入は行わない。破棄は GPU 完了後に行い、失敗時は Handle を維持する
class IGpuResources
{
public:
    virtual ~IGpuResources() = default;

    /// @brief 指定 Memory に Buffer を生成する
    [[nodiscard]] virtual Result<GpuResourceHandle> create_buffer(GpuBufferDesc a_desc) = 0;

    /// @brief Color または Depth の 2D Texture を生成する
    [[nodiscard]] virtual Result<GpuResourceHandle> create_texture(GpuTextureDesc a_desc) = 0;

    /// @brief Upload Buffer の指定範囲へ CPU Data を書く
    [[nodiscard]] virtual Result<void> write_buffer(GpuResourceHandle a_buffer, std::uint64_t a_offset,
                                                    const void* a_data, std::uint64_t a_size) = 0;

    /// @brief GPU 完了済みの Readback Buffer から CPU Data を読む
    [[nodiscard]] virtual Result<void> read_buffer(GpuResourceHandle a_buffer, std::uint64_t a_offset,
                                                   void* a_data, std::uint64_t a_size) = 0;

    /// @brief Resource と範囲に対応する View を生成する
    [[nodiscard]] virtual Result<GpuViewHandle> create_view(GpuResourceHandle a_resource,
                                                              GpuViewDesc a_desc) = 0;

    /// @brief 全体 Texture View を簡潔に作る
    [[nodiscard]] virtual Result<GpuViewHandle> create_view(GpuResourceHandle a_resource,
                                                              GpuViewKind a_kind)
    {
        return create_view(a_resource, GpuViewDesc{a_kind});
    }

    /// @brief GPU 完了後に View を解放する
    [[nodiscard]] virtual Result<void> destroy_view(GpuViewHandle a_view) = 0;

    /// @brief GPU 完了後に View を持たない Resource を解放する
    [[nodiscard]] virtual Result<void> destroy(GpuResourceHandle a_resource) = 0;
};
} // namespace cue
