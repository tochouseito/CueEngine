#pragma once

#include <Cue/Foundation/Result.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace cue
{
enum class GraphResourceLifetime
{
    Imported,
    Persistent,
    Transient
};

enum class GraphResourceState
{
    Undefined,
    Common,
    Present,
    RenderTarget,
    DepthWrite,
    ShaderResource,
    CopySource,
    CopyDest,
    UnorderedAccess
};

enum class GraphAccess
{
    Read,
    Write,
    ReadWrite
};

/// @brief Graph 構築中だけ有効な Resource の識別子
struct GraphResourceHandle final
{
    std::uint32_t index = 0;
    std::uint64_t graphId = 0;
};

/// @brief Graph 構築中だけ有効な Pass の識別子
struct GraphPassHandle final
{
    std::uint32_t index = 0;
    std::uint64_t graphId = 0;
};

/// @brief Pass が Resource に要求する状態と読書き方向
struct GraphResourceUse final
{
    GraphResourceHandle resource;
    GraphResourceState state;
    GraphAccess access;
};

enum class GraphBarrierKind
{
    Transition,
    UnorderedAccess
};

/// @brief Pass 前または Graph 終端で必要な Resource Barrier
struct GraphBarrier final
{
    GraphResourceHandle resource;
    GraphBarrierKind kind;
    GraphResourceState before;
    GraphResourceState after;
};

/// @brief 元の Pass Index と実行直前の Barrier を保持する
struct GraphPassPlan final
{
    std::uint32_t sourceIndex = 0;
    std::string name;
    std::vector<GraphBarrier> barriers;
};

/// @brief 一つの Graph の直列実行順と最終状態への遷移を所有する
struct CompiledFrameGraph final
{
    std::vector<GraphPassPlan> passes;
    std::vector<GraphBarrier> finalBarriers;
};

/// @brief Frame 単位の Resource と Pass 宣言を検証して実行計画を作る
///
/// 呼出 Thread に専有する。Handle はこの Builder でのみ使え、次 Frame へ持ち越さない
/// GPU Resource の所有と Pass の実行は Backend 側が担う
class FrameGraphBuilder final
{
public:
    /// @brief 他の Builder と Handle を混同しない識別子を割り当てる
    FrameGraphBuilder();

    FrameGraphBuilder(const FrameGraphBuilder&) = delete;
    FrameGraphBuilder& operator=(const FrameGraphBuilder&) = delete;
    FrameGraphBuilder(FrameGraphBuilder&&) = delete;
    FrameGraphBuilder& operator=(FrameGraphBuilder&&) = delete;

    /// @brief 外部 Owner の Resource を初期状態と終了状態付きで取り込む
    [[nodiscard]] Result<GraphResourceHandle> import_resource(std::string a_name,
                                                               GraphResourceState a_initial,
                                                               GraphResourceState a_final);

    /// @brief Backend が所有する永続または一時 Resource を宣言する
    [[nodiscard]] Result<GraphResourceHandle> create_resource(std::string a_name,
                                                               GraphResourceLifetime a_lifetime,
                                                               GraphResourceState a_initial,
                                                               GraphResourceState a_final);

    /// @brief Resource 使用を明示した Pass を登録する
    [[nodiscard]] Result<GraphPassHandle> add_pass(std::string a_name, std::vector<GraphResourceUse> a_uses);

    /// @brief 先行 Pass を明示し、循環は compile で拒否する
    [[nodiscard]] Result<void> add_dependency(GraphPassHandle a_before, GraphPassHandle a_after);

    /// @brief 依存順、不正使用、Barrier を確定し、失敗時は Builder を変更しない
    [[nodiscard]] Result<CompiledFrameGraph> compile() const;

private:
    struct ResourceRecord final
    {
        std::string name;
        GraphResourceLifetime lifetime;
        GraphResourceState initial;
        GraphResourceState final;
    };

    struct PassRecord final
    {
        std::string name;
        std::vector<GraphResourceUse> uses;
    };

    /// @brief Resource Handle がこの Graph の現行 Index を指すか確認する
    [[nodiscard]] bool owns(GraphResourceHandle a_handle) const noexcept;

    /// @brief Pass Handle がこの Graph の現行 Index を指すか確認する
    [[nodiscard]] bool owns(GraphPassHandle a_handle) const noexcept;

    std::vector<ResourceRecord> m_resources;
    std::vector<PassRecord> m_passes;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> m_dependencies;
    std::uint64_t m_graphId = 0;
};
} // namespace cue
