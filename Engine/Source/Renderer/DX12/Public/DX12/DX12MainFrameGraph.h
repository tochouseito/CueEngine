#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include <DX12/DX12Contexts.h>
#include <DX12/DX12FrameGraphExecutor.h>
#include <Foundation/Result.h>
#include <Passes/MainFrameGraph.h>
#include <RHI/GpuResourcePool.h>

namespace cue
{
class ICommandCompletion;
}

namespace cue::dx12
{
class DX12DescriptorAllocator;
class DX12FrameGraphFrames;
class DX12PipelineManager;
class DX12GpuCommandContext;
class DX12RenderDevice;
class DX12SwapChain;

/// @brief 本番 Graph の描画枠、初期色、追加描画と Host 選択の表示 Pass の設定
///
/// configure / displayPassFactory と生成 Pass の setup は構築の呼出 Thread、旧 Pass の破棄は Resize の呼出 Thread
/// で行われる Callback は Graph ごとに新しい Pass と Handle を構築する。WindowsHost の Resize 呼出元は Owner Main
/// Thread
struct DX12MainFrameGraphConfig final
{
    std::uint32_t frameCount = 2;
    std::array<float, 4> clearColor{0.0f, 0.0f, 0.0f, 1.0f};
    frameGraphConfigure configure;
    // Graph へ一意所有を移し、未指定時は標準の表示 Pass を使う
    std::unique_ptr<FrameGraphPass> displayPass;
    // サイズ変更後の Graph に同じ実体を再利用せず、新しい表示 Pass を生成する
    std::function<std::unique_ptr<FrameGraphPass>()> displayPassFactory;
};

/// @brief 旧 FrameGraphPass 契約で本番描画 Graph を構築・記録する
///
/// SwapChain と ViewManager を借用する。Present は呼出側が行う
class DX12MainFrameGraph final : public IFrameGraphRecorder
{
    struct CreateToken final
    {
    };

  public:
    /// @brief create 内部でのみ未生成状態を構築する
    explicit DX12MainFrameGraph(CreateToken) noexcept;

    /// @brief Context の参照先を借用し、SwapChain と同じ形状の Graph を用意する
    ///
    /// 参照先と SwapChain は shutdown より長く生存させる。途中失敗は部分生成物を回収する
    /// configure / displayPassFactory は初回の呼出 Thread で実行し、Resize 時はその呼出 Thread で再実行する
    [[nodiscard]] static Result<std::unique_ptr<DX12MainFrameGraph>> create(const DX12ResourceContext &a_resources,
                                                                            DX12SwapChain &a_swapChain,
                                                                            DX12MainFrameGraphConfig a_config = {});

    /// @brief GPU 完了後に資源を解放する
    ~DX12MainFrameGraph() override;

    DX12MainFrameGraph(const DX12MainFrameGraph &) = delete;
    DX12MainFrameGraph &operator=(const DX12MainFrameGraph &) = delete;

    /// @brief Pass を依存順に実行し、終了 State まで記録する
    ///
    /// 手動提出時は mark_submitted、未提出時は discard_unsubmitted を必ず呼ぶ
    [[nodiscard]] Result<void> record(std::uint32_t a_frameIndex, DX12GpuCommandContext &a_context);

    /// @brief 基底 Command を DX12 Context に検証して記録する
    [[nodiscard]] Result<void> record(std::uint32_t a_frameIndex, ICommandContext &a_context) override;

    /// @brief Context の Pool を使い、Queue 間依存を満たして Graph を提出する
    ///
    /// Context の参照先は実行終了まで生存させる。呼出しは直列化する
    [[nodiscard]] Result<bool> execute(std::uint32_t a_frameIndex, const DX12ExecutionContext &a_execution,
                                       std::function<bool()> a_shouldCancel = {});

    /// @brief 提出の GPU 完了点を枠へ登録する
    [[nodiscard]] Result<void> mark_submitted(std::uint32_t a_frameIndex,
                                              std::shared_ptr<ICommandCompletion> a_completion) override;

