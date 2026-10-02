#pragma once

#include <array>
#include <cstdint>

namespace cue
{
/// @brief GPU Resource の形状を識別する
enum class GpuResourceKind
{
    Buffer,
    Texture2D,
};

/// @brief Resource の配置先と CPU からのアクセス方法を指定する
enum class GpuMemoryUsage
{
    Default,
    Upload,
    Readback,
};

/// @brief Texture の画素形式を Backend に依存せず指定する
enum class GpuTextureFormat
{
    Rgba8Unorm,
    Bgra8Unorm,
    Rgba16Float,
    R32Float,
};

/// @brief Buffer の容量と Heap 用途を指定する
struct GpuBufferDesc final
{
    std::uint64_t byteSize = 0;
    GpuMemoryUsage memory = GpuMemoryUsage::Default;
};

/// @brief Default Heap に置く二次元 Texture の形状を指定する
///
/// Texture の Upload／Readback は転送用 Buffer を介して行う
struct GpuTexture2DDesc final
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint16_t mipLevels = 1;
    GpuTextureFormat format = GpuTextureFormat::Rgba8Unorm;
    bool isRenderTarget = false;
    std::array<float, 4> clearColor{0.0f, 0.0f, 0.0f, 1.0f};
};

/// @brief Backend が所有する GPU Resource の共通契約
///
/// 生成と破棄は呼出側で同期する。GPU 提出後は最終参照の Fence 完了まで所有を維持する
class IGpuResource
{
public:
    /// @brief 派生 Resource を基底 Pointer から安全に破棄する
    virtual ~IGpuResource() = default;

    IGpuResource(const IGpuResource&) = delete;
    IGpuResource& operator=(const IGpuResource&) = delete;

    /// @brief 作成時に固定した Resource の形状を返す
    [[nodiscard]] virtual GpuResourceKind kind() const noexcept = 0;

    /// @brief 作成時に固定した Heap 用途を返す
    [[nodiscard]] virtual GpuMemoryUsage memory_usage() const noexcept = 0;

protected:
    /// @brief Backend 固有の生成経路だけが基底契約を構築する
    IGpuResource() = default;
};
} // namespace cue
