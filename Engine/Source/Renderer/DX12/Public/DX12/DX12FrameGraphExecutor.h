#pragma once

#include <cstdint>
#include <functional>
#include <span>

#include <d3d12.h>

#include <Foundation/Result.h>
#include <FrameGraph/FrameGraphBuilder.h>

namespace cue::dx12
{
class DX12FrameGraphResources;
class DX12GpuCommandContext;

/// @brief 呼出側が完了まで所有する外部 Resource の非所有 Binding
struct DX12FrameGraphExternalResource final
{
    FrameGraphResourceHandle handle;
    ID3D12Resource* resource = nullptr;
};

/// @brief 記録中の Pass から論理 Handle に対応する Native Resource を借用する
///
/// Callback の実行中だけ有効。同じ Context の記録は呼出側で直列化する
class DX12FrameGraphPassContext final
{
public:
    /// @brief Executor が検証した Resource 表を一時的に参照する
    DX12FrameGraphPassContext(std::uint64_t a_graphId, std::span<ID3D12Resource* const> a_resources) noexcept;

    /// @brief 別 Graph または範囲外の Handle では nullptr を返す
    [[nodiscard]] ID3D12Resource* resource(FrameGraphResourceHandle a_handle) const noexcept;

private:
    std::uint64_t m_graphId = 0;
    std::span<ID3D12Resource* const> m_resources;
};

/// @brief 実行順に対応する Pass の Native Command 記録処理
using dx12FrameGraphPassCallback = std::function<Result<void>(ID3D12GraphicsCommandList&,
                                                               const DX12FrameGraphPassContext&)>;

/// @brief 論理 Barrier 計画と Pass Callback を一つの Graphics Command List に記録する
class DX12FrameGraphExecutor final
{
public:
    /// @brief 全 Binding を検証してから Aliasing、State、Pass、終了 State の順で記録する
    ///
    /// Callback は plan.passes() の実行順に一つずつ渡す。失敗時は Command List を提出しない
    /// 外部 Resource は GPU 完了まで呼出側が所有し、提出後は一時 Resource に mark_submitted を呼ぶ
    [[nodiscard]] static Result<void> record(const FrameGraphPlan& a_plan, DX12FrameGraphResources& a_resources,
                                             std::span<const DX12FrameGraphExternalResource> a_external,
                                             std::span<const dx12FrameGraphPassCallback> a_callbacks,
                                             DX12GpuCommandContext& a_context);
};
} // namespace cue::dx12
