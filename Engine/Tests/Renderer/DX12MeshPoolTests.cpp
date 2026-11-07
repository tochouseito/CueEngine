#include <DX12/DX12MeshPool.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include <d3d12sdklayers.h>

#include <Assets/BuiltinMesh.h>
#include <DX12/DX12Backend.h>
#include <DX12/DX12CommandPool.h>
#include <DX12/DX12GpuResource.h>
#include <DX12/DX12QueuePool.h>
#include <DX12/DX12RenderDevice.h>
#include <RHI/CommandCompletion.h>

#define CHECK(a_condition)                                                                                             \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(a_condition))                                                                                            \
        {                                                                                                              \
            std::fprintf(stderr, "MeshPool test line %d\n", __LINE__);                                                 \
            return false;                                                                                              \
        }                                                                                                              \
    } while (false)

namespace
{
/// @brief すべての Copy Queue を停止し、未完了 Upload の公開禁止を再現する
class CopyGate final
{
  public:
    /// @brief 早期失敗時にも GPU 待機を必ず解除する
    ~CopyGate()
    {
        open();
    }
    /// @brief Queue を全て借りた後に外部 Fence 待機を記録する
    bool close(ID3D12Device &a_device, cue::IQueuePool &a_queues)
    {
        CHECK(SUCCEEDED(a_device.CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence))));
        CHECK(SUCCEEDED(m_fence->SetName(L"CueEngine Mesh Test Copy Gate")));
        std::vector<cue::queueLease> queues;
        for (int index = 0; index < 4; ++index)
        {
            auto result = a_queues.acquire(cue::QueueType::Copy);
            CHECK(result.has_value());
            queues.push_back(result.take_value());
            auto *native = dynamic_cast<cue::dx12::DX12GpuCommandQueue *>(queues.back().get());
            CHECK(native && SUCCEEDED(native->command_queue()->Wait(m_fence.Get(), 1)));
        }
        return true;
    }
    /// @brief 未提出の Test 分岐でも滞留 Queue を解放する
    void open() noexcept
    {
        if (m_fence)
            (void)m_fence->Signal(1);
    }

    /// @brief 全 Queue の完了確認後にだけ Test の Fence 参照を解放する
    void release() noexcept
    {
        m_fence.Reset();
    }

  private:
    Microsoft::WRL::ComPtr<ID3D12Fence> m_fence;
};

/// @brief GPU Read 完了までの遅延破棄を制御する。実 GPU 提出は模倣しない
class ManualCompletion final : public cue::ICommandCompletion
{
  public:
    bool isDone = false;
    bool hasError = false;
    /// @brief Graphics の読み取り完了点として扱う
    cue::QueueType type() const noexcept override
    {
        return cue::QueueType::Graphics;
    }
    /// @brief Test が設定した完了状態を返す
    bool is_complete() const noexcept override
    {
        return isDone;
    }
    /// @brief Device Lost 相当の診断も未完了と区別して伝える
    cue::Result<bool> completion_status() const override
    {
        if (hasError)
            return cue::Result<bool>::failure({cue::ErrorCategory::PlatformFailure, "MeshTest.device_lost"});
        return cue::Result<bool>::success(isDone);
    }
    /// @brief Test 早期失敗時の所有解除でも停止可能にする
    cue::Result<void> wait() override
    {
        isDone = true;
        return cue::Result<void>::success();
    }
};

/// @brief 計測用 Sleep に依存せず全 Queue の転送完了を確認する
bool ready(cue::IMeshPool &a_pool, cue::MeshHandle a_handle, cue::IQueuePool &a_queues)
{
    CHECK(a_queues.wait_idle().has_value());
    auto state = a_pool.status(a_handle);
    CHECK(state.has_value() && *state.try_value() == cue::MeshStatus::Ready);
    return true;
}

