#pragma once

#include "DX12RenderDevice.h"

#include <Cue/Renderer/FrameGraph/FrameGraph.h>

#include <vector>

namespace cue::detail
{
/// @brief 一つの FrameGraph の論理 Handle と借用 DX12 Resource を対応付ける
/// @details Resource Owner は Graph 実行と GPU 完了まで生存させる。Binding 自体は Resource を所有しない
class DX12GraphResourceBindings final
{
public:
    /// @brief 対象 Graph の Resource 宣言数に合わせて空の対応表を作る
    explicit DX12GraphResourceBindings(const FrameGraphBuilder& a_builder);

    /// @brief 同じ Graph の未登録 Handle に重複しない物理 Resource を設定する
    [[nodiscard]] Result<void> bind(GraphResourceHandle a_handle, ID3D12Resource* a_resource);

    /// @brief 全 Resource が設定済みなら宣言順の借用 Pointer 配列を返す
    [[nodiscard]] Result<std::vector<ID3D12Resource*>> resolve() const;

private:
    std::uint64_t m_graphId = 0;
    std::vector<ID3D12Resource*> m_resources;
};
} // namespace cue::detail
