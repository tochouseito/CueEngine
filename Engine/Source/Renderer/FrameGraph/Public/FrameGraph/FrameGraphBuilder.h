#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <Foundation/Result.h>
#include <RHI/GpuResource.h>
#include <RHI/GpuResourcePool.h>
#include <RHI/Queue.h>

namespace cue
{
/// @brief 一つの Graph 内だけで有効な論理 Resource の非所有 Handle
struct FrameGraphResourceHandle final
{
    std::uint64_t graphId = 0;
    std::uint32_t index = 0;

    /// @brief 未設定の Handle か判定する
    [[nodiscard]] bool is_valid() const noexcept
    {
        return graphId != 0;
    }
};

/// @brief 一つの Graph 内だけで有効な Pass の非所有 Handle
struct FrameGraphPassHandle final
{
    std::uint64_t graphId = 0;
    std::uint32_t index = 0;

    /// @brief 未設定の Handle か判定する
    [[nodiscard]] bool is_valid() const noexcept
    {
        return graphId != 0;
    }
};

/// @brief Pass が論理 Resource に行う Access
enum class FrameGraphAccess
{
    Read,
    Write,
};

/// @brief Backend 型を公開せずに Resource の GPU 使用状態を宣言する
enum class FrameGraphResourceState
{
    Common,
    GenericRead,
    CopySource,
    CopyDestination,
    ShaderRead,
    UnorderedAccess,
    RenderTarget,
    DepthRead,
    DepthWrite,
    Present,
};

/// @brief 一つの Pass が Resource を使う契約
struct FrameGraphUse final
{
    FrameGraphResourceHandle resource;
    FrameGraphAccess access = FrameGraphAccess::Read;
    FrameGraphResourceState state = FrameGraphResourceState::Common;
};

/// @brief Pass 前または Graph 終了時に必要な同期の種類
enum class FrameGraphBarrierKind
{
    Transition,
    UnorderedAccess,
};

/// @brief Backend が Native Barrier へ変換する論理計画
struct FrameGraphBarrierPlan final
{
    FrameGraphBarrierKind kind = FrameGraphBarrierKind::Transition;
    FrameGraphResourceHandle resource;
    FrameGraphResourceState before = FrameGraphResourceState::Common;
    FrameGraphResourceState after = FrameGraphResourceState::Common;
};

/// @brief 実行順を確定した Pass と先行 Pass を所有する
struct FrameGraphPassPlan final
{
    FrameGraphPassHandle handle;
    std::string name;
    QueueType queue = QueueType::Graphics;
    std::vector<FrameGraphUse> uses;
    std::vector<FrameGraphPassHandle> dependencies;
    std::vector<FrameGraphBarrierPlan> barriersBefore;
    std::vector<FrameGraphBarrierPlan> barriersAfter;
};

/// @brief 実行順における論理 Resource の使用区間を所有する
struct FrameGraphResourcePlan final
{
    FrameGraphResourceHandle handle;
    std::string name;
    GpuResourceKind kind = GpuResourceKind::Buffer;
    GpuBufferDesc bufferDesc;
    GpuTexture2DDesc textureDesc;
    bool isImported = false;
    /// Pool と Resource は Plan と GPU 完了より長く生存させる。nullptr は Pool 外の Resource
    IGpuResourcePool* pool = nullptr;
    GpuResourceHandle poolHandle;
    bool restoreFinalState = false;
    FrameGraphResourceState initialState = FrameGraphResourceState::Common;
    FrameGraphResourceState finalState = FrameGraphResourceState::Common;
    std::optional<std::size_t> firstUse;
    std::optional<std::size_t> lastUse;
};

/// @brief 実行順で使用期間が重ならず、同じ物理領域を共有できる候補
struct FrameGraphAliasSlotPlan final
{
    GpuResourceKind kind = GpuResourceKind::Buffer;
    std::vector<FrameGraphResourceHandle> resources;
};

/// @brief Builder の宣言を検証した時点の実行順と Resource 寿命を所有する
///
/// Builder を変更・破棄しても Plan の値は有効。使用区間は Pass Index の両端を含む
/// Alias Slot は直列 Queue の実行順を前提とする。物理配置と Barrier は Backend が決める
class FrameGraphPlan final
{
public:
    /// @brief 同じ Build 結果を複写した Plan で共有する識別子を返す
    [[nodiscard]] std::uint64_t id() const noexcept;