    /// @brief 提出しなかった Graph 記録の Pool Lease を返す
    void discard_unsubmitted(std::uint32_t a_frameIndex) noexcept override;

    /// @brief 構築済み Plan を本体の生存中だけ返す
    [[nodiscard]] const FrameGraphPlan &plan() const noexcept;

    /// @brief Render が更新した CPU 集計と GPU 完了済み Timestamp を任意 Thread へ複写する
    [[nodiscard]] MainFrameGraphPerformance performance() const override;

    /// @brief 枠の GPU 完了後に物理 Resource を破棄する
    [[nodiscard]] Result<void> shutdown();

    /// @brief GPU 完了後に旧 Graph と BackBuffer を解放し、指定寸法の Graph を再生成する
    ///
    /// 呼出側は新しい Render 投入を止め、全 execute の完了を待ってから直列に呼ぶ
    /// WindowsHost からは Owner Main Thread で呼ぶ。0 寸法、未提出枠、再生成不能な一回限りの Pass は変更前に拒否する
    /// SwapChain の変更後に Graph 再生成が失敗した場合は停止状態を保ち、同じ Backend で再試行できる
    /// 保持した configure / displayPassFactory、新 Pass の setup、旧 Pass の破棄は呼出 Thread で実行する
    [[nodiscard]] Result<void> resize(const DX12ResourceContext &a_resources, std::uint32_t a_width,
                                      std::uint32_t a_height);

  private:
    /// @brief 全 Pass が Graphics の場合は単一 Queue に記録して提出する
    [[nodiscard]] Result<bool> execute_graphics(std::uint32_t a_frameIndex, ICommandPool &a_commandPool,
                                                std::function<bool()> a_shouldCancel);

    /// @brief 枠の Pool Lease と外部 Binding を初回 Pass より前に保持する
    [[nodiscard]] Result<void> prepare_frame(std::uint32_t a_frameIndex);

    /// @brief 準備済み枠の指定 Pass と終了 Barrier を記録する
    [[nodiscard]] Result<void> record_range(std::uint32_t a_frameIndex, DX12GpuCommandContext &a_context,
                                            std::size_t a_firstPass, std::size_t a_passCount, bool a_includeFinal);

    /// @brief 未提出または GPU 完了後の枠の借用を返す
    void clear_frame(std::uint32_t a_frameIndex) noexcept;

    /// @brief Build 済み Pass を直接参照する Callback と描画枠の作業容量を構築する
    [[nodiscard]] Result<void> build_execution_cache();

    std::unique_ptr<FrameGraph> m_graph;
    FrameGraphResourceHandle m_backBuffer;
    std::unique_ptr<DX12FrameGraphFrames> m_frames;
    DX12PipelineManager *m_pipelineManager = nullptr;
    std::vector<std::vector<gpuResourceLease>> m_poolLeases;
    std::vector<std::uint32_t> m_poolResourceIndices;
    std::vector<std::vector<DX12FrameGraphExternalResource>> m_externalBindings;
    std::vector<bool> m_isPrepared;
    std::vector<DX12FrameGraphPrepared> m_prepared;
    std::vector<std::vector<dx12FrameGraphPassCallback>> m_callbacks;
    std::vector<DX12GpuCommandContext *> m_recordingContexts;
    std::vector<bool> m_initialRecorded;
    std::vector<std::chrono::nanoseconds> m_frameRecordDurations;
    std::vector<std::vector<IQueueContext *>> m_queueByPass;
    std::vector<std::vector<std::uint64_t>> m_fenceByPass;
    mutable std::mutex m_performanceMutex;
    TimingSamples m_recordTimings;
    TimingSamples m_frameWaitTimings;
    std::vector<GpuPassTiming> m_gpuPasses;
    std::uint64_t m_completedGpuFrames = 0;
    DX12SwapChain *m_swapChain = nullptr;
    frameGraphConfigure m_configure;
    std::function<std::unique_ptr<FrameGraphPass>()> m_displayPassFactory;
    std::array<float, 4> m_clearColor{};
    std::uint32_t m_frameCount = 0;
    bool m_hasOneShotDisplayPass = false;
};
} // namespace cue::dx12