/// @brief GPU 上の各分離 Stream を Graphics Queue から読み戻し、CPU 正本と Byte 単位で照合する
bool verify_readback(cue::meshLease &a_lease, const cue::MeshData &a_mesh, cue::dx12::DX12RenderDevice &a_device,
                     cue::ICommandPool &a_commands, cue::IQueuePool &a_queues)
{
    std::array<std::vector<std::byte>, 4> expected;
    const auto appendVertices = []<class V>(std::vector<std::byte> &a_output, std::span<const V> a_values)
    {
        for (const auto &value : a_values)
            for (std::size_t component = 0; component < V::k_dimension; ++component)
            {
                const float scalar = value[component];
                const auto bytes = std::as_bytes(std::span{&scalar, 1});
                a_output.insert(a_output.end(), bytes.begin(), bytes.end());
            }
    };
    appendVertices(expected[0], a_mesh.positions());
    appendVertices(expected[1], a_mesh.normals());
    appendVertices(expected[2], a_mesh.uv0());
    const auto indices = a_mesh.index_format() == cue::MeshIndexFormat::Uint16 ? std::as_bytes(a_mesh.indices16())
                                                                               : std::as_bytes(a_mesh.indices32());
    expected[3].assign(indices.begin(), indices.end());
    std::size_t totalBytes = 0;
    for (const auto &bytes : expected)
        totalBytes += bytes.size();
    auto readbackResult = cue::dx12::DX12GpuResource::create_buffer(
        *a_device.device(), {totalBytes, cue::GpuMemoryUsage::Readback}, L"CueEngine Mesh Test Readback");
    CHECK(readbackResult.has_value());
    auto readback = readbackResult.take_value();
    auto queueResult = a_queues.acquire(cue::QueueType::Graphics);
    auto commandResult = a_commands.acquire(cue::QueueType::Graphics);
    CHECK(queueResult.has_value() && commandResult.has_value());
    auto queue = queueResult.take_value();
    auto command = commandResult.take_value();
    auto *native = dynamic_cast<cue::dx12::DX12GpuCommandContext *>(command.get());
    CHECK(native);
    const auto &draw = a_lease->draw_range();
    std::size_t destination = 0;
    for (std::size_t index = 0; index < expected.size(); ++index)
    {
        if (expected[index].empty())
            continue;
        const auto &view = index == 3 ? draw.indices : draw.vertices[index];
        auto *resource = dynamic_cast<cue::dx12::DX12GpuResource *>(view.resource);
        CHECK(resource && resource->memory_usage() == cue::GpuMemoryUsage::Default && !resource->is_placed());
        const auto first = index == 3 ? draw.firstIndex : static_cast<std::uint32_t>(draw.baseVertex);
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resource->resource();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        native->command_list()->ResourceBarrier(1, &barrier);
        native->command_list()->CopyBufferRegion(readback->resource(), destination, resource->resource(),
                                                 std::uint64_t(first) * view.stride, expected[index].size());
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        native->command_list()->ResourceBarrier(1, &barrier);
        destination += expected[index].size();
    }
    CHECK(command->close().has_value());
    auto completion = std::make_shared<cue::SharedCommandCompletion>();
    auto submitted = a_commands.submit(*queue, *command);
    CHECK(submitted.has_value());
    completion->set(submitted.take_value());
    CHECK(a_lease->mark_submitted(completion).has_value());
    CHECK(completion->wait().has_value());
    std::vector<std::byte> output(totalBytes);
    CHECK(readback->read(0, output).has_value());
    destination = 0;
    for (const auto &bytes : expected)
    {
        CHECK(std::equal(bytes.begin(), bytes.end(), output.begin() + destination));
        destination += bytes.size();
    }
    return true;
}

/// @brief D3D12 Debug Layer に State / 寿命 / Copy の Error が残っていないことを確認する
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

