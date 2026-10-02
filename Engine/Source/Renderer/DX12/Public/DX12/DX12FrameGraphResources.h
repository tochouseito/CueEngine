#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include <d3d12.h>

#include <Foundation/Result.h>
#include <FrameGraph/FrameGraphBuilder.h>
#include <RHI/Command.h>
#include <RHI/GpuResourcePool.h>

namespace cue::dx12
{
class DX12GpuResource;
class DX12GpuResourcePool;
class DX12RenderDevice;

/// @brief FrameGraph の一時 Resource と初回使用前の Aliasing Barrier を所有する
///
/// 一つの Graph Plan を一つの直列 Queue で実行する前提。外部 Resource は所有しない
/// Alias Slot の Resource は専用 ResourcePool が所有し、Graph が配置順を管理する
/// Pool Lease は Graph の GPU 完了まで保持し、shutdown で待機後に返す
/// Resource と Barrier の Pointer は本体の shutdown まで有効。GPU 提出後は毎回 mark_submitted を呼ぶ
class DX12FrameGraphResources final
{
    struct CreateToken final
    {
    };

public:
    /// @brief create の内部でだけ空の所有者を構築する
    explicit DX12FrameGraphResources(CreateToken) noexcept;

    /// @brief Alias Slot ごとに一つの Placed 領域を確保して Resource を作る
    ///
    /// 途中失敗では全 Resource を解放し、部分生成物を公開しない
    [[nodiscard]] static Result<std::unique_ptr<DX12FrameGraphResources>> create(
        DX12RenderDevice& a_device, const FrameGraphPlan& a_plan);

    /// @brief 登録済み GPU 作業を待ってから Resource を解放する
    ~DX12FrameGraphResources();

    DX12FrameGraphResources(const DX12FrameGraphResources&) = delete;
    DX12FrameGraphResources& operator=(const DX12FrameGraphResources&) = delete;

    /// @brief 論理 Handle に対応する一時 Resource を shutdown まで借用する
    ///
    /// 別 Graph、外部 Resource、不正な Handle では nullptr を返す
    [[nodiscard]] DX12GpuResource* resource(FrameGraphResourceHandle a_handle) const noexcept;

    /// @brief Pass の最初の Resource 使用前に記録する Barrier 群を借用する
    [[nodiscard]] std::span<const D3D12_RESOURCE_BARRIER> barriers_before_pass(std::size_t a_passIndex) const noexcept;

    /// @brief 作成時の Plan と同じ Build 結果か確認する
    [[nodiscard]] bool matches_plan(const FrameGraphPlan& a_plan) const noexcept;

    /// @brief 同じ Queue で直列に実行した最後の GPU 提出の完了点を登録する
    ///
    /// GPU 提出直後、Resource を破棄する前に毎回呼ぶ。異なる Queue の完了点は混在させない
    /// 失敗時は所有状態を維持する
    [[nodiscard]] Result<void> mark_submitted(std::shared_ptr<ICommandCompletion> a_completion);

    /// @brief GPU 完了を待って Resource と Heap を解放する
    ///
    /// Wait 失敗時は Resource を保持し、後から再試行できる
    [[nodiscard]] Result<void> shutdown();

private:
    std::unique_ptr<DX12GpuResourcePool> m_pool;
    std::vector<DX12GpuResource*> m_poolResources;
    std::vector<gpuResourceLease> m_poolLeases;
    std::vector<std::vector<D3D12_RESOURCE_BARRIER>> m_barriers;
    std::shared_ptr<ICommandCompletion> m_completion;
    std::uint64_t m_graphId = 0;
    std::uint64_t m_planId = 0;
    bool m_isClosed = false;
};
} // namespace cue::dx12
