#include "DX12GraphResourceBindings.h"

#include <algorithm>

namespace cue::detail
{
/// @brief Graph と同じ Index 空間の対応表を確保する
DX12GraphResourceBindings::DX12GraphResourceBindings(const FrameGraphBuilder& a_builder)
    : m_graphId(a_builder.graph_id()), m_resources(a_builder.resource_count(), nullptr)
{
}

/// @brief 別 Graph の Handle と物理 Resource の重複登録を拒否する
Result<void> DX12GraphResourceBindings::bind(GraphResourceHandle a_handle, ID3D12Resource* a_resource)
{
    if (a_handle.graphId != m_graphId || a_handle.index >= m_resources.size() ||
        !a_resource || m_resources[a_handle.index] ||
        std::ranges::find(m_resources, a_resource) != m_resources.end())
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument,
                                      "DX12GraphResourceBindings.bind"});
    }
    m_resources[a_handle.index] = a_resource;
    return Result<void>::success();
}

/// @brief 未解決 Resource を含む Graph を GPU へ投入しない
Result<std::vector<ID3D12Resource*>> DX12GraphResourceBindings::resolve() const
{
    if (std::ranges::find(m_resources, nullptr) != m_resources.end())
    {
        return Result<std::vector<ID3D12Resource*>>::failure({ErrorCategory::InvalidState,
                                                                "DX12GraphResourceBindings.resolve"});
    }
    return Result<std::vector<ID3D12Resource*>>::success(m_resources);
}
} // namespace cue::detail