    /// @brief 実行順に並んだ Pass を返す
    [[nodiscard]] const std::vector<FrameGraphPassPlan>& passes() const noexcept;

    /// @brief Handle の Index 順に並んだ Resource と寿命を返す
    [[nodiscard]] const std::vector<FrameGraphResourcePlan>& resources() const noexcept;

    /// @brief 一時 Resource の非重複寿命から作った共有候補を返す
    [[nodiscard]] const std::vector<FrameGraphAliasSlotPlan>& alias_slots() const noexcept;

    /// @brief 外部 Resource と再利用する固定 Resource を終了 State に戻す Barrier を返す
    [[nodiscard]] const std::vector<FrameGraphBarrierPlan>& final_barriers() const noexcept;

private:
    friend class FrameGraphBuilder;

    /// @brief 検証済み Pass と Resource の Snapshot だけを所有する
    FrameGraphPlan(std::vector<FrameGraphPassPlan> a_passes,
                   std::vector<FrameGraphResourcePlan> a_resources,
                   std::vector<FrameGraphAliasSlotPlan> a_aliasSlots,
                   std::vector<FrameGraphBarrierPlan> a_finalBarriers,
                   std::uint64_t a_id) noexcept;

    std::uint64_t m_id = 0;
    std::vector<FrameGraphPassPlan> m_passes;
    std::vector<FrameGraphResourcePlan> m_resources;
    std::vector<FrameGraphAliasSlotPlan> m_aliasSlots;
    std::vector<FrameGraphBarrierPlan> m_finalBarriers;
};

/// @brief Pass と論理 Resource の宣言から依存順と使用区間を構築する
///
/// Builder は呼出側が所有し、操作は同一 Thread で直列化する。失敗時は既存の宣言を維持する
/// 生成した Handle はこの Builder の Graph ID にだけ有効で、Plan は独立した Snapshot になる
class FrameGraphBuilder final
{
    struct CreateToken final
    {
    };

public:
    /// @brief create だけが Builder を構築する
    explicit FrameGraphBuilder(CreateToken) noexcept;

    /// @brief 他 Graph と混同しない ID を発行する
    [[nodiscard]] static Result<std::unique_ptr<FrameGraphBuilder>> create();

    /// @brief FinalColorTexture を固定登録した本番用 Graph Builder を作る
    ///
    /// 生成した Handle は Builder の寿命中だけ有効で、物理 Texture は Backend が枠ごとに作る
    [[nodiscard]] static Result<std::unique_ptr<FrameGraphBuilder>> create_main(GpuTexture2DDesc a_finalColor);

    /// @brief 本番用 Graph に固定した FinalColorTexture の論理 Handle を返す
    [[nodiscard]] FrameGraphResourceHandle final_color() const noexcept;

    FrameGraphBuilder(const FrameGraphBuilder&) = delete;
    FrameGraphBuilder& operator=(const FrameGraphBuilder&) = delete;

    /// @brief Default Buffer を論理的な一時 Resource として登録する
    [[nodiscard]] Result<FrameGraphResourceHandle> create_transient_buffer(GpuBufferDesc a_desc);

    /// @brief 名前を付けて一時 Buffer を登録する
    [[nodiscard]] Result<FrameGraphResourceHandle> create_transient_buffer(std::string a_name,
                                                                           GpuBufferDesc a_desc);

    /// @brief 二次元 Texture を論理的な一時 Resource として登録する
    [[nodiscard]] Result<FrameGraphResourceHandle> create_transient_texture2d(GpuTexture2DDesc a_desc);

    /// @brief 名前を付けて一時 Texture を登録する
    [[nodiscard]] Result<FrameGraphResourceHandle> create_transient_texture2d(std::string a_name,
                                                                              GpuTexture2DDesc a_desc);

    /// @brief 外部 Buffer の開始・終了 State を登録する。物理 Bind は後続段階で行う
    [[nodiscard]] Result<FrameGraphResourceHandle> import_buffer(GpuBufferDesc a_desc,
                                                                 FrameGraphResourceState a_initial,
                                                                 FrameGraphResourceState a_final);

    /// @brief 名前付きの Pool 所有 Buffer を Graph に取り込む
    [[nodiscard]] Result<FrameGraphResourceHandle> import_buffer(std::string a_name, GpuBufferDesc a_desc,
                                                                 FrameGraphResourceState a_initial,
                                                                 FrameGraphResourceState a_final);

