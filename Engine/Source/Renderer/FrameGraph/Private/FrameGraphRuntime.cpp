#include <Cue/Renderer/FrameGraph/FrameGraphRuntime.h>

#include <utility>

namespace cue
{
/// @brief Pass 登録と同じ Builder に Resource 宣言を集める
FrameGraphBuilder& FrameGraph::builder() noexcept
{
    return m_builder;
}

/// @brief setup 失敗時は Pass の所有を追加しない
Result<GraphPassHandle> FrameGraph::add_pass(std::unique_ptr<FrameGraphPass> a_pass)
{
    if (!a_pass || m_isFaulted || m_builder.pass_count() != m_passes.size())
    {
        return Result<GraphPassHandle>::failure({ErrorCategory::InvalidArgument, "FrameGraph.add_pass"});
    }
    const auto previousCount = m_builder.pass_count();
    auto result = a_pass->setup(m_builder);
    if (!result.has_value())
    {
        // setup は Resource 宣言も変更できるため、失敗後の部分状態を再利用しない
        m_isFaulted = true;
        return result;
    }
    const auto handle = result.take_value();
    if (handle.index != previousCount || m_builder.pass_count() != previousCount + 1)
    {
        m_isFaulted = true;
        return Result<GraphPassHandle>::failure({ErrorCategory::InvalidState,
                                                  "FrameGraph.add_pass.externalPass"});
    }
    m_passes.push_back(std::move(a_pass));
    return Result<GraphPassHandle>::success(handle);
}

/// @brief Builder の検証結果を GPU 実行へ渡す
Result<CompiledFrameGraph> FrameGraph::build() const
{
    if (m_isFaulted || m_builder.pass_count() != m_passes.size())
    {
        return Result<CompiledFrameGraph>::failure({ErrorCategory::InvalidState, "FrameGraph.build"});
    }
    return m_builder.compile();
}

/// @brief GPU Resource の所有は Manager に残し、Graph は Handle のみ借用する
Result<void> FrameGraph::bind_resource(GraphResourceHandle a_graphResource, GpuResourceHandle a_resource)
{
    if (!a_resource.owner)
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "FrameGraph.bind_resource"});
    }
    return bind(a_graphResource, {FrameGraphBindingKind::ManagedResource, a_resource});
}

/// @brief Surface は Frame Slot ごとに実体が変わるため種類だけを保存する
Result<void> FrameGraph::bind_surface_color(GraphResourceHandle a_graphResource)
{
    return bind(a_graphResource, {FrameGraphBindingKind::SurfaceColor, {}});
}

/// @brief Depth Surface の再生成後も同じ論理 Handle を使う
Result<void> FrameGraph::bind_surface_depth(GraphResourceHandle a_graphResource)
{
    return bind(a_graphResource, {FrameGraphBindingKind::SurfaceDepth, {}});
}

/// @brief 全 Resource の Binding 欠落を GPU Submit 前に拒否する
Result<std::vector<FrameGraphResourceBinding>> FrameGraph::resource_bindings() const
{
    using BindingsResult = Result<std::vector<FrameGraphResourceBinding>>;
    if (m_bindings.size() != m_builder.resource_count())
    {
        return BindingsResult::failure({ErrorCategory::InvalidState, "FrameGraph.resource_bindings.count"});
    }
    std::vector<FrameGraphResourceBinding> bindings;
    bindings.reserve(m_bindings.size());
    for (const auto& binding : m_bindings)
    {
        if (!binding)
        {
            return BindingsResult::failure({ErrorCategory::InvalidState,
                                            "FrameGraph.resource_bindings.missing"});
        }
        bindings.push_back(*binding);
    }
    return BindingsResult::success(std::move(bindings));
}

/// @brief 別 Graph と同じ Index の二重登録を拒否する
Result<void> FrameGraph::bind(GraphResourceHandle a_graphResource,
                              FrameGraphResourceBinding a_binding)
{
    if (a_graphResource.graphId != m_builder.graph_id() ||
        a_graphResource.index >= m_builder.resource_count())
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "FrameGraph.bind"});
    }
    m_bindings.resize(m_builder.resource_count());
    if (m_bindings[a_graphResource.index])
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "FrameGraph.bind.duplicate"});
    }
    m_bindings[a_graphResource.index] = a_binding;
    return Result<void>::success();
}

/// @brief Render Thread が更新する直近の計測値を一貫した Snapshot として返す
FrameGraphExecutionStats FrameGraph::execution_stats_copy() const
{
    std::lock_guard lock(m_statsMutex);
    return m_stats;
}

/// @brief 失敗した Frame の途中値で最後の成功値を上書きしない
void FrameGraph::update_execution_stats(FrameGraphExecutionStats a_stats)
{
    std::lock_guard lock(m_statsMutex);
    m_stats = std::move(a_stats);
}

/// @brief Callback Index と一致する構築順で Pass を公開する
const std::vector<std::unique_ptr<FrameGraphPass>>& FrameGraph::passes() const noexcept
{
    return m_passes;
}
} // namespace cue
