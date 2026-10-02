#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include <Foundation/Result.h>
#include <RHI/Command.h>
#include <RHI/GpuResource.h>

namespace cue
{
/// @brief Pool と Slot の世代を識別する非所有 Handle
struct GpuResourceHandle final
{
    std::uint32_t index = 0;
    std::uint64_t generation = 0;
    std::uint64_t poolId = 0;

    /// @brief 初期値のままの Handle か判定する
    [[nodiscard]] bool is_valid() const noexcept
    {
        return generation != 0 && poolId != 0;
    }
};

/// @brief Resource の GPU 利用中に要求する共有方法
enum class GpuResourceAccess
{
    Read,
    Write,
};

/// @brief Pool の Resource を短期間借用し、GPU 提出の完了点を登録する契約
///
/// Lease は呼出側が所有する。同じ Lease の操作は直列化し、最後の GPU 提出後に破棄する
class IGpuResourceLease
{
public:
    /// @brief 借用を返却し、登録済み GPU 作業が終わるまで Resource を Pool に残す
    virtual ~IGpuResourceLease() = default;

    IGpuResourceLease(const IGpuResourceLease&) = delete;
    IGpuResourceLease& operator=(const IGpuResourceLease&) = delete;

    /// @brief Lease の生存中だけ Resource を非所有で返す
    [[nodiscard]] virtual IGpuResource* resource() const noexcept = 0;

    /// @brief 提出済み GPU 作業の完了点を一度だけ登録する
    ///
    /// 同一提出が複数 Resource を使う場合は Completion を共有する
    /// 登録成功まで Lease を返却しない。失敗時は借用状態を維持する
    [[nodiscard]] virtual Result<void> mark_submitted(std::shared_ptr<ICommandCompletion> a_completion) = 0;

protected:
    /// @brief Backend 固有の借用経路だけが基底契約を構築する
    IGpuResourceLease() = default;
};

/// @brief Pool の共有状態を保持し、破棄時に借用を自動返却する
using gpuResourceLease = std::unique_ptr<IGpuResourceLease>;

/// @brief Resource の所有、世代付き参照、GPU 完了後の破棄を管理する契約
///
/// Pool は Backend が所有する。公開操作は直列化され、取得した Lease は Pool 停止後も Resource を維持する
class IGpuResourcePool
{
public:
    /// @brief 派生 Pool を基底 Pointer から安全に破棄する
    virtual ~IGpuResourcePool() = default;

    IGpuResourcePool(const IGpuResourcePool&) = delete;
    IGpuResourcePool& operator=(const IGpuResourcePool&) = delete;

    /// @brief Buffer を一意所有し、その世代付き Handle を返す
    [[nodiscard]] virtual Result<GpuResourceHandle> create_buffer(GpuBufferDesc a_desc) = 0;

    /// @brief Default Heap の Texture を一意所有し、その Handle を返す
    [[nodiscard]] virtual Result<GpuResourceHandle> create_texture2d(GpuTexture2DDesc a_desc) = 0;

    /// @brief FrameGraph 用 Buffer を Placed Heap に重複なしで配置する
    ///
    /// Default 用途だけを受け付ける。常駐 Buffer は create_buffer を使う
    [[nodiscard]] virtual Result<GpuResourceHandle> create_transient_buffer(GpuBufferDesc a_desc) = 0;

    /// @brief FrameGraph 用 Texture を Placed Heap に重複なしで配置する
    ///
    /// 使用期間の共有と Aliasing Barrier は後続の FrameGraph が決める
    [[nodiscard]] virtual Result<GpuResourceHandle> create_transient_texture2d(GpuTexture2DDesc a_desc) = 0;

    /// @brief 非重複の使用期間を持つ Buffer 群を同じ Placed 領域に作る
    ///
    /// 呼出側が使用順と Aliasing Barrier を管理する。失敗時は一つも公開しない
    [[nodiscard]] virtual Result<std::vector<GpuResourceHandle>> create_alias_buffers(
        std::span<const GpuBufferDesc> a_descs) = 0;

    /// @brief 非重複の使用期間を持つ Texture 群を同じ Placed 領域に作る
    ///
    /// 呼出側が使用順と Aliasing Barrier を管理する。失敗時は一つも公開しない
    [[nodiscard]] virtual Result<std::vector<GpuResourceHandle>> create_alias_texture2ds(
        std::span<const GpuTexture2DDesc> a_descs) = 0;

    /// @brief GPU の読み取り共有または書き込み排他の Lease を借りる
    ///
    /// 競合中は待たずに InvalidState を返す。CPU 記録と GPU State 遷移は呼出側が管理する
    [[nodiscard]] virtual Result<gpuResourceLease> acquire(GpuResourceHandle a_handle,
                                                            GpuResourceAccess a_access) = 0;

    /// @brief Handle を直ちに無効化し、全 Lease と GPU 利用の完了後に実体を破棄する
    [[nodiscard]] virtual Result<void> retire(GpuResourceHandle a_handle) = 0;

    /// @brief 完了済み GPU 作業と破棄予約を回収し、解放した Resource 数を返す
    [[nodiscard]] virtual Result<std::size_t> collect() = 0;

    /// @brief 新規操作を止め、借用がなければ全 GPU 作業の完了後に Resource を破棄する
    ///
    /// 借用中なら InvalidState を返し、Lease 側へ寿命を引き継ぐ。再呼出し可能
    [[nodiscard]] virtual Result<void> shutdown() = 0;

protected:
    /// @brief 具体 Pool の生成経路だけが基底契約を構築する
    IGpuResourcePool() = default;
};
} // namespace cue
