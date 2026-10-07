#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include <d3d12.h>
#include <wrl/client.h>

#include <DX12/DX12Contexts.h>
#include <DX12/DX12FrameGraphExecutor.h>
#include <DX12/DX12ViewManager.h>
#include <Foundation/Result.h>
#include <FrameGraph/FrameGraphBuilder.h>
#include <FrameGraph/FrameGraphPerformance.h>
#include <RHI/Command.h>

namespace cue::dx12
{
class DX12FrameGraphResources;
class DX12GpuResource;
class DX12RenderDevice;

/// @brief 枠数と借用 RTV の論理 Resource を指定する
struct DX12FrameGraphFramesConfig final
{
    std::uint32_t frameCount = 2;
    FrameGraphResourceHandle borrowedRtvResource;
};

/// @brief Graph の一時 Texture と View を描画枠ごとに所有する
///
/// Context の参照先は shutdown より長く生存させる。操作は一つの制御 Thread で直列化する
/// 枠の再利用前に begin_frame で前回 GPU 完了を待つ
class DX12FrameGraphFrames final
{
    struct CreateToken final
    {
    };

  public:
    /// @brief create の内部でだけ未生成状態を構築する
    explicit DX12FrameGraphFrames(CreateToken) noexcept;

    /// @brief 同じ Plan から枠ごとに独立した Resource と必要な View を作る
    ///
    /// 途中失敗では生成済みの枠と Descriptor を回収し、部分生成物を公開しない
    [[nodiscard]] static Result<std::unique_ptr<DX12FrameGraphFrames>> create(const DX12ResourceContext &a_resources,
                                                                              const FrameGraphPlan &a_plan,
                                                                              DX12FrameGraphFramesConfig a_config = {});

    /// @brief GPU 完了後に枠の Resource と Descriptor を回収する
    ~DX12FrameGraphFrames();

    DX12FrameGraphFrames(const DX12FrameGraphFrames &) = delete;
    DX12FrameGraphFrames &operator=(const DX12FrameGraphFrames &) = delete;

    /// @brief 次の記録前に同じ枠の前回 GPU 作業を待つ
    [[nodiscard]] Result<void> begin_frame(std::uint32_t a_frameIndex);

    /// @brief Queue ごとの Timestamp 対応を返す。Copy は Device capability に従う
    [[nodiscard]] bool timestamp_supported(QueueType a_type) const noexcept;

    /// @brief 今回の記録 Queue の Timestamp 周波数を描画枠に保存する
    void set_timestamp_frequency(std::uint32_t a_frameIndex, QueueType a_type, std::uint64_t a_frequency) noexcept;

    /// @brief Pass 本体の前後で Query を記録する。未対応 Queue では計測しない
    void begin_pass(std::uint32_t a_frameIndex, std::size_t a_passIndex, ID3D12GraphicsCommandList &a_list) noexcept;
    /// @brief 終了 Timestamp と当該二点だけの Readback を同じ Queue へ記録する
    void end_pass(std::uint32_t a_frameIndex, std::size_t a_passIndex, ID3D12GraphicsCommandList &a_list) noexcept;

    /// @brief begin_frame で GPU 完了後に取得できた計測があるか返す
    [[nodiscard]] bool has_gpu_sample(std::uint32_t a_frameIndex) const noexcept;
    /// @brief GPU 完了済みの計測結果を Owner Thread で複写する
    [[nodiscard]] std::span<const GpuPassTiming> gpu_timings(std::uint32_t a_frameIndex) const noexcept;

    /// @brief 今回借用した外部 Texture を検証し、予約済み View を記録前に設定する
    ///
    /// begin_frame 後に呼ぶ。外部 Resource と借用 RTV は Command 提出まで呼出側が維持する
    [[nodiscard]] Result<void> prepare_imported_views(std::uint32_t a_frameIndex, const FrameGraphPlan &a_plan,
                                                      std::span<const DX12FrameGraphExternalResource> a_external,
                                                      D3D12_CPU_DESCRIPTOR_HANDLE a_borrowedRtv = {});

    /// @brief 提出済み Graph の GPU 完了点を枠へ登録する
    [[nodiscard]] Result<void> mark_submitted(std::uint32_t a_frameIndex,
                                              std::shared_ptr<ICommandCompletion> a_completion);

    /// @brief 論理 Handle に対応する指定枠の一時 Resource を借用する
    [[nodiscard]] DX12GpuResource *resource(std::uint32_t a_frameIndex,
                                            FrameGraphResourceHandle a_handle) const noexcept;

    /// @brief 指定枠の Graph Resource 表を借用する
    [[nodiscard]] DX12FrameGraphResources *graph_resources(std::uint32_t a_frameIndex) const noexcept;

    /// @brief 枠と論理 Handle に対応する RTV を返す
    [[nodiscard]] Result<D3D12_CPU_DESCRIPTOR_HANDLE> rtv(std::uint32_t a_frameIndex,
                                                          FrameGraphResourceHandle a_handle) const;

    /// @brief 枠と論理 Handle に対応する Shader 可視 SRV を返す
    [[nodiscard]] Result<D3D12_GPU_DESCRIPTOR_HANDLE> srv(std::uint32_t a_frameIndex,
                                                          FrameGraphResourceHandle a_handle) const;

    /// @brief SRV を Command List に設定する Heap を借用する
    [[nodiscard]] ID3D12DescriptorHeap *srv_heap() const noexcept;

    /// @brief 描画枠の個数を返す
    [[nodiscard]] std::size_t frame_count() const noexcept;

    /// @brief 全枠の GPU 完了を待って Resource と Descriptor を解放する
    ///
    /// 待機失敗時は所有状態を維持し、再試行できる
    [[nodiscard]] Result<void> shutdown();

  private:
    struct Views final
    {
        DX12ViewHandle rtv;
        DX12ViewHandle srv;
        std::optional<D3D12_CPU_DESCRIPTOR_HANDLE> borrowedRtv;
        bool isImported = false;
        bool needsRtv = false;
        bool needsSrv = false;
        bool isPrepared = false;
    };

    struct Frame final
    {
        std::unique_ptr<DX12FrameGraphResources> graph;
        std::vector<Views> views;
        std::shared_ptr<ICommandCompletion> completion;
        std::array<Microsoft::WRL::ComPtr<ID3D12QueryHeap>, 3> queries;
        std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, 3> timestampReadbacks;
        std::array<std::uint64_t, 3> frequencies{};
        std::vector<bool> recordedPasses;
        std::vector<GpuPassTiming> timings;
        bool hasGpuSample = false;
    };

    /// @brief 同じ Graph の一時 Resource Handle か確認する
    [[nodiscard]] bool owns(std::uint32_t a_frameIndex, FrameGraphResourceHandle a_handle) const noexcept;

    std::vector<Frame> m_frames;
    DX12ViewManager *m_viewManager = nullptr;
    DX12RenderDevice *m_device = nullptr;
    FrameGraphResourceHandle m_borrowedRtvResource;
    std::uint64_t m_graphId = 0;
    bool m_isClosed = false;
    std::vector<TimingSamples> m_passTimings;
    std::array<bool, 3> m_timestampSupported{true, true, false};
};
} // namespace cue::dx12
