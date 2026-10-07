#include <FrameGraph/FrameGraph.h>

#include <memory>
#include <new>
#include <utility>

#include <RHI/CommandCompletion.h>

namespace cue
{
/// @brief 記録中に有効な借用情報を保持する
FrameGraphContext::FrameGraphContext(std::uint32_t a_width, std::uint32_t a_height, std::uint32_t a_frameIndex,
                                     ICommandContext &a_command) noexcept
    : m_width(a_width), m_height(a_height), m_frameIndex(a_frameIndex), m_command(&a_command)
{
}

/// @brief Graph の幅を返す
std::uint32_t FrameGraphContext::width() const noexcept
{
    return m_width;
}

/// @brief Graph の高さを返す
std::uint32_t FrameGraphContext::height() const noexcept
{
    return m_height;
}

/// @brief 描画枠の Index を返す
std::uint32_t FrameGraphContext::frame_index() const noexcept
{
    return m_frameIndex;
}

/// @brief 記録中の Command を返す
ICommandContext &FrameGraphContext::command_context() const noexcept
{
    return *m_command;
}

/// @brief 非対応の Backend では Command を変更せず失敗する
Result<void> FrameGraphContext::set_graphics_pipeline(PipelineStateHandle)
{
    return Result<void>::failure({ErrorCategory::InvalidState, "FrameGraphContext.set_graphics_pipeline.unsupported"});
}

/// @brief 非対応の Backend では Command を変更せず失敗する
Result<void> FrameGraphContext::set_compute_pipeline(PipelineStateHandle)
{
    return Result<void>::failure({ErrorCategory::InvalidState, "FrameGraphContext.set_compute_pipeline.unsupported"});
}

/// @brief 非対応の Backend では Command を変更せず失敗する
Result<void> FrameGraphContext::set_viewport_scissor(std::uint32_t, std::uint32_t)
{
    return Result<void>::failure({ErrorCategory::InvalidState, "FrameGraphContext.set_viewport_scissor.unsupported"});
}

/// @brief 非対応の Backend では Command を変更せず失敗する
Result<void> FrameGraphContext::draw_instanced(std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t)
{
    return Result<void>::failure({ErrorCategory::InvalidState, "FrameGraphContext.draw_instanced.unsupported"});
}

/// @brief 非対応の Backend では Command を変更せず失敗する
Result<void> FrameGraphContext::dispatch(std::uint32_t, std::uint32_t, std::uint32_t)
{
    return Result<void>::failure({ErrorCategory::InvalidState, "FrameGraphContext.dispatch.unsupported"});
}

/// @brief 通常は各 Frame で Pass を実行する
bool FrameGraphPass::is_enabled() const noexcept
{
    return true;
}

/// @brief 空でない Builder と寸法だけを受け付ける
Result<std::unique_ptr<FrameGraph>> FrameGraph::create(std::unique_ptr<FrameGraphBuilder> a_builder,
                                                       std::uint32_t a_width, std::uint32_t a_height)
{
    if (!a_builder || a_builder->pass_count() != 0 || a_width == 0 || a_height == 0)
    {
        return Result<std::unique_ptr<FrameGraph>>::failure({ErrorCategory::InvalidArgument, "FrameGraph.create"});
    }
    try
    {
        return Result<std::unique_ptr<FrameGraph>>::success(
            std::make_unique<FrameGraph>(CreateToken{}, std::move(a_builder), a_width, a_height));
    }
    catch (const std::bad_alloc &)
    {
        return Result<std::unique_ptr<FrameGraph>>::failure(
            {ErrorCategory::PlatformFailure, "FrameGraph.create.allocation"});
    }
}

/// @brief Builder と寸法の所有を開始する
FrameGraph::FrameGraph(CreateToken, std::unique_ptr<FrameGraphBuilder> a_builder, std::uint32_t a_width,
                       std::uint32_t a_height) noexcept
    : m_builder(std::move(a_builder)), m_width(a_width), m_height(a_height)
{
}

/// @brief Build 後の追加を拒否し、Pass を所有する
Result<void> FrameGraph::add_pass(std::unique_ptr<FrameGraphPass> a_pass)
{
    if (!a_pass || !a_pass->name() || a_pass->name()[0] == '\0' || m_buildAttempted)
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "FrameGraph.add_pass"});
    }
    try
    {
        m_passes.push_back(std::move(a_pass));
        return Result<void>::success();
    }
    catch (const std::bad_alloc &)
    {
        return Result<void>::failure({ErrorCategory::PlatformFailure, "FrameGraph.add_pass.allocation"});
    }
}

/// @brief Legacy と同じ二段階の Pass 宣言を既存の Plan Builder へ渡す
Result<void> FrameGraph::build()
{
    if (!m_builder || m_buildAttempted)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "FrameGraph.build"});
    }
    m_builder->m_isBuildingPipelines = true;
    auto result = Result<void>::success();
    try
    {
        result = build_passes();
    }
    catch (const std::bad_alloc &)
    {
        result = Result<void>::failure({ErrorCategory::PlatformFailure, "FrameGraph.build.allocation"});
    }
    m_builder->m_isBuildingPipelines = false;
    if (!result.has_value())
    {
        auto cleanup = m_builder->release_build_pipelines();
        if (!cleanup.has_value())
        {
            return cleanup;
        }
    }
    return result;
}