/// @brief 本番 Backend 所有の Pool と個別容量の Pool で転送・公開・Page 増設・回収を検証する
bool verify_pool()
{
    auto backendResult = cue::dx12::DX12Backend::create(cue::dx12::AdapterSelection::Warp);
    CHECK(backendResult.has_value());
    auto backend = backendResult.take_value();
    auto *device = dynamic_cast<cue::dx12::DX12RenderDevice *>(backend->get_render_device());
    auto &commands = *backend->get_command_pool();
    auto &queues = *backend->get_queue_pool();
    CHECK(device && backend->get_mesh_pool());
    auto cubeResult = cue::create_cube_mesh();
    CHECK(cubeResult.has_value());
    auto cube = cubeResult.take_value();
    CHECK(!cue::dx12::DX12MeshPool::create(*device, commands, queues, {0, 1, 1, 1}).has_value());
    auto poolResult = cue::dx12::DX12MeshPool::create(*device, commands, queues, {48, 72, 8, 16});
    CHECK(poolResult.has_value());
    auto pool = poolResult.take_value();

    // SwapChain と同じく Graphics を借用したままでも、Copy Queue の転送を開始できる
    auto graphicsResult = queues.acquire(cue::QueueType::Graphics);
    CHECK(graphicsResult.has_value());
    auto graphics = graphicsResult.take_value();
    CopyGate gate;
    CHECK(gate.close(*device->device(), queues));
    auto firstResult = pool->upload(cube);
    CHECK(firstResult.has_value());
    const auto first = firstResult.take_value();
    auto pending = pool->status(first);
    CHECK(pending.has_value() && *pending.try_value() == cue::MeshStatus::Uploading);
    CHECK(!pool->acquire(first).has_value());
    CHECK(!backend->get_mesh_pool()->status(first).has_value());
    gate.open();
    graphics.reset();
    CHECK(ready(*pool, first, queues));
    gate.release();
    CopyGate secondGate;
    CHECK(secondGate.close(*device->device(), queues));
    auto secondResult = pool->upload(cube);
    CHECK(secondResult.has_value());
    const auto second = secondResult.take_value();
    auto firstDuringUpload = pool->status(first);
    CHECK(firstDuringUpload.has_value() && *firstDuringUpload.try_value() == cue::MeshStatus::Uploading);
    CHECK(!pool->acquire(first).has_value());
    secondGate.open();
    CHECK(ready(*pool, second, queues));
    secondGate.release();
    auto firstLeaseResult = pool->acquire(first);
    auto secondLeaseResult = pool->acquire(second);
    CHECK(firstLeaseResult.has_value() && secondLeaseResult.has_value());
    auto firstLease = firstLeaseResult.take_value();
    auto secondLease = secondLeaseResult.take_value();
    CHECK(firstLease->draw_range().pageId == secondLease->draw_range().pageId);
    CHECK(firstLease->draw_range().baseVertex == 0 && secondLease->draw_range().baseVertex == 24);
    CHECK(secondLease->draw_range().firstIndex == 36 && secondLease->draw_range().indices.stride == 2);
    CHECK(verify_readback(secondLease, cube, *device, commands, queues));
    secondLease.reset();

    auto pendingRead = std::make_shared<ManualCompletion>();
    CHECK(pool->retire(first).has_value());
    CHECK(!pool->acquire(first).has_value() && !pool->status(first).has_value());
    CHECK(firstLease->mark_submitted(pendingRead).has_value());
    CHECK(!firstLease->mark_submitted(pendingRead).has_value());
    auto collected = pool->collect();
    CHECK(collected.has_value() && *collected.try_value() == 0);
    firstLease.reset();
    collected = pool->collect();
    CHECK(collected.has_value() && *collected.try_value() == 0);
    pendingRead->hasError = true;
    CHECK(!pool->collect().has_value());
    pendingRead->hasError = false;
    pendingRead->isDone = true;
    collected = pool->collect();
    CHECK(collected.has_value() && *collected.try_value() == 1);
    auto reusedResult = pool->upload(cube);
    CHECK(reusedResult.has_value());
    const auto reused = reusedResult.take_value();
    CHECK(reused.index == first.index && reused.generation != first.generation);
    CHECK(!pool->retire(first).has_value());
    CHECK(ready(*pool, reused, queues));
    auto reusedLeaseResult = pool->acquire(reused);
    CHECK(reusedLeaseResult.has_value());
    auto reusedLease = reusedLeaseResult.take_value();
    CHECK(reusedLease->draw_range().baseVertex == 0 && reusedLease->draw_range().firstIndex == 0);

    // CPU Snapshot がある Page と、返却後も GPU Read 中の Page には追加転送しない
    auto thirdResult = pool->upload(cube);
    CHECK(thirdResult.has_value());
    const auto third = thirdResult.take_value();
    CHECK(ready(*pool, third, queues));
    auto thirdLeaseResult = pool->acquire(third);
    CHECK(thirdLeaseResult.has_value());
    auto thirdLease = thirdLeaseResult.take_value();
    CHECK(thirdLease->draw_range().pageId != reusedLease->draw_range().pageId);
    const auto thirdPage = thirdLease->draw_range().pageId;
    auto thirdRead = std::make_shared<ManualCompletion>();
    CHECK(thirdLease->mark_submitted(thirdRead).has_value());
    thirdLease.reset();
    auto fourthResult = pool->upload(cube);
    CHECK(fourthResult.has_value());
    const auto fourth = fourthResult.take_value();
    CHECK(ready(*pool, fourth, queues));
    auto fourthLeaseResult = pool->acquire(fourth);
    CHECK(fourthLeaseResult.has_value());
    auto fourthLease = fourthLeaseResult.take_value();
    CHECK(fourthLease->draw_range().pageId != thirdPage);
    thirdRead->isDone = true;
    reusedLease.reset();
    fourthLease.reset();

    // 32bit と属性なしは独立した Page に分ける
    cue::MeshDescriptor plain;
    plain.positions = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    plain.indices = std::vector<std::uint32_t>{0, 1, 2};
    auto plainResult = cue::MeshData::create(std::move(plain));
    CHECK(plainResult.has_value());
    auto plainMesh = plainResult.take_value();
    auto plainUpload = pool->upload(plainMesh);
    CHECK(plainUpload.has_value());
    const auto plainHandle = plainUpload.take_value();
    CHECK(ready(*pool, plainHandle, queues));
    auto plainLeaseResult = pool->acquire(plainHandle);
    CHECK(plainLeaseResult.has_value());
    auto plainLease = plainLeaseResult.take_value();
    CHECK(plainLease->draw_range().indices.stride == 4 && !plainLease->draw_range().vertices[1].resource &&
          !plainLease->draw_range().vertices[2].resource && plainLease->draw_range().pageId != thirdPage);
    CHECK(verify_readback(plainLease, plainMesh, *device, commands, queues));
    plainLease.reset();
    CHECK(pool->shutdown().has_value() && pool->shutdown().has_value());
    CHECK(!pool->upload(cube).has_value() && !pool->acquire(reused).has_value());
    pool.reset();

    // 16bit Mesh を 65536 頂点より後へ置いても Index を書き換えず BaseVertex で解決する
    auto widePoolResult = cue::dx12::DX12MeshPool::create(*device, commands, queues, {70000, 12, 1, 4});
    CHECK(widePoolResult.has_value());
    auto widePool = widePoolResult.take_value();
    cue::MeshDescriptor large;
    large.positions.resize(65536);
    large.indices = std::vector<std::uint16_t>{0, 1, 65535};
    auto largeResult = cue::MeshData::create(std::move(large));
    CHECK(largeResult.has_value());
    auto largeMesh = largeResult.take_value();
    auto largeUpload = widePool->upload(largeMesh);
    CHECK(largeUpload.has_value() && ready(*widePool, *largeUpload.try_value(), queues));
    cue::MeshDescriptor triangleDescriptor;
    triangleDescriptor.positions = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    triangleDescriptor.indices = std::vector<std::uint16_t>{0, 1, 2};
    auto smallResult = cue::MeshData::create(std::move(triangleDescriptor));
    CHECK(smallResult.has_value());
    auto smallMesh = smallResult.take_value();
    auto smallUpload = widePool->upload(smallMesh);
    CHECK(smallUpload.has_value());
    const auto smallHandle = smallUpload.take_value();
    CHECK(ready(*widePool, smallHandle, queues));
    auto smallLeaseResult = widePool->acquire(smallHandle);
    CHECK(smallLeaseResult.has_value());
    auto smallLease = smallLeaseResult.take_value();
    CHECK(smallLease->draw_range().baseVertex == 65536 && smallLease->draw_range().indices.stride == 2);
    CHECK(verify_readback(smallLease, smallMesh, *device, commands, queues));
    CHECK(!widePool->upload(cube).has_value());
    // 空き容量があっても CPU Lease を Pin した Page へ書かず、上限到達を返す
    CHECK(!widePool->upload(smallMesh).has_value());
    CHECK(!widePool->shutdown().has_value());
    CHECK(!widePool->upload(smallMesh).has_value());
    smallLease.reset();
    CHECK(widePool->shutdown().has_value());
    widePool.reset();

    // 転送中に退役した Mesh も Fence が終わるまでは回収しない
    CopyGate retirementGate;
    CHECK(retirementGate.close(*device->device(), queues));
    auto retiringUpload = backend->get_mesh_pool()->upload(cube);
    CHECK(retiringUpload.has_value());
    CHECK(backend->get_mesh_pool()->retire(*retiringUpload.try_value()).has_value());
    auto pendingRetirement = backend->get_mesh_pool()->collect();
    CHECK(pendingRetirement.has_value() && *pendingRetirement.try_value() == 0);
    retirementGate.open();
    CHECK(queues.wait_idle().has_value());
    retirementGate.release();
    auto finishedRetirement = backend->get_mesh_pool()->collect();
    CHECK(finishedRetirement.has_value() && *finishedRetirement.try_value() == 1);
    CHECK(verify_debug_messages(*device->device()));
    CHECK(backend->shutdown().has_value() && !backend->get_mesh_pool());
    return true;
}
} // namespace

/// @brief CPU Pin と GPU Fence の双方を含む MeshPool の WARP 統合検証を行う
int main()
{
    return verify_pool() ? 0 : 1;
}
