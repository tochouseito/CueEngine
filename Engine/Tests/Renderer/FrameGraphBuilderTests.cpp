#include <FrameGraph/FrameGraphBuilder.h>

#include <cstdint>

/// @brief 依存順、寿命、Graph 境界、未初期化読み取りと循環を検証する
int main()
{
    auto builderResult = cue::FrameGraphBuilder::create();
    auto foreignResult = cue::FrameGraphBuilder::create();
    if (!builderResult.has_value() || !foreignResult.has_value())
    {
        return 1;
    }
    auto builder = builderResult.take_value();
    auto foreign = foreignResult.take_value();
    if (builder->create_transient_buffer({0}).has_value() ||
        builder->create_transient_buffer({64, cue::GpuMemoryUsage::Upload}).has_value() ||
        builder->create_transient_texture2d({0, 4}).has_value() ||
        builder->import_texture2d({4, 0}, cue::FrameGraphResourceState::Common,
                                  cue::FrameGraphResourceState::Common).has_value() ||
        builder->add_pass("").has_value())
    {
        return 2;
    }

    auto bufferResult = builder->create_transient_buffer({64});
    auto textureResult = builder->create_transient_texture2d({4, 4});
    auto externalResult = builder->import_texture2d({4, 4}, cue::FrameGraphResourceState::Present,
                                                    cue::FrameGraphResourceState::Present);
    auto unusedResult = builder->create_transient_buffer({128});
    auto foreignResourceResult = foreign->create_transient_buffer({64});
    auto foreignPassResult = foreign->add_pass("Foreign");
    if (!bufferResult.has_value() || !textureResult.has_value() || !externalResult.has_value() ||
        !unusedResult.has_value() || !foreignResourceResult.has_value() || !foreignPassResult.has_value())
    {
        return 3;
    }
    const auto buffer = bufferResult.take_value();
    const auto texture = textureResult.take_value();
    const auto external = externalResult.take_value();
    const auto unused = unusedResult.take_value();
    const auto foreignResource = foreignResourceResult.take_value();
    const auto foreignPass = foreignPassResult.take_value();

    auto seedResult = builder->add_pass("Seed");
    auto drawResult = builder->add_pass("Draw");
    auto finishResult = builder->add_pass("Finish");
    if (!seedResult.has_value() || !drawResult.has_value() || !finishResult.has_value())
    {
        return 4;
    }
    const auto seed = seedResult.take_value();
    const auto draw = drawResult.take_value();
    const auto finish = finishResult.take_value();
    if (builder->use(foreignPass, buffer, cue::FrameGraphAccess::Write,
                     cue::FrameGraphResourceState::CopyDestination).has_value() ||
        builder->use(seed, foreignResource, cue::FrameGraphAccess::Write,
                     cue::FrameGraphResourceState::CopyDestination).has_value() ||
        builder->use(seed, {buffer.graphId, 999}, cue::FrameGraphAccess::Write,
                     cue::FrameGraphResourceState::CopyDestination).has_value() ||
        builder->depends_on(seed, foreignPass).has_value() || builder->depends_on(seed, seed).has_value())
    {
        return 5;
    }
    if (!builder->use(seed, buffer, cue::FrameGraphAccess::Write,
                      cue::FrameGraphResourceState::CopyDestination).has_value() ||
        builder->use(seed, buffer, cue::FrameGraphAccess::Read,
                     cue::FrameGraphResourceState::CopySource).has_value() ||
        !builder->use(draw, buffer, cue::FrameGraphAccess::Read,
                      cue::FrameGraphResourceState::CopySource).has_value() ||
        !builder->use(draw, texture, cue::FrameGraphAccess::Write,
                      cue::FrameGraphResourceState::CopyDestination).has_value() ||
        !builder->use(finish, texture, cue::FrameGraphAccess::Read,
                      cue::FrameGraphResourceState::CopySource).has_value() ||
        !builder->use(finish, external, cue::FrameGraphAccess::Write,
                      cue::FrameGraphResourceState::CopyDestination).has_value())
    {
        return 6;
    }
    auto planResult = builder->build();
    if (!planResult.has_value())
    {
        return 7;
    }
    auto plan = planResult.take_value();
    if (plan.passes().size() != 3 || plan.passes()[0].handle.index != seed.index ||
        plan.passes()[1].handle.index != draw.index || plan.passes()[2].handle.index != finish.index ||
        plan.resources()[buffer.index].firstUse != 0 || plan.resources()[buffer.index].lastUse != 1 ||
        plan.resources()[texture.index].firstUse != 1 || plan.resources()[texture.index].lastUse != 2 ||
        plan.resources()[external.index].firstUse != 2 || !plan.resources()[external.index].isImported ||
        plan.resources()[unused.index].firstUse.has_value() || plan.resources()[unused.index].lastUse.has_value())
    {
        return 8;
    }
    if (plan.passes()[1].dependencies.size() != 1 ||
        plan.passes()[1].dependencies[0].index != seed.index ||
        plan.passes()[2].dependencies.size() != 1 ||
        plan.passes()[2].dependencies[0].index != draw.index)
    {
        return 9;
    }
    if (plan.passes()[0].barriersBefore.size() != 1 ||
        plan.passes()[0].barriersBefore[0].before != cue::FrameGraphResourceState::Common ||
        plan.passes()[0].barriersBefore[0].after != cue::FrameGraphResourceState::CopyDestination ||
        plan.passes()[1].barriersBefore.size() != 2 ||
        plan.passes()[1].barriersBefore[0].resource.index != buffer.index ||
        plan.passes()[1].barriersBefore[0].before != cue::FrameGraphResourceState::CopyDestination ||
        plan.passes()[1].barriersBefore[0].after != cue::FrameGraphResourceState::CopySource ||
        plan.passes()[2].barriersBefore.size() != 2 ||
        plan.passes()[2].barriersBefore[1].before != cue::FrameGraphResourceState::Present ||
        plan.final_barriers().size() != 1 ||
        plan.final_barriers()[0].resource.index != external.index ||
        plan.final_barriers()[0].before != cue::FrameGraphResourceState::CopyDestination ||
        plan.final_barriers()[0].after != cue::FrameGraphResourceState::Present)
    {
        return 21;
    }

    // Build 後に宣言を追加しても既存 Plan は Snapshot のまま残る
    auto overwriteResult = builder->add_pass("Overwrite");
    if (!overwriteResult.has_value())
    {
        return 10;
    }
    const auto overwrite = overwriteResult.take_value();
    if (!builder->use(overwrite, buffer, cue::FrameGraphAccess::Write,
                      cue::FrameGraphResourceState::CopyDestination).has_value())
    {
        return 11;
    }
    auto extendedResult = builder->build();
    if (!extendedResult.has_value() || plan.passes().size() != 3 || plan.resources()[buffer.index].lastUse != 1)
    {
        return 12;
    }
    auto extended = extendedResult.take_value();
    if (extended.passes().size() != 4 || extended.resources()[buffer.index].lastUse != 3 ||
        extended.passes()[3].dependencies.size() != 2 ||
        extended.passes()[3].dependencies[0].index != seed.index ||
        extended.passes()[3].dependencies[1].index != draw.index)
    {
        return 13;
    }

    auto uninitializedResult = cue::FrameGraphBuilder::create();
    if (!uninitializedResult.has_value())
    {
        return 14;
    }
    auto uninitialized = uninitializedResult.take_value();
    auto uninitializedResourceResult = uninitialized->create_transient_buffer({64});
    auto readerResult = uninitialized->add_pass("ReadFirst");
    if (!uninitializedResourceResult.has_value() || !readerResult.has_value() ||
        !uninitialized->use(readerResult.take_value(), uninitializedResourceResult.take_value(),
                            cue::FrameGraphAccess::Read,
                            cue::FrameGraphResourceState::CopySource).has_value() ||
        uninitialized->build().has_value())
    {
        return 15;
    }

    // 明示した制約で宣言順と異なる整列ができ、循環は Build 時に拒否する
    auto reorderResult = cue::FrameGraphBuilder::create();
    if (!reorderResult.has_value())
    {
        return 16;
    }
    auto reorder = reorderResult.take_value();
    auto laterResult = reorder->add_pass("Later");
    auto earlierResult = reorder->add_pass("Earlier");
    if (!laterResult.has_value() || !earlierResult.has_value())
    {
        return 17;
    }
    const auto later = laterResult.take_value();
    const auto earlier = earlierResult.take_value();
    if (!reorder->depends_on(later, earlier).has_value())
    {
        return 18;
    }
    auto reorderedResult = reorder->build();
    if (!reorderedResult.has_value() || reorderedResult.try_value()->passes()[0].handle.index != earlier.index ||
        reorderedResult.try_value()->passes()[1].handle.index != later.index)
    {
        return 19;
    }
    if (!reorder->depends_on(earlier, later).has_value() || reorder->build().has_value())
    {
        return 20;
    }

    // UAV の Read 同士は同期せず、Write を挟む Access だけ Barrier を追加する
    auto uavBuilderResult = cue::FrameGraphBuilder::create();
    if (!uavBuilderResult.has_value())
    {
        return 22;
    }
    auto uavBuilder = uavBuilderResult.take_value();
    auto uavResourceResult = uavBuilder->import_buffer({64}, cue::FrameGraphResourceState::UnorderedAccess,
                                                       cue::FrameGraphResourceState::Common);
    if (!uavResourceResult.has_value() ||
        uavBuilder->import_buffer({64, cue::GpuMemoryUsage::Upload},
                                  cue::FrameGraphResourceState::Common,
                                  cue::FrameGraphResourceState::Common).has_value() ||
        uavBuilder->import_texture2d({4, 4}, cue::FrameGraphResourceState::GenericRead,
                                     cue::FrameGraphResourceState::Common).has_value())
    {
        return 23;
    }
    const auto uavResource = uavResourceResult.take_value();
    auto readAResult = uavBuilder->add_pass("ReadA");
    auto readBResult = uavBuilder->add_pass("ReadB");
    auto writeResult = uavBuilder->add_pass("Write");
    auto readCResult = uavBuilder->add_pass("ReadC");
    auto copyResult = uavBuilder->add_pass("Copy");
    if (!readAResult.has_value() || !readBResult.has_value() || !writeResult.has_value() ||
        !readCResult.has_value() || !copyResult.has_value())
    {
        return 24;
    }
    const auto readA = readAResult.take_value();
    const auto readB = readBResult.take_value();
    const auto write = writeResult.take_value();
    const auto readC = readCResult.take_value();
    const auto copy = copyResult.take_value();
    if (uavBuilder->use(readA, uavResource, cue::FrameGraphAccess::Write,
                        cue::FrameGraphResourceState::CopySource).has_value() ||
        uavBuilder->use(readA, uavResource, cue::FrameGraphAccess::Read,
                        cue::FrameGraphResourceState::RenderTarget).has_value() ||
        uavBuilder->use(readA, uavResource, cue::FrameGraphAccess::Read,
                        cue::FrameGraphResourceState::Present).has_value() ||
        !uavBuilder->use(readA, uavResource, cue::FrameGraphAccess::Read,
                         cue::FrameGraphResourceState::UnorderedAccess).has_value() ||
        !uavBuilder->use(readB, uavResource, cue::FrameGraphAccess::Read,
                         cue::FrameGraphResourceState::UnorderedAccess).has_value() ||
        !uavBuilder->use(write, uavResource, cue::FrameGraphAccess::Write,
                         cue::FrameGraphResourceState::UnorderedAccess).has_value() ||
        !uavBuilder->use(readC, uavResource, cue::FrameGraphAccess::Read,
                         cue::FrameGraphResourceState::UnorderedAccess).has_value() ||
        !uavBuilder->use(copy, uavResource, cue::FrameGraphAccess::Read,
                         cue::FrameGraphResourceState::CopySource).has_value())
    {
        return 25;
    }
    auto uavPlanResult = uavBuilder->build();
    if (!uavPlanResult.has_value())
    {
        return 26;
    }
    const auto uavPlan = uavPlanResult.take_value();
    if (!uavPlan.passes()[0].barriersBefore.empty() || !uavPlan.passes()[1].barriersBefore.empty() ||
        uavPlan.passes()[2].barriersBefore.size() != 1 ||
        uavPlan.passes()[2].barriersBefore[0].kind != cue::FrameGraphBarrierKind::UnorderedAccess ||
        uavPlan.passes()[3].barriersBefore.size() != 1 ||
        uavPlan.passes()[3].barriersBefore[0].kind != cue::FrameGraphBarrierKind::UnorderedAccess ||
        uavPlan.passes()[4].barriersBefore.size() != 1 ||
        uavPlan.passes()[4].barriersBefore[0].kind != cue::FrameGraphBarrierKind::Transition ||
        uavPlan.final_barriers().size() != 1 ||
        uavPlan.final_barriers()[0].after != cue::FrameGraphResourceState::Common)
    {
        return 27;
    }
    return 0;
}
