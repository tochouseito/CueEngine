#include <Cue/Renderer/FrameGraph/FrameGraph.h>

#define CHECK(a_condition) do { if (!(a_condition)) { return __LINE__; } } while (false)

/// @brief Back Buffer の遷移と Pass 順序を検証する
int main()
{
    using namespace cue;
    FrameGraphBuilder graph;
    auto backBuffer = graph.import_resource("BackBuffer", GraphResourceState::Present,
                                            GraphResourceState::Present);
    CHECK(backBuffer.has_value());
    auto transient = graph.create_resource("Offscreen", GraphResourceLifetime::Transient,
                                            GraphResourceState::Common, GraphResourceState::Common);
    CHECK(transient.has_value());
    const auto back = backBuffer.take_value();
    const auto offscreen = transient.take_value();
    auto clear = graph.add_pass("Clear", {{offscreen, GraphResourceState::RenderTarget, GraphAccess::Write}});
    auto copy = graph.add_pass("Copy", {{offscreen, GraphResourceState::CopySource, GraphAccess::Read},
                                         {back, GraphResourceState::CopyDest, GraphAccess::Write}});
    CHECK(clear.has_value() && copy.has_value());
    auto plan = graph.compile();
    CHECK(plan.has_value());
    const auto compiled = plan.take_value();
    CHECK(compiled.passes.size() == 2);
    CHECK(compiled.passes[0].name == "Clear");
    CHECK(compiled.passes[1].name == "Copy");
    CHECK(compiled.passes[0].barriers.size() == 1);
    CHECK(compiled.passes[1].barriers.size() == 2);
    CHECK(compiled.finalBarriers.size() == 2);

    FrameGraphBuilder invalid;
    CHECK(!invalid.add_pass("Foreign", {{back, GraphResourceState::RenderTarget, GraphAccess::Write}}).has_value());
    auto unproduced = invalid.create_resource("Unproduced", GraphResourceLifetime::Transient,
                                              GraphResourceState::Common, GraphResourceState::Common);
    CHECK(unproduced.has_value());
    CHECK(invalid.add_pass("Read", {{unproduced.take_value(), GraphResourceState::CopySource,
                                       GraphAccess::Read}}).has_value());
    CHECK(!invalid.compile().has_value());

    FrameGraphBuilder cyclic;
    auto resource = cyclic.import_resource("Buffer", GraphResourceState::Common, GraphResourceState::Common);
    CHECK(resource.has_value());
    const auto handle = resource.take_value();
    auto first = cyclic.add_pass("First", {{handle, GraphResourceState::CopyDest, GraphAccess::Write}});
    auto second = cyclic.add_pass("Second", {{handle, GraphResourceState::CopyDest, GraphAccess::Write}});
    CHECK(first.has_value() && second.has_value());
    CHECK(cyclic.add_dependency(second.take_value(), first.take_value()).has_value());
    CHECK(!cyclic.compile().has_value());

    FrameGraphBuilder ordered;
    auto left = ordered.import_resource("Left", GraphResourceState::Common, GraphResourceState::Common);
    auto right = ordered.import_resource("Right", GraphResourceState::Common, GraphResourceState::Common);
    CHECK(left.has_value() && right.has_value());
    const auto leftHandle = left.take_value();
    const auto rightHandle = right.take_value();
    CHECK(!ordered.add_pass("Conflict", {{leftHandle, GraphResourceState::CopyDest, GraphAccess::Write},
                                          {leftHandle, GraphResourceState::CopySource, GraphAccess::Read}}).has_value());
    auto later = ordered.add_pass("Later", {{leftHandle, GraphResourceState::CopyDest, GraphAccess::Write}});
    auto earlier = ordered.add_pass("Earlier", {{rightHandle, GraphResourceState::CopyDest, GraphAccess::Write}});
    CHECK(later.has_value() && earlier.has_value());
    CHECK(ordered.add_dependency(earlier.take_value(), later.take_value()).has_value());
    auto orderedPlan = ordered.compile();
    CHECK(orderedPlan.has_value());
    CHECK(orderedPlan.try_value()->passes[0].name == "Earlier");

    FrameGraphBuilder uav;
    auto uavResource = uav.import_resource("Uav", GraphResourceState::Common, GraphResourceState::Common);
    CHECK(uavResource.has_value());
    const auto uavHandle = uavResource.take_value();
    CHECK(uav.add_pass("Write0", {{uavHandle, GraphResourceState::UnorderedAccess,
                                   GraphAccess::Write}}).has_value());
    CHECK(uav.add_pass("Write1", {{uavHandle, GraphResourceState::UnorderedAccess,
                                   GraphAccess::Write}}).has_value());
    auto uavPlan = uav.compile();
    CHECK(uavPlan.has_value());
    CHECK(uavPlan.try_value()->passes[1].barriers[0].kind == GraphBarrierKind::UnorderedAccess);
    return 0;
}