/// @brief 各 Pass の生成依頼と Access 宣言から一度だけ Plan を確定する
Result<void> FrameGraph::build_passes()
{
    if (m_buildAttempted || !m_builder || m_passes.empty())
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "FrameGraph.build"});
    }
    m_buildAttempted = true;
    // 無効 Pass がある場合は setup による Resource 宣言を始めずに Build を拒否する
    for (const auto &pass : m_passes)
    {
        if (!pass->is_enabled())
        {
            return Result<void>::failure({ErrorCategory::InvalidState, "FrameGraph.build.disabled_pass"});
        }
    }
    for (const auto &pass : m_passes)
    {
        auto setupResult = pass->setup(*m_builder);
        if (!setupResult.has_value())
        {
            return setupResult;
        }
        auto handleResult = m_builder->add_pass(pass->name(), pass->type());
        if (!handleResult.has_value())
        {
            return Result<void>::failure(*handleResult.try_error());
        }
        m_builder->begin_pass(handleResult.take_value());
        auto describeResult = pass->describe_resources(*m_builder);
        m_builder->end_pass();
        if (!describeResult.has_value())
        {
            return describeResult;
        }
    }
    auto planResult = m_builder->build();
    if (!planResult.has_value())
    {
        return Result<void>::failure(*planResult.try_error());
    }
    m_plan.emplace(planResult.take_value());
    return Result<void>::success();
}

/// @brief Build 後に有効状態が変わっても Command 提出前に拒否する
Result<void> FrameGraph::validate_enabled() const
{
    if (!m_plan)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "FrameGraph.validate_enabled.plan"});
    }
    for (const auto &pass : m_passes)
    {
        if (!pass->is_enabled())
        {
            return Result<void>::failure({ErrorCategory::InvalidState, "FrameGraph.validate_enabled.disabled_pass"});
        }
    }
    return Result<void>::success();
}

/// @brief Pool の Lease を提出後の完了点へ移し、Backend Owner へ共有する
Result<std::shared_ptr<ICommandCompletion>> FrameGraph::execute(std::uint32_t a_frameIndex, ICommandPool &a_commandPool,
                                                                IQueueContext &a_queue, IFrameGraphRecorder &a_recorder,
                                                                std::function<bool()> a_shouldCancel)
{
    using CompletionResult = Result<std::shared_ptr<ICommandCompletion>>;
    if (!m_plan || a_queue.type() != QueueType::Graphics)
    {
        return CompletionResult::failure({ErrorCategory::InvalidState, "FrameGraph.execute"});
    }
    auto enabledResult = validate_enabled();
    if (!enabledResult.has_value())
    {
        return CompletionResult::failure(*enabledResult.try_error());
    }
    for (const auto &pass : m_plan->passes())
    {
        if (pass.queue != a_queue.type())
        {
            return CompletionResult::failure({ErrorCategory::InvalidState, "FrameGraph.execute.queue"});
        }
    }
    auto acquireResult = a_commandPool.acquire(a_queue.type());
    if (!acquireResult.has_value())
    {
        return CompletionResult::failure(*acquireResult.try_error());
    }
    auto command = acquireResult.take_value();
    auto recordResult = a_recorder.record(a_frameIndex, *command);
    if (!recordResult.has_value())
    {
        a_recorder.discard_unsubmitted(a_frameIndex);
        return CompletionResult::failure(*recordResult.try_error());
    }
    if (a_shouldCancel && a_shouldCancel())
    {
        a_recorder.discard_unsubmitted(a_frameIndex);
        return CompletionResult::success({});
    }
    auto closeResult = command->close();
    if (!closeResult.has_value())
    {
        a_recorder.discard_unsubmitted(a_frameIndex);
        return CompletionResult::failure(*closeResult.try_error());
    }
    std::shared_ptr<SharedCommandCompletion> completion;
    try
    {
        completion = std::make_shared<SharedCommandCompletion>();
    }
    catch (const std::bad_alloc &)
    {
        a_recorder.discard_unsubmitted(a_frameIndex);
        return CompletionResult::failure({ErrorCategory::PlatformFailure, "FrameGraph.execute.allocation"});
    }
    auto submitResult = a_commandPool.submit(a_queue, *command);
    if (!submitResult.has_value())
    {
        return CompletionResult::failure(*submitResult.try_error());
    }
    completion->set(submitResult.take_value());
    auto markResult = a_recorder.mark_submitted(a_frameIndex, completion);
    if (!markResult.has_value())
    {
        auto waitResult = completion->wait();
        if (!waitResult.has_value())
        {
            return CompletionResult::failure(*waitResult.try_error());
        }
        return CompletionResult::failure(*markResult.try_error());
    }
    return CompletionResult::success(std::move(completion));
}

/// @brief Build 済みの場合だけ Plan を公開する
const FrameGraphPlan *FrameGraph::plan() const noexcept
{
    return m_plan ? &*m_plan : nullptr;
}

/// @brief Plan の識別子と Index を検証して Pass を返す
FrameGraphPass *FrameGraph::pass(FrameGraphPassHandle a_handle) const noexcept
{
    if (!m_plan || a_handle.index >= m_passes.size())
    {
        return nullptr;
    }
    for (const auto &planned : m_plan->passes())
    {
        if (planned.handle.index == a_handle.index && planned.handle.graphId == a_handle.graphId)
        {
            return m_passes[a_handle.index].get();
        }
    }
    return nullptr;
}

/// @brief Graph の幅を返す
std::uint32_t FrameGraph::width() const noexcept
{
    return m_width;
}

/// @brief Graph の高さを返す
std::uint32_t FrameGraph::height() const noexcept
{
    return m_height;
}
} // namespace cue
