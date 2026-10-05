#pragma once

#include <cstdint>
#include <memory>
#include <optional>

#include <DX12/DX12Contexts.h>
#include <Foundation/Result.h>
#include <RHI/Backend.h>

namespace cue::dx12
{
class DX12RenderDevice;
class DX12QueuePool;
class DX12CommandPool;
class DX12GpuResourcePool;
class DX12DescriptorAllocator;
class DX12SwapChain;
class DX12ViewManager;
class DX12PipelineManager;
struct DX12SwapChainConfig;
struct DX12DescriptorHeapState;

/// @brief Backend が所有する Descriptor Heap の用途
enum class DX12DescriptorHeapRole
{
    CpuView,
    ShaderView,
    CpuSampler,
    ShaderSampler,
    Rtv,
    Dsv,
    Count,
};

/// @brief DX12 Backend が起動時に確保する Descriptor 数
struct DX12DescriptorHeapConfig final
{
    std::uint32_t cpuViewCapacity = 1024;
    std::uint32_t shaderViewCapacity = 1024;
    std::uint32_t cpuSamplerCapacity = 64;
    std::uint32_t shaderSamplerCapacity = 64;
    std::uint32_t rtvCapacity = 64;
    std::uint32_t dsvCapacity = 32;
};

/// @brief DX12 Device と後続の GPU 資源を一意所有する Backend
///
/// Device、Descriptor Heap、ViewManager、QueuePool、CommandPool、ResourcePool を所有する
/// 生成、停止、破棄は同一 Thread から直列に行う
class DX12Backend final : public IBackend
{
    struct CreateToken final
    {
    };

public:
  /// @brief create 内部でだけ未生成の所有状態を構築する
  explicit DX12Backend(CreateToken) noexcept;

  /// @brief Device、Descriptor Heap、各 Pool を生成し、成功時だけ Backend を公開する
  ///
  /// 容量 0 は拒否し、途中失敗時は生成済みの Heap と Device を解放する
  [[nodiscard]] static Result<std::unique_ptr<DX12Backend>> create(
      const DX12DescriptorHeapConfig &a_descriptorConfig = {});

  /// @brief 明示停止されていない Queue の GPU 作業も待って解放する
  ~DX12Backend() override;

  /// @brief GPU 資源の所有権を複製させない
  DX12Backend(const DX12Backend &) = delete;
  /// @brief GPU 資源の所有権を複製させない
  DX12Backend &operator=(const DX12Backend &) = delete;

  /// @brief Queue を待ってから Device を解放し、停止後の再呼出しも成功する
  [[nodiscard]] Result<void> shutdown() override;

  /// @brief 稼働中だけ Device を借用し、停止後は nullptr を返す
  [[nodiscard]] IRenderDevice *get_render_device() noexcept override;

  /// @brief 稼働中だけ QueuePool を借用させる
  [[nodiscard]] IQueuePool *get_queue_pool() noexcept override;

  /// @brief 稼働中だけ CommandPool を借用させる
  [[nodiscard]] ICommandPool *get_command_pool() noexcept override;

  /// @brief 稼働中だけ ResourcePool を借用させる
  [[nodiscard]] IGpuResourcePool *get_resource_pool() noexcept override;

  /// @brief 稼働中だけ指定用途の DescriptorAllocator を借用させる
  ///
  /// 停止後の再取得は nullptr。取得済み Pointer は同じ Backend の Queue または Command Lease が残る間だけ有効
  /// Allocator の公開操作は複数 Thread から呼べる。最後の Lease の返却後は Pointer を使わない
  [[nodiscard]] DX12DescriptorAllocator *get_descriptor_allocator(DX12DescriptorHeapRole a_role) noexcept;

  /// @brief 稼働中だけ Resource 生成用の非所有 Context を貸す
  ///
  /// 利用者は Backend の shutdown 前に停止・破棄する。取得済み Context も停止時に失効する
  [[nodiscard]] const DX12ResourceContext *get_resource_context() const noexcept;

  /// @brief 稼働中だけ GPU 実行用 Context を貸す
  ///
  /// 実行を停止してから Backend を shutdown する。停止後は nullptr を返す
  [[nodiscard]] const DX12ExecutionContext *get_execution_context() const noexcept;

  /// @brief Window に対する SwapChain を一度だけ作成して所有する
  ///
  /// Window は shutdown 完了まで呼出側が生存させる。失敗時は Backend の既存状態を維持する
  [[nodiscard]] Result<void> create_swap_chain(void *a_windowHandle, const DX12SwapChainConfig &a_config);

  /// @brief 稼働中の SwapChain を Backend の停止まで借用させる
  [[nodiscard]] DX12SwapChain *get_swap_chain() noexcept;

private:
    std::unique_ptr<DX12RenderDevice> m_device;
    std::shared_ptr<DX12DescriptorHeapState> m_descriptors;
    std::unique_ptr<DX12ViewManager> m_viewManager;
    std::unique_ptr<DX12PipelineManager> m_pipelineManager;
    std::unique_ptr<DX12QueuePool> m_queuePool;
    std::unique_ptr<DX12CommandPool> m_commandPool;
    std::unique_ptr<DX12GpuResourcePool> m_resourcePool;
    std::optional<DX12ResourceContext> m_resourceContext;
    std::optional<DX12ExecutionContext> m_executionContext;
    std::unique_ptr<DX12SwapChain> m_swapChain;
};
} // namespace cue::dx12
