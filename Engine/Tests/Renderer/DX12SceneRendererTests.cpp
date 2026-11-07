#include <SceneRender/SceneRenderer.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include <d3d12sdklayers.h>

#include <Assets/BuiltinMesh.h>
#include <DX12/DX12Backend.h>
#include <DX12/DX12FrameGraphContext.h>
#include <DX12/DX12GpuResource.h>
#include <DX12/DX12MainFrameGraph.h>
#include <DX12/DX12RenderDevice.h>
#include <DX12/DX12SwapChain.h>
#include <Math/Projection.h>
#include <Platform/Windows/WindowsPlatform.h>

namespace
{
#define CHECK(...)                                                                                                     \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(__VA_ARGS__))                                                                                            \
        {                                                                                                              \
            std::fprintf(stderr, "DX12 Scene test line %d\n", __LINE__);                                               \
            return false;                                                                                              \
        }                                                                                                              \
    } while (false)

/// @brief GPU コピー先を Test が所有し、Graph Resize の完了境界でだけ再生成する
struct Capture final
{
    ID3D12Device *device = nullptr;
    std::unique_ptr<cue::dx12::DX12GpuResource> readback;
    std::unique_ptr<cue::dx12::DX12GpuResource> depthReadback;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT depthFootprint{};
    std::uint64_t byteSize = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    /// @brief 現在の FinalColor に対応する RowPitch と Readback Buffer を用意する
    cue::Result<void> prepare(std::uint32_t a_width, std::uint32_t a_height)
    {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = a_width;
        desc.Height = a_height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &byteSize);
        auto buffer = cue::dx12::DX12GpuResource::create_buffer(*device, {byteSize, cue::GpuMemoryUsage::Readback},
                                                                L"CueEngine Scene Renderer Test Capture");
        if (!buffer.has_value())
            return cue::Result<void>::failure(*buffer.try_error());
        readback = buffer.take_value();
        desc.Format = DXGI_FORMAT_D32_FLOAT;
        UINT64 depthBytes = 0;
        device->GetCopyableFootprints(&desc, 0, 1, 0, &depthFootprint, nullptr, nullptr, &depthBytes);
        auto depthBuffer = cue::dx12::DX12GpuResource::create_buffer(
            *device, {depthBytes, cue::GpuMemoryUsage::Readback}, L"CueEngine Scene Renderer Test Depth Capture");
        if (!depthBuffer.has_value())
            return cue::Result<void>::failure(*depthBuffer.try_error());
        depthReadback = depthBuffer.take_value();
        width = a_width;
        height = a_height;
        return cue::Result<void>::success();
    }

    /// @brief 最後の提出を待った後、中心の RGBA8 画素を読み取る
    cue::Result<std::array<unsigned char, 4>> center() const
    {
        return pixel(width / 2, height / 2);
    }

    /// @brief GPU 完了後に指定画素だけを読み、Resize 後の投影範囲も検証できるようにする
    cue::Result<std::array<unsigned char, 4>> pixel(std::uint32_t a_x, std::uint32_t a_y) const
    {
        std::array<std::byte, 4> bytes{};
        const auto offset = static_cast<std::size_t>(a_y) * footprint.Footprint.RowPitch + a_x * 4;
        auto result = readback->read(offset, bytes);
        if (!result.has_value())
            return cue::Result<std::array<unsigned char, 4>>::failure(*result.try_error());
        return cue::Result<std::array<unsigned char, 4>>::success(
            {std::to_integer<unsigned char>(bytes[0]), std::to_integer<unsigned char>(bytes[1]),
             std::to_integer<unsigned char>(bytes[2]), std::to_integer<unsigned char>(bytes[3])});
    }

    /// @brief Depth の中心値を読み、前面が描画されたかを Color と独立して調べる
    cue::Result<float> center_depth() const
    {
        std::array<std::byte, sizeof(float)> bytes{};
        const auto offset = static_cast<std::size_t>(height / 2) * depthFootprint.Footprint.RowPitch + (width / 2) * 4;
        auto result = depthReadback->read(offset, bytes);
        if (!result.has_value())
            return cue::Result<float>::failure(*result.try_error());
        float value = 0;
        std::memcpy(&value, bytes.data(), sizeof(value));
        return cue::Result<float>::success(value);
    }
};

