#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

#include <Assets/MeshData.h>
#include <RHI/Command.h>
#include <RHI/GpuResource.h>

namespace cue
{
/// @brief Pool と Slot の再利用を区別する非所有参照。永続 Scene には保存しない
struct MeshHandle final
{
    std::uint32_t index = 0;
    std::uint64_t generation = 0;
    std::uint64_t poolId = 0;
    /// @brief 既定の無効値か確認する。生存判定は Pool が行う
    [[nodiscard]] bool is_valid() const noexcept
    {
        return generation != 0 && poolId != 0;
    }
};

enum class MeshStatus
{
    Uploading,
    Ready,
};

/// @brief Page 全体の非所有 Buffer View。借用 Mesh Lease の寿命まで有効
struct MeshBufferView final
{
    IGpuResource *resource = nullptr;
    std::uint32_t byteSize = 0;
    std::uint32_t stride = 0;
};

/// @brief Page を一度 Bind し、Mesh ごとの範囲を Indexed Draw / ExecuteIndirect へ渡す
/// 頂点 Stream は Position / Normal / UV0 順。存在しない属性の View は空
struct MeshDrawRange final
{
    std::uint64_t pageId = 0;
    std::array<MeshBufferView, 3> vertices{};
    MeshBufferView indices;
    MeshIndexFormat indexFormat = MeshIndexFormat::Uint16;
    std::uint32_t indexCount = 0;
    std::uint32_t firstIndex = 0;
    std::int32_t baseVertex = 0;
};

/// @brief CPU Snapshot から GPU 提出まで Mesh と Page を保持する読み取り借用
/// GPU 完了前に返す場合は最後の提出を登録する。GPU 完了まで Lease 自体を保持する経路も許可する
/// 同じ Lease の操作は呼出側で直列化する
/// Buffer は COMMON で受け取り、暗黙 promotion / decay または明示的な COMMON 復帰を使う
class IMeshLease
{
  public:
    /// @brief CPU 参照を返す。提出済み GPU 参照は Pool が Fence まで維持する
    virtual ~IMeshLease() = default;
    /// @brief 借用の Pin と返却責務の複製を禁止する
    IMeshLease(const IMeshLease &) = delete;
    /// @brief 借用の Pin と返却責務の複製を禁止する
    IMeshLease &operator=(const IMeshLease &) = delete;
    /// @brief Lease の生存中だけ有効な描画範囲を返す。Resource の書込み・再配置は禁止
    [[nodiscard]] virtual const MeshDrawRange &draw_range() const noexcept = 0;
    /// @brief 一つの最終 GPU 提出完了点を登録する。同一 Timeline の最終提出か全 Queue を Join した Token が必要
    /// 失敗時は Lease を保持したまま再試行し、登録前に返却しない
    [[nodiscard]] virtual Result<void> mark_submitted(std::shared_ptr<ICommandCompletion> a_completion) = 0;

  protected:
    /// @brief Backend の借用経路だけが構築する
    IMeshLease() = default;
};
using meshLease = std::unique_ptr<IMeshLease>;

/// @brief 統合 VB / IB の Page 容量と増設上限。16bit Index でも Page の頂点数は 65536 を超えられる
struct MeshPoolConfig final
{
    std::uint32_t verticesPerPage = 65536;
    std::uint32_t indicesPerPage = 196608;
    std::uint32_t maxPages = 64;
    std::uint32_t maxMeshes = 4096;
};

/// @brief 静的 Mesh の転送・公開・世代参照・遅延破棄を所有する Backend 契約
/// 公開操作は内部で直列化する。Backend 停止前に全 Lease を返す。再入は不可
class IMeshPool
{
  public:
    /// @brief 停止し、残る Lease に Page の寿命を引き継ぐ
    virtual ~IMeshPool() = default;
    /// @brief Pool の資源所有と世代管理の複製を禁止する
    IMeshPool(const IMeshPool &) = delete;
    /// @brief Pool の資源所有と世代管理の複製を禁止する
    IMeshPool &operator=(const IMeshPool &) = delete;
    /// @brief CPU Data を転送用所有値へコピーし、未公開の Uploading Handle を返す
    /// 呼出中は CPU Data を変更しない。失敗時は既存 Mesh を変更しない
    [[nodiscard]] virtual Result<MeshHandle> upload(const MeshData &a_mesh) = 0;
    /// @brief 転送 Fence を非待機で確認する。同 Page の別 Mesh 転送中も Uploading を返す
    /// 退役・異 Pool・古い世代は失敗を返す
    [[nodiscard]] virtual Result<MeshStatus> status(MeshHandle a_handle) = 0;
    /// @brief 初回ロード等で明示的に転送完了を CPU 待機する。毎 Frame の非待機判定には status を使う
    [[nodiscard]] virtual Result<void> wait_ready(MeshHandle)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "MeshPool.wait_ready.unsupported"});
    }
    /// @brief Ready の Mesh だけを Snapshot に借用する。退役後の新規取得は禁止
    [[nodiscard]] virtual Result<meshLease> acquire(MeshHandle a_handle) = 0;
    /// @brief 即時に新規参照を止める。取得済み Lease の提出は引き続き許可する
    [[nodiscard]] virtual Result<void> retire(MeshHandle a_handle) = 0;
    /// @brief 転送・CPU 借用・全 GPU 参照が終わった退役 Mesh を回収し、その数を返す
    [[nodiscard]] virtual Result<std::size_t> collect() = 0;
    /// @brief 新規操作を止める。Lease が残れば失敗し、返却後に再試行できる
    /// 停止開始後は既存 Lease からの新規提出も禁止する
    [[nodiscard]] virtual Result<void> shutdown() = 0;

  protected:
    /// @brief Backend の生成経路だけが構築する
    IMeshPool() = default;
};
} // namespace cue
