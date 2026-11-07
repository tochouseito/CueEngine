#include <DX12/DX12Backend.h>

#include <cstdio>

#include <DX12/DX12FrameGraphFrames.h>
#include <DX12/DX12GpuResource.h>
#include <DX12/DX12PlacedResourceAllocator.h>
#include <DX12/DX12RenderDevice.h>
#include <DX12/DX12ViewManager.h>
#include <FrameGraph/FrameGraphBuilder.h>

#define CHECK(a_condition)                                                                                             \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(a_condition))                                                                                            \
        {                                                                                                              \
            std::fprintf(stderr, "Depth test line %d\n", __LINE__);                                                    \
            return 1;                                                                                                  \
        }                                                                                                              \
    } while (false)

/// @brief Depth Texture の入力検証、Committed / Placed 生成、DSV と Frame ごとの分離を WARP で確認する
int main()
{
    auto backendResult = cue::dx12::DX12Backend::create(cue::dx12::AdapterSelection::Warp);
    CHECK(backendResult.has_value());
    auto backend = backendResult.take_value();
    {
        auto *device = dynamic_cast<cue::dx12::DX12RenderDevice *>(backend->get_render_device());
        CHECK(device);
        auto &views = backend->get_resource_context()->get_view_manager();
        cue::GpuTexture2DDesc depth{32, 32};
        depth.format = cue::GpuTextureFormat::D32Float;
        depth.isDepthStencil = true;
        auto invalid = depth;
        invalid.isRenderTarget = true;
        CHECK(!cue::dx12::DX12GpuResource::create_texture2d(*device->device(), invalid).has_value());
        invalid = depth;
        invalid.clearDepth = 2;
        CHECK(!cue::dx12::DX12GpuResource::create_texture2d(*device->device(), invalid).has_value());
        auto committedResult =
            cue::dx12::DX12GpuResource::create_texture2d(*device->device(), depth, L"CueEngine Depth Test Committed");
        CHECK(committedResult.has_value());
        auto committed = committedResult.take_value();
        CHECK(committed->resource()->GetDesc().Format == DXGI_FORMAT_D32_FLOAT);
        auto dsv = views.create_dsv(*committed->resource());
        CHECK(dsv.has_value() && !views.create_rtv(*committed->resource()).has_value() &&
              !views.create_srv(*committed->resource()).has_value());
        CHECK(views.release(dsv.take_value()).has_value());
        auto allocatorResult = cue::dx12::DX12PlacedResourceAllocator::create(*device);
        CHECK(allocatorResult.has_value());
        auto allocator = allocatorResult.take_value();
        auto placed = allocator->create_texture2d(depth);
        CHECK(placed.has_value() && placed.try_value()->get()->is_placed());
        auto builderResult = cue::FrameGraphBuilder::create();
        CHECK(builderResult.has_value());
        auto builder = builderResult.take_value();
        auto textureResult = builder->create_transient_texture2d("Depth", depth);
        auto passResult = builder->add_pass("DepthWrite", cue::QueueType::Graphics);
        CHECK(textureResult.has_value() && passResult.has_value());
        const auto texture = textureResult.take_value();
        CHECK(!builder
                   ->use(*passResult.try_value(), texture, cue::FrameGraphAccess::Write,
                         cue::FrameGraphResourceState::RenderTarget)
                   .has_value());
        CHECK(builder
                  ->use(passResult.take_value(), texture, cue::FrameGraphAccess::Write,
                        cue::FrameGraphResourceState::DepthWrite)
                  .has_value());
        auto planResult = builder->build();
        CHECK(planResult.has_value());
        auto plan = planResult.take_value();
        auto framesResult = cue::dx12::DX12FrameGraphFrames::create(*backend->get_resource_context(), plan, {2});
        CHECK(framesResult.has_value());
        auto frames = framesResult.take_value();
        auto first = frames->dsv(0, texture);
        auto second = frames->dsv(1, texture);
        CHECK(first.has_value() && second.has_value() && first.try_value()->ptr != second.try_value()->ptr);
        CHECK(frames->resource(0, texture) != frames->resource(1, texture));
        CHECK(frames->shutdown().has_value());
    }
    CHECK(backend->shutdown().has_value());
    return 0;
}