/// @brief MeshPass の後に FinalColor を読み戻す。Barrier は FrameGraph の宣言から生成する
class CapturePass final : public cue::FrameGraphPass
{
  public:
    /// @brief Test Owner が GPU 完了まで維持するコピー先を借用する
    explicit CapturePass(Capture &a_capture) : m_capture(a_capture)
    {
    }
    /// @brief PIX と Graph 計画に固定の名前を公開する
    const char *name() const noexcept override
    {
        return "CaptureFinalColor";
    }
    /// @brief MeshPass と同じ Graphics Queue に Copy を記録する
    cue::QueueType type() const noexcept override
    {
        return cue::QueueType::Graphics;
    }
    /// @brief 新 Graph の FinalColor Handle と寸法を取得する
    cue::Result<void> setup(cue::FrameGraphBuilder &a_builder) override
    {
        auto color = a_builder.get_texture("FinalColorTexture");
        if (!color.has_value())
            return cue::Result<void>::failure(*color.try_error());
        m_color = color.take_value();
        auto depth = a_builder.get_texture("SceneDepth");
        if (!depth.has_value())
            return cue::Result<void>::failure(*depth.try_error());
        m_depth = depth.take_value();
        auto descriptor = a_builder.texture_desc(m_color);
        if (!descriptor.has_value())
            return cue::Result<void>::failure(*descriptor.try_error());
        return m_capture.prepare(descriptor.try_value()->width, descriptor.try_value()->height);
    }
    /// @brief CopySource への遷移を FrameGraph へ要求する
    cue::Result<void> describe_resources(cue::FrameGraphBuilder &a_builder) override
    {
        auto color = a_builder.use(m_color, cue::FrameGraphAccess::Read, cue::FrameGraphResourceState::CopySource);
        return color.has_value()
                   ? a_builder.use(m_depth, cue::FrameGraphAccess::Read, cue::FrameGraphResourceState::CopySource)
                   : std::move(color);
    }
    /// @brief 宣言済み FinalColor から Readback Buffer へ Native Copy を記録する
    cue::Result<void> execute(cue::FrameGraphContext &a_context) override
    {
        auto *native = dynamic_cast<cue::dx12::DX12FrameGraphContext *>(&a_context);
        if (!native || !native->resource(m_color) || !native->resource(m_depth))
            return cue::Result<void>::failure({cue::ErrorCategory::InvalidState, "SceneTest.capture_context"});
        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = native->resource(m_color);
        source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = m_capture.readback->resource();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        destination.PlacedFootprint = m_capture.footprint;
        native->command_list().CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        source.pResource = native->resource(m_depth);
        destination.pResource = m_capture.depthReadback->resource();
        destination.PlacedFootprint = m_capture.depthFootprint;
        native->command_list().CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        return cue::Result<void>::success();
    }

  private:
    Capture &m_capture;
    cue::FrameGraphResourceHandle m_color;
    cue::FrameGraphResourceHandle m_depth;
};

/// @brief 非同期 Validation も提出 Fence 完了後に調べ、Error を見落とさない
bool verify_debug_messages(ID3D12Device &a_device)
{
    Microsoft::WRL::ComPtr<ID3D12InfoQueue> info;
    if (FAILED(a_device.QueryInterface(IID_PPV_ARGS(&info))))
        return true;
    for (UINT64 index = 0; index < info->GetNumStoredMessages(); ++index)
    {
        SIZE_T size = 0;
        CHECK(SUCCEEDED(info->GetMessage(index, nullptr, &size)));
        std::vector<std::byte> storage(size);
        auto *message = reinterpret_cast<D3D12_MESSAGE *>(storage.data());
        CHECK(SUCCEEDED(info->GetMessage(index, message, &size)));
        if (message->Severity == D3D12_MESSAGE_SEVERITY_ERROR || message->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION)
        {
            std::fprintf(stderr, "%s\n", message->pDescription);
            return false;
        }
    }
    return true;
}

