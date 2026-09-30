#pragma once

#include <Cue/Renderer/RHI/GpuResources.h>

#include <string_view>

namespace cue
{
/// @brief Resource View の作成と破棄を担う契約
/// @details Descriptor の実所有者は実装側の Allocator。GPU 完了を確認してから破棄する
class IViewManager
{
public:
    virtual ~IViewManager() = default;

    /// @brief Resource と範囲に対応する View を作る
    [[nodiscard]] virtual Result<GpuViewHandle> create_view(GpuResourceHandle a_resource,
                                                              GpuViewDesc a_desc) = 0;

    /// @brief 名前付きで作成した View の現行 Handle を返す
    [[nodiscard]] virtual Result<GpuViewHandle> get_view(std::string_view a_name) const = 0;

    /// @brief GPU 完了後に View を解放する
    [[nodiscard]] virtual Result<void> destroy_view(GpuViewHandle a_view) = 0;
};
} // namespace cue