    /// @brief Back Buffer などの外部 Texture の開始・終了 State を登録する
    [[nodiscard]] Result<FrameGraphResourceHandle> import_texture2d(GpuTexture2DDesc a_desc,
                                                                    FrameGraphResourceState a_initial,
                                                                    FrameGraphResourceState a_final);

    /// @brief 名前付きの外部 Texture を Graph に取り込む
    [[nodiscard]] Result<FrameGraphResourceHandle> import_texture2d(std::string a_name,
                                                                    GpuTexture2DDesc a_desc,
                                                                    FrameGraphResourceState a_initial,
                                                                    FrameGraphResourceState a_final);

    /// @brief Pool 所有 Buffer を世代付き Handle で取り込む。Pool と Resource は Graph より長く生存させる
    [[nodiscard]] Result<FrameGraphResourceHandle> import_pool_buffer(
        std::string a_name, IGpuResourcePool& a_pool, GpuResourceHandle a_poolHandle,
        GpuBufferDesc a_desc, FrameGraphResourceState a_initial, FrameGraphResourceState a_final);

    /// @brief Pool 所有 Texture を世代付き Handle で取り込む。Pool と Resource は Graph より長く生存させる
    [[nodiscard]] Result<FrameGraphResourceHandle> import_pool_texture2d(
        std::string a_name, IGpuResourcePool& a_pool, GpuResourceHandle a_poolHandle,
        GpuTexture2DDesc a_desc, FrameGraphResourceState a_initial, FrameGraphResourceState a_final);

    /// @brief Legacy の get_texture と同様、先に登録した名前を検索する
    [[nodiscard]] Result<FrameGraphResourceHandle> get_texture(std::string_view a_name) const;

    /// @brief Legacy の get_buffer と同様、先に登録した名前を検索する
    [[nodiscard]] Result<FrameGraphResourceHandle> get_buffer(std::string_view a_name) const;

    /// @brief 診断に使う名前を持つ Pass を追加する
    [[nodiscard]] Result<FrameGraphPassHandle> add_pass(std::string a_name,
                                                         QueueType a_queue = QueueType::Graphics);

    /// @brief Pass 内の Resource Access と必要 State を一度だけ登録する
    ///
    /// Read／Write と State の不整合、Resource 種類に合わない State は拒否する
    [[nodiscard]] Result<void> use(FrameGraphPassHandle a_pass, FrameGraphResourceHandle a_resource,
                                   FrameGraphAccess a_access, FrameGraphResourceState a_state);

    /// @brief FrameGraph が実行中の describe_resources から Resource 使用を宣言する
    [[nodiscard]] Result<void> use(FrameGraphResourceHandle a_resource, FrameGraphAccess a_access,
                                   FrameGraphResourceState a_state);

    /// @brief a_pass を a_before より後に実行する制約を登録する
    [[nodiscard]] Result<void> depends_on(FrameGraphPassHandle a_pass, FrameGraphPassHandle a_before);

    /// @brief Hazard と明示依存を検証し、安定した直列順と使用区間を返す
    ///
    /// 同じ Builder は失敗後も修正して再 Build できる。Build 自体は宣言を変更しない
    [[nodiscard]] Result<FrameGraphPlan> build() const;

    /// @brief Graph が Pass の Index 対応を確認するため現在数を返す
    [[nodiscard]] std::size_t pass_count() const noexcept;

private:
    friend class FrameGraph;

    /// @brief Pass の資源宣言範囲を開始する
    void begin_pass(FrameGraphPassHandle a_pass) noexcept;

    /// @brief Pass の資源宣言範囲を終了する
    void end_pass() noexcept;

    /// @brief Resource 宣言を追加し、この Graph にだけ有効な Handle を返す
    [[nodiscard]] Result<FrameGraphResourceHandle> add_resource(FrameGraphResourcePlan a_resource);

    /// @brief この Builder に属する Resource Handle か判定する
    [[nodiscard]] bool owns(FrameGraphResourceHandle a_handle) const noexcept;

    /// @brief この Builder に属する Pass Handle か判定する
    [[nodiscard]] bool owns(FrameGraphPassHandle a_handle) const noexcept;

    std::uint64_t m_graphId = 0;
    FrameGraphResourceHandle m_finalColor;
    std::optional<FrameGraphPassHandle> m_currentPass;
    std::vector<FrameGraphResourcePlan> m_resources;
    std::vector<FrameGraphPassPlan> m_passes;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> m_dependencies;
};
} // namespace cue