/// @brief MeshPool の Cube を ExecuteIndirect で描き、位置・空 Frame・Depth・Resize の実画素を確認する
bool verify_scene_renderer()
{
    auto systemResult = cue::create_windows_window_system();
    CHECK(systemResult.has_value());
    auto system = systemResult.take_value();
    auto windowResult = system->create_window({"CueEngine Scene Renderer WARP Test", {64, 64}});
    CHECK(windowResult.has_value());
    auto window = windowResult.take_value();
    CHECK(window->show().has_value());
    auto handle = cue::borrow_windows_window_handle(*window);
    auto backendResult = cue::dx12::DX12Backend::create(cue::dx12::AdapterSelection::Warp);
    CHECK(handle.has_value() && backendResult.has_value());
    auto backend = backendResult.take_value();
    auto *device = dynamic_cast<cue::dx12::DX12RenderDevice *>(backend->get_render_device());
    CHECK(device);
    CHECK(backend->create_swap_chain(*handle.try_value(), {64, 64, 2, DXGI_FORMAT_R8G8B8A8_UNORM, false, false, false})
              .has_value());
    auto rendererResult = cue::create_scene_renderer(*backend);
    CHECK(rendererResult.has_value());
    auto renderer = rendererResult.take_value();
    Capture capture;
    capture.device = device->device();
    cue::dx12::DX12MainFrameGraphConfig config;
    config.clearColor = {0.02f, 0.03f, 0.04f, 1.0f};
    config.configure = [&](cue::FrameGraph &a_graph, cue::FrameGraphResourceHandle) -> cue::Result<void>
    {
        auto pass = renderer->create_pass();
        if (!pass.has_value())
            return cue::Result<void>::failure(*pass.try_error());
        auto added = a_graph.add_pass(pass.take_value());
        if (!added.has_value())
            return cue::Result<void>::failure(*added.try_error());
        auto copied = a_graph.add_pass(std::make_unique<CapturePass>(capture));
        return copied.has_value() ? cue::Result<void>::success() : cue::Result<void>::failure(*copied.try_error());
    };
    auto graphResult = cue::dx12::DX12MainFrameGraph::create(*backend->get_resource_context(),
                                                             *backend->get_swap_chain(), std::move(config));
    CHECK(graphResult.has_value());
    auto graph = graphResult.take_value();
    auto draw = [&](std::uint64_t a_frame, cue::SceneRenderSnapshot a_snapshot) -> bool
    {
        CHECK(renderer->publish_frame(a_frame, std::move(a_snapshot)).has_value());
        auto result = renderer->render_frame(
            a_frame,
            [&]() -> cue::Result<void>
            {
                auto executed =
                    graph->execute(static_cast<std::uint32_t>(a_frame % 2), *backend->get_execution_context());
                if (!executed.has_value())
                    return cue::Result<void>::failure(*executed.try_error());
                return *executed.try_value() ? cue::Result<void>::success()
                                             : cue::Result<void>::failure(
                                                   {cue::ErrorCategory::InvalidState, "SceneTest.execute_cancelled"});
            });
        if (!result.has_value())
            std::fprintf(stderr, "Scene render error: %s\n", result.try_error()->operation.c_str());
        CHECK(result.has_value());
        auto completion = graph->submitted_completion();
        CHECK(completion && completion->wait().has_value());
        return true;
    };
    cue::math::floatTransform cameraTransform;
    cameraTransform.position.z = 3;
    auto view = cue::math::view_from_transform(cameraTransform);
    CHECK(view.has_value());
    cue::SceneRenderSnapshot base;
    base.camera = cue::RenderCameraData{view.take_value()};
    base.objects.push_back({cue::math::float4x4::identity(), std::string(cue::k_cubeMeshId)});
    CHECK(draw(0, base));
    auto blue = capture.center();
    CHECK(blue.has_value() && (*blue.try_value())[2] > 180 && (*blue.try_value())[0] < 80);
    auto projection = cue::math::perspective_rh_zo(base.camera->verticalFovRadians, 1.0f, base.camera->nearClip,
                                                   base.camera->farClip);
    CHECK(projection.has_value());
    auto expectedFrontDepth = cue::math::transform_point(cue::math::float3{0, 0, -2.5f}, *projection.try_value());
    auto frontDepth = capture.center_depth();
    CHECK(expectedFrontDepth.has_value() && frontDepth.has_value() &&
          cue::math::nearly_equal(*frontDepth.try_value(), expectedFrontDepth.try_value()->z, 0.0001f, 0.0001f));
    auto moved = base;
    moved.objects[0].world = cue::math::translation_matrix(cue::math::float3{10, 0, 0});
    CHECK(draw(1, std::move(moved)));
    auto background = capture.center();
    CHECK(background.has_value() && (*background.try_value())[0] < 15 && (*background.try_value())[1] < 15 &&
          (*background.try_value())[2] < 15);
    CHECK(draw(2, {}));
    auto empty = capture.center();
    CHECK(empty.has_value() && *empty.try_value() == *background.try_value());

    // 近い赤面を先、遠い青面を後に提出し、描画順ではなく Depth が勝つことを確認する
    auto rotation = cue::math::rotation_axis_angle(cue::math::float3{0, 1, 0}, cue::math::k_pi<float> / 2);
    CHECK(rotation.has_value());
    auto nearRotation = cue::math::rotation_matrix(*rotation.try_value());
    CHECK(nearRotation.has_value());
    auto overlap = base;
    overlap.objects[0].world = *nearRotation.try_value() * cue::math::translation_matrix(cue::math::float3{0, 0, 0.5f});
    overlap.objects.push_back(
        {cue::math::translation_matrix(cue::math::float3{0, 0, -1}), std::string(cue::k_cubeMeshId)});
    CHECK(draw(3, std::move(overlap)));
    auto red = capture.center();
    CHECK(red.has_value() && (*red.try_value())[0] > 180 && (*red.try_value())[2] < 80);

    // 先頭を画面外へ置き、2 番目の Indirect Record が別 Transform を参照することを確認する
    auto secondVisible = base;
    secondVisible.objects[0].world = cue::math::translation_matrix(cue::math::float3{10, 0, 0});
    secondVisible.objects.push_back(base.objects[0]);
    CHECK(draw(4, std::move(secondVisible)));
    auto secondBlue = capture.center();
    CHECK(secondBlue.has_value() && (*secondBlue.try_value())[2] > 180);

    CHECK(graph->resize(*backend->get_resource_context(), 96, 48).has_value());
    CHECK(capture.width == 96 && capture.height == 48 && draw(5, base));
    auto resized = capture.center();
    CHECK(resized.has_value() && (*resized.try_value())[2] > 180 && (*resized.try_value())[0] < 80);
    auto outside = capture.pixel(60, 24);
    CHECK(outside.has_value() && (*outside.try_value())[0] < 15 && (*outside.try_value())[1] < 15 &&
          (*outside.try_value())[2] < 15);
    CHECK(verify_debug_messages(*device->device()));
    CHECK(graph->shutdown().has_value());
    graph.reset();
    capture.readback.reset();
    capture.depthReadback.reset();
    CHECK(renderer->shutdown().has_value());
    renderer.reset();
    CHECK(backend->shutdown().has_value());
    return true;
}
} // namespace

/// @brief 本番 Backend / MeshPass / FrameGraph の接続を WARP の Readback で検証する
int main()
{
    return verify_scene_renderer() ? 0 : 1;
}
