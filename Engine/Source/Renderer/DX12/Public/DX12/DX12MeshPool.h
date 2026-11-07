#pragma once

#include <memory>

#include <RHI/MeshPool.h>

namespace cue::dx12
{
class DX12RenderDevice;

/// @brief 属性別 Committed Buffer を Page 単位で所有し、Copy Queue で静的 Mesh を転送する
/// 生成元 Device / CommandPool / QueuePool は shutdown まで呼出側が生存させる
/// 空き領域があっても CPU / GPU 読取り中の Page は更新せず、別 Page を使う
class DX12MeshPool final : public IMeshPool
{
    struct State;
    struct Lease;
    struct CreateToken final
    {
    };

  public:
    /// @brief Factory 内でのみ空の Pool を構築する
    explicit DX12MeshPool(CreateToken) noexcept;
    /// @brief Page 容量を検証し、まだ GPU Buffer を確保しない Pool を作る
    [[nodiscard]] static Result<std::unique_ptr<DX12MeshPool>> create(DX12RenderDevice &a_device,
                                                                      ICommandPool &a_commands, IQueuePool &a_queues,
                                                                      const MeshPoolConfig &a_config = {});
    /// @brief 停止を試み、残る Lease に GPU 完了までの所有を引き継ぐ
    ~DX12MeshPool() override;
    /// @brief 不変 Geometry を転送し、完了確認前の Handle を返す
    [[nodiscard]] Result<MeshHandle> upload(const MeshData &a_mesh) override;
    /// @brief Fence による公開可否を確認する
    [[nodiscard]] Result<MeshStatus> status(MeshHandle a_handle) override;
    /// @brief 指定 Mesh と同 Page の転送完了を明示的に待つ
    [[nodiscard]] Result<void> wait_ready(MeshHandle a_handle) override;
    /// @brief Ready の Mesh と Page を読み取り借用する
    [[nodiscard]] Result<meshLease> acquire(MeshHandle a_handle) override;
    /// @brief 新規の Handle 利用を止める
    [[nodiscard]] Result<void> retire(MeshHandle a_handle) override;
    /// @brief 完了済み転送と退役 Mesh の領域を回収する
    [[nodiscard]] Result<std::size_t> collect() override;
    /// @brief 入口を閉じ、借用がなければ全 GPU 参照を待って解放する
    [[nodiscard]] Result<void> shutdown() override;

  private:
    std::shared_ptr<State> m_state;
};
} // namespace cue::dx12
