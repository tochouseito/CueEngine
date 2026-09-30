#pragma once

#include "DX12RenderDevice.h"
#include "DX12CommandPool.h"
#include "DX12QueuePool.h"

#include <Cue/Renderer/FrameGraph/FrameGraph.h>

#include <functional>
#include <array>
#include <vector>

namespace cue::detail
{
/// @brief Graph の Pass Index ごとに Command List へ記録する処理
using GraphPassCallback = std::function<Result<void>(ID3D12GraphicsCommandList*)>;

/// @brief Platform 非依存の Barrier 計画を DX12 Command List へ記録する
class DX12GraphExecutor final
{
public:
    /// @brief Graph の順序で Barrier と Pass を記録し、Submit は呼出側に任せる
    ///
    /// Resource Pointer は Graph 実行中だけ借用する。失敗時は呼出側が Command Lease を停止する
    [[nodiscard]] static Result<void> record(const CompiledFrameGraph& a_graph,
                                             ID3D12GraphicsCommandList* a_list,
                                             const std::vector<ID3D12Resource*>& a_resources,
                                             const std::vector<GraphPassCallback>& a_callbacks);

    /// @brief Pass を指定 Queue へ個別投入し、Queue 間依存を GPU Fence で同期する
    /// @details Pool、Queue、物理 Resource は GPU 完了まで生存させる。失敗後は全 Queue 完了を確認して破棄する
    [[nodiscard]] static Result<void> execute(const CompiledFrameGraph& a_graph,
                                              DX12QueuePool& a_queues,
                                              const std::array<DX12CommandPool*, 3>& a_pools,
                                              UINT a_slot,
                                              const std::vector<ID3D12Resource*>& a_resources,
                                              const std::vector<GraphPassCallback>& a_callbacks,
                                              FrameGraphExecutionStats* a_stats = nullptr);

private:
    /// @brief Resource Handle を物理 Resource へ対応付けて Barrier を記録する
    [[nodiscard]] static Result<void> record_barrier(const GraphBarrier& a_barrier,
                                                     ID3D12GraphicsCommandList* a_list,
                                                     const std::vector<ID3D12Resource*>& a_resources);
};
} // namespace cue::detail
