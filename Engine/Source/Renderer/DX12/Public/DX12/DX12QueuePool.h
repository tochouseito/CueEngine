#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>

#include <d3d12.h>
#include <wrl/client.h>

#include <Foundation/Result.h>
#include <RHI/Queue.h>

namespace cue::dx12
{
class DX12RenderDevice;

/// @brief 単一 DX12 Queue と、その投入完了を追跡する Fence を所有する
///
/// QueuePool の共有状態が所有し、投入と Fence 発行を同一 Queue 上で直列化する
/// 公開操作は複数 Thread から呼べる。失敗後は呼出側が作業 Resource を保持して停止する
class DX12GpuCommandQueue final : public IQueueContext
{
    struct CreateToken final
    {
    };

public:
    /// @brief create だけが部分生成状態を構築する
    explicit DX12GpuCommandQueue(CreateToken) noexcept;

    /// @brief Queue、Fence、CPU 待機 Event をすべて生成してから公開する
    [[nodiscard]] static Result<std::unique_ptr<DX12GpuCommandQueue>> create(ID3D12Device& a_device,
                                                                            QueueType a_type, std::uint32_t a_index);

    /// @brief CPU 待機 Event と COM Resource を解放する
    ~DX12GpuCommandQueue() override;

    DX12GpuCommandQueue(const DX12GpuCommandQueue&) = delete;
    DX12GpuCommandQueue& operator=(const DX12GpuCommandQueue&) = delete;

    /// @brief 作成時に固定した Queue の種類を返す
    [[nodiscard]] QueueType type() const noexcept override;

    /// @brief Queue を生成した Native Device を Queue の生存中だけ借用させる
    [[nodiscard]] ID3D12Device* device() const noexcept;

    /// @brief SwapChain 作成用の Native Queue を Lease の生存中だけ借用させる
    [[nodiscard]] ID3D12CommandQueue* command_queue() const noexcept;

    /// @brief 提出先 Queue の Lease 解放後も完了確認に使える Fence 参照を返す
    [[nodiscard]] Microsoft::WRL::ComPtr<ID3D12Fence> completion_fence() const noexcept;

    /// @brief GPU 作業の完了点を Fence に記録する
    [[nodiscard]] Result<std::uint64_t> signal() override;

    /// @brief Command List を投入し、直後の完了点を同じ操作内で発行する
    ///
    /// Signal 失敗時も Command List は投入済みの場合がある
    /// a_mayHaveExecuted には ExecuteCommandLists の呼出前後を失敗時にも返す
    [[nodiscard]] Result<std::uint64_t> submit(std::span<ID3D12CommandList* const> a_lists,
                                               bool* a_mayHaveExecuted = nullptr);

    /// @brief 指定 Fence の完了または Device Lost を待つ
    [[nodiscard]] Result<void> wait_for_fence(std::uint64_t a_fenceValue) override;

    /// @brief 指定 Fence が正常に完了したか返す
    [[nodiscard]] bool is_fence_complete(std::uint64_t a_fenceValue) const noexcept override;

    /// @brief 別 DX12 Queue の Fence をこの Queue の GPU 側で待つ
    [[nodiscard]] Result<void> wait_for_queue(IQueueContext& a_queue, std::uint64_t a_fenceValue) override;

    /// @brief GPU Timestamp の毎秒 Tick 数を返す
    [[nodiscard]] Result<std::uint64_t> get_timestamp_frequency() const override;

    /// @brief 最後に発行した Fence 値まで CPU を待機させる
    [[nodiscard]] Result<void> wait_idle();

    /// @brief 致命的な Queue 失敗後に新規 GPU 作業を拒否する状態か返す
    [[nodiscard]] bool is_poisoned() const noexcept;

    /// @brief 破棄前に GPU 完了を証明できていない作業があるか返す
    [[nodiscard]] bool has_unconfirmed_work() const noexcept;

private:
    /// @brief 投入順序を保ったまま Fence 値を発行する。m_mutex 保持中に呼ぶ
    [[nodiscard]] Result<std::uint64_t> signal_locked();

    Microsoft::WRL::ComPtr<ID3D12Device> m_device;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_queue;
    Microsoft::WRL::ComPtr<ID3D12Fence> m_fence;
    void* m_fenceEvent = nullptr;
    std::atomic<std::uint64_t> m_fenceValue = 0;
    mutable std::mutex m_mutex;
    std::mutex m_waitMutex;
    bool m_isPoisoned = false;
    bool m_hasUnknownSubmission = false;
    bool m_hasUnfencedWait = false;
    QueueType m_type = QueueType::Graphics;
};

/// @brief Graphics 1、Compute 4、Copy 4 個の Queue を所有して貸し出す
///
/// Lease が残る間は共有状態と Native Device の所有権を維持する
class DX12QueuePool final : public IQueuePool
{
    struct CreateToken final
    {
    };

public:
    /// @brief create だけが部分生成状態を構築する
    explicit DX12QueuePool(CreateToken) noexcept;

    /// @brief 全 Queue の生成に成功した場合だけ Pool を公開する
    ///
    /// a_gpuLifetime は最後の Queue Lease が GPU 完了を確認するまで共有所有する
    [[nodiscard]] static Result<std::unique_ptr<DX12QueuePool>> create(
        DX12RenderDevice& a_device, std::shared_ptr<const void> a_gpuLifetime = {});

    /// @brief 新規貸出を止め、残る Lease に Queue の寿命を委ねる
    ~DX12QueuePool() override;

    DX12QueuePool(const DX12QueuePool&) = delete;
    DX12QueuePool& operator=(const DX12QueuePool&) = delete;

    /// @brief 指定種類の空き Queue を借用する
    [[nodiscard]] Result<queueLease> acquire(QueueType a_type) override;

    /// @brief 全 Queue の発行済み GPU 作業を待機する
    [[nodiscard]] Result<void> wait_idle() override;

    /// @brief 貸出を止め、貸出がなければ全 Queue の GPU 完了を待つ
    [[nodiscard]] Result<void> shutdown() override;

private:
    static constexpr std::size_t k_graphicsCount = 1;
    static constexpr std::size_t k_computeCount = 4;
    static constexpr std::size_t k_copyCount = 4;
    static constexpr std::size_t k_queueCount = k_graphicsCount + k_computeCount + k_copyCount;

    struct State;
    std::shared_ptr<State> m_state;
};
} // namespace cue::dx12
