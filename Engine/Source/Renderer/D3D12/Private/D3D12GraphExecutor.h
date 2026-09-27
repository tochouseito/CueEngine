#pragma once

#include "D3D12DeviceContext.h"

#include <Cue/Renderer/FrameGraph/FrameGraph.h>

#include <functional>
#include <vector>

namespace cue::detail
{
/// @brief Graph の Pass Index ごとに Command List へ記録する処理
using GraphPassCallback = std::function<Result<void>(ID3D12GraphicsCommandList*)>;

/// @brief Platform 非依存の Barrier 計画を D3D12 Command List へ記録する
class D3D12GraphExecutor final
{
public:
    /// @brief Graph の順序で Barrier と Pass を記録し、Submit は呼出側に任せる
    ///
    /// Resource Pointer は Graph 実行中だけ借用する。失敗時は呼出側が Command Lease を停止する
    [[nodiscard]] static Result<void> record(const CompiledFrameGraph& a_graph,
                                             ID3D12GraphicsCommandList* a_list,
                                             const std::vector<ID3D12Resource*>& a_resources,
                                             const std::vector<GraphPassCallback>& a_callbacks);

private:
    /// @brief Resource Handle を物理 Resource へ対応付けて Barrier を記録する
    [[nodiscard]] static Result<void> record_barrier(const GraphBarrier& a_barrier,
                                                     ID3D12GraphicsCommandList* a_list,
                                                     const std::vector<ID3D12Resource*>& a_resources);
};
} // namespace cue::detail
