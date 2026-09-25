#include <Cue/Renderer/D3D12/D3D12Renderer.h>

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

namespace cue
{
namespace
{
constexpr UINT k_bufferCount = 2;
constexpr DWORD k_gpuWaitMilliseconds = 10'000;

/// @brief HRESULTを操作名付きのPlatform Errorへ変換する
Error gpu_error(const char* a_operation, HRESULT a_result)
{
    return {ErrorCategory::PlatformFailure, a_operation, static_cast<std::int64_t>(a_result)};
}
} // namespace

class D3D12Renderer::State final
{
public:
    struct FrameResource final
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> backBuffer;
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
        D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
        std::uint64_t fenceValue = 0;
    };

    /// @brief Event HandleをCOM資源と同じOwnerで解放する
    ~State()
    {
        if (fenceEvent)
        {
            CloseHandle(fenceEvent);
        }
    }

    /// @brief 指定したFence値に達するまでCPU側で待機する
    [[nodiscard]] Result<void> wait_for(std::uint64_t a_value)
    {
        if (fence->GetCompletedValue() >= a_value)
        {
            return Result<void>::success();
        }
        const HRESULT eventResult = fence->SetEventOnCompletion(a_value, fenceEvent);
        if (FAILED(eventResult))
        {
            return Result<void>::failure(gpu_error("ID3D12Fence.SetEventOnCompletion", eventResult));
        }
        const DWORD waitResult = WaitForSingleObject(fenceEvent, k_gpuWaitMilliseconds);
        if (waitResult != WAIT_OBJECT_0)
        {
            // Device Lost時は待機の症状よりDeviceのHRESULTを優先して返す
            const HRESULT removedReason = device ? device->GetDeviceRemovedReason() : S_OK;
            if (FAILED(removedReason))
            {
                return Result<void>::failure(gpu_error("ID3D12Device.GetDeviceRemovedReason", removedReason));
            }
            return Result<void>::failure({ErrorCategory::PlatformFailure, "WaitForSingleObject.GpuFence",
                                          static_cast<std::int64_t>(waitResult == WAIT_FAILED ? GetLastError() : waitResult)});
        }
        return Result<void>::success();
    }

    /// @brief Queue上の全ての処理が完了したことを確認する
    [[nodiscard]] Result<void> wait_idle()
    {
        // Queue作成前の部分失敗では待機対象がない
        if (!queue || !fence)
        {
            return Result<void>::success();
        }
        const std::uint64_t value = nextFenceValue++;
        const HRESULT signalResult = queue->Signal(fence.Get(), value);
        if (FAILED(signalResult))
        {
            return Result<void>::failure(gpu_error("ID3D12CommandQueue.Signal", signalResult));
        }
        return wait_for(value);
    }

    /// @brief GPU完了後に旧Buffer参照を解放してRTVを再生成する
    [[nodiscard]] Result<void> resize(WindowSize a_size)
    {
        if (a_size.width == size.width && a_size.height == size.height)
        {
            return Result<void>::success();
        }
        auto idleResult = wait_idle();
        if (!idleResult.has_value())
        {
            return idleResult;
        }

        // Closed Command Listにも旧Buffer参照が残り得るため先に解放する
        commandList.Reset();
        for (auto& frame : frames)
        {
            frame.backBuffer.Reset();
            frame.fenceValue = 0;
        }
        HRESULT result = swapChain->ResizeBuffers(k_bufferCount, a_size.width, a_size.height,
                                                   DXGI_FORMAT_R8G8B8A8_UNORM, 0);
        if (FAILED(result))
        {
            return Result<void>::failure(gpu_error("IDXGISwapChain.ResizeBuffers", result));
        }

        const UINT descriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        const auto rtvStart = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        for (UINT index = 0; index < k_bufferCount; ++index)
        {
            auto& frame = frames[index];
            result = swapChain->GetBuffer(index, IID_PPV_ARGS(&frame.backBuffer));
            if (FAILED(result))
            {
                return Result<void>::failure(gpu_error("IDXGISwapChain.GetBuffer", result));
            }
            frame.rtv.ptr = rtvStart.ptr + static_cast<SIZE_T>(index) * descriptorSize;
            device->CreateRenderTargetView(frame.backBuffer.Get(), nullptr, frame.rtv);
        }
        result = frames[0].allocator->Reset();
        if (FAILED(result))
        {
            return Result<void>::failure(gpu_error("ID3D12CommandAllocator.Reset.resize", result));
        }
        result = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                           frames[0].allocator.Get(), nullptr, IID_PPV_ARGS(&commandList));
        if (FAILED(result))
        {
            return Result<void>::failure(gpu_error("ID3D12Device.CreateCommandList.resize", result));
        }
        result = commandList->Close();
        if (FAILED(result))
        {
            return Result<void>::failure(gpu_error("ID3D12GraphicsCommandList.Close.resize", result));
        }
        size = a_size;
        {
            std::lock_guard lock(surfaceMutex);
            progress.surfaceSize = a_size;
        }
        return Result<void>::success();
    }

    Microsoft::WRL::ComPtr<IDXGIFactory6> factory;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
    Microsoft::WRL::ComPtr<IDXGISwapChain3> swapChain;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtvHeap;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    std::array<FrameResource, k_bufferCount> frames;
    HANDLE fenceEvent = nullptr;
    std::uint64_t nextFenceValue = 1;
    WindowSize size{};
    std::mutex surfaceMutex;
    WindowSize requestedSize{};
    D3D12RendererProgress progress{};
    bool isMinimized = false;
    bool isWarp = false;
    std::thread::id renderThreadId;
    std::uint64_t lastFrame = 0;
    bool hasRendered = false;
};

/// @brief 初期化済みGPU Stateを受け取る
D3D12Renderer::D3D12Renderer(std::unique_ptr<State> a_state) noexcept
    : m_state(std::move(a_state))
{
}

/// @brief 呼出側が明示停止を忘れてもGPUを待ってから所有資源を破棄する
D3D12Renderer::~D3D12Renderer()
{
    [[maybe_unused]] auto result = shutdown();
}

/// @brief Hardware優先または明示WARPでDeviceとPresentation資源を生成する
Result<std::unique_ptr<D3D12Renderer>> D3D12Renderer::create(void* a_nativeWindow, WindowSize a_clientSize,
                                                                  bool a_useWarp)
{
    using RendererResult = Result<std::unique_ptr<D3D12Renderer>>;

    // Window生成後の有効なClient AreaだけでSwap Chainを作る
    if (!a_nativeWindow || a_clientSize.width == 0 || a_clientSize.height == 0)
    {
        return RendererResult::failure({ErrorCategory::InvalidArgument, "D3D12Renderer.create"});
    }
    auto state = std::make_unique<State>();
    state->size = a_clientSize;
    state->requestedSize = a_clientSize;
    state->progress.surfaceSize = a_clientSize;
    state->isWarp = a_useWarp;

#if defined(_DEBUG) && !defined(CUE_SHIPPING)
    // Debug Layerがインストール済みなら初期化時からD3D12検証を有効にする
    Microsoft::WRL::ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
    {
        debug->EnableDebugLayer();
    }
#endif

    HRESULT result = CreateDXGIFactory2(0, IID_PPV_ARGS(&state->factory));
    if (FAILED(result))
    {
        return RendererResult::failure(gpu_error("CreateDXGIFactory2", result));
    }

    // Hardwareを選ぶ場合は高性能優先でD3D12対応Adapterを探す
    if (a_useWarp)
    {
        result = state->factory->EnumWarpAdapter(IID_PPV_ARGS(&state->adapter));
        if (FAILED(result))
        {
            return RendererResult::failure(gpu_error("IDXGIFactory.EnumWarpAdapter", result));
        }
    }
    else
    {
        for (UINT index = 0;; ++index)
        {
            Microsoft::WRL::ComPtr<IDXGIAdapter1> candidate;
            result = state->factory->EnumAdapterByGpuPreference(index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                                 IID_PPV_ARGS(&candidate));
            if (result == DXGI_ERROR_NOT_FOUND)
            {
                break;
            }
            if (FAILED(result))
            {
                return RendererResult::failure(gpu_error("IDXGIFactory.EnumAdapterByGpuPreference", result));
            }
            DXGI_ADAPTER_DESC1 desc{};
            result = candidate->GetDesc1(&desc);
            if (FAILED(result))
            {
                return RendererResult::failure(gpu_error("IDXGIAdapter.GetDesc1", result));
            }
            if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0 &&
                SUCCEEDED(D3D12CreateDevice(candidate.Get(), D3D_FEATURE_LEVEL_11_0,
                                            __uuidof(ID3D12Device), nullptr)))
            {
                state->adapter = std::move(candidate);
                break;
            }
        }
        if (!state->adapter)
        {
            return RendererResult::failure({ErrorCategory::PlatformFailure, "D3D12Renderer.noHardwareAdapter",
                                            static_cast<std::int64_t>(DXGI_ERROR_NOT_FOUND)});
        }
    }

    result = D3D12CreateDevice(state->adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&state->device));
    if (FAILED(result))
    {
        return RendererResult::failure(gpu_error("D3D12CreateDevice", result));
    }

    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    result = state->device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&state->queue));
    if (FAILED(result))
    {
        return RendererResult::failure(gpu_error("ID3D12Device.CreateCommandQueue", result));
    }

    // Flip ModelのSwap ChainへDeviceではなくDirect Queueを渡す
    DXGI_SWAP_CHAIN_DESC1 swapDesc{};
    swapDesc.Width = a_clientSize.width;
    swapDesc.Height = a_clientSize.height;
    swapDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapDesc.BufferCount = k_bufferCount;
    swapDesc.SampleDesc.Count = 1;
    swapDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> swapChain;
    result = state->factory->CreateSwapChainForHwnd(state->queue.Get(), static_cast<HWND>(a_nativeWindow),
                                                    &swapDesc, nullptr, nullptr, &swapChain);
    if (FAILED(result))
    {
        return RendererResult::failure(gpu_error("IDXGIFactory.CreateSwapChainForHwnd", result));
    }
    result = swapChain.As(&state->swapChain);
    if (FAILED(result))
    {
        return RendererResult::failure(gpu_error("IDXGISwapChain.QueryInterface", result));
    }

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heapDesc.NumDescriptors = k_bufferCount;
    result = state->device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&state->rtvHeap));
    if (FAILED(result))
    {
        return RendererResult::failure(gpu_error("ID3D12Device.CreateDescriptorHeap", result));
    }
    const UINT descriptorSize = state->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    const auto rtvStart = state->rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT index = 0; index < k_bufferCount; ++index)
    {
        auto& frame = state->frames[index];
        result = state->swapChain->GetBuffer(index, IID_PPV_ARGS(&frame.backBuffer));
        if (FAILED(result))
        {
            return RendererResult::failure(gpu_error("IDXGISwapChain.GetBuffer", result));
        }
        frame.rtv.ptr = rtvStart.ptr + static_cast<SIZE_T>(index) * descriptorSize;
        state->device->CreateRenderTargetView(frame.backBuffer.Get(), nullptr, frame.rtv);
        result = state->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                        IID_PPV_ARGS(&frame.allocator));
        if (FAILED(result))
        {
            return RendererResult::failure(gpu_error("ID3D12Device.CreateCommandAllocator", result));
        }
    }
    result = state->device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                               state->frames[0].allocator.Get(), nullptr,
                                               IID_PPV_ARGS(&state->commandList));
    if (FAILED(result))
    {
        return RendererResult::failure(gpu_error("ID3D12Device.CreateCommandList", result));
    }
    result = state->commandList->Close();
    if (FAILED(result))
    {
        return RendererResult::failure(gpu_error("ID3D12GraphicsCommandList.Close", result));
    }

    result = state->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&state->fence));
    if (FAILED(result))
    {
        return RendererResult::failure(gpu_error("ID3D12Device.CreateFence", result));
    }
    state->fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!state->fenceEvent)
    {
        return RendererResult::failure({ErrorCategory::PlatformFailure, "CreateEventW.GpuFence", GetLastError()});
    }
    D3D12Renderer renderer(std::move(state));
    return RendererResult::success(std::make_unique<D3D12Renderer>(std::move(renderer)));
}

/// @brief GPU作業を完了させてからWindow依存資源を破棄する
Result<void> D3D12Renderer::shutdown()
{
    if (!m_state)
    {
        return Result<void>::success();
    }
    auto waitResult = m_state->wait_idle();
    m_state.reset();
    return waitResult;
}

/// @brief Back BufferをClearしてGPU完了条件をFrame単位で記録する
Result<void> D3D12Renderer::render_frame(std::uint64_t a_frame)
{
    if (!m_state)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "D3D12Renderer.render_frame"});
    }
    State& state = *m_state;
    if (state.renderThreadId == std::thread::id{})
    {
        state.renderThreadId = std::this_thread::get_id();
    }
    if (state.renderThreadId != std::this_thread::get_id())
    {
        return Result<void>::failure({ErrorCategory::WrongThread, "D3D12Renderer.render_frame"});
    }
    if (state.hasRendered && a_frame <= state.lastFrame)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "D3D12Renderer.frameOrder"});
    }

    // MainThreadのResize連打は最後の要求だけを反映し、最小化中はGPU操作を休止する
    WindowSize requestedSize{};
    bool isMinimized = false;
    {
        std::lock_guard lock(state.surfaceMutex);
        requestedSize = state.requestedSize;
        isMinimized = state.isMinimized;
    }
    if (isMinimized || requestedSize.width == 0 || requestedSize.height == 0)
    {
        state.lastFrame = a_frame;
        state.hasRendered = true;
        return Result<void>::success();
    }
    auto resizeResult = state.resize(requestedSize);
    if (!resizeResult.has_value())
    {
        return resizeResult;
    }

    // BufferごとのFence完了後だけAllocatorを再利用する
    const UINT index = state.swapChain->GetCurrentBackBufferIndex();
    auto& frame = state.frames[index];
    if (frame.fenceValue != 0)
    {
        auto waitResult = state.wait_for(frame.fenceValue);
        if (!waitResult.has_value())
        {
            return waitResult;
        }
    }
    HRESULT result = frame.allocator->Reset();
    if (FAILED(result))
    {
        return Result<void>::failure(gpu_error("ID3D12CommandAllocator.Reset", result));
    }
    result = state.commandList->Reset(frame.allocator.Get(), nullptr);
    if (FAILED(result))
    {
        return Result<void>::failure(gpu_error("ID3D12GraphicsCommandList.Reset", result));
    }

    // Present可能な状態からRTVへ遷移して単色で塗り、Present状態へ戻す
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = frame.backBuffer.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    state.commandList->ResourceBarrier(1, &barrier);
    state.commandList->OMSetRenderTargets(1, &frame.rtv, FALSE, nullptr);
    constexpr float k_clearColor[4] = {0.07f, 0.13f, 0.25f, 1.0f};
    state.commandList->ClearRenderTargetView(frame.rtv, k_clearColor, 0, nullptr);
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    state.commandList->ResourceBarrier(1, &barrier);
    result = state.commandList->Close();
    if (FAILED(result))
    {
        return Result<void>::failure(gpu_error("ID3D12GraphicsCommandList.Close", result));
    }
    ID3D12CommandList* lists[] = {state.commandList.Get()};
    state.queue->ExecuteCommandLists(1, lists);

    // FrameControllerが60 FPSを制御するためPresent側では待機を追加しない
    result = state.swapChain->Present(0, 0);
    if (FAILED(result))
    {
        return Result<void>::failure(gpu_error("IDXGISwapChain.Present", result));
    }
    const std::uint64_t fenceValue = state.nextFenceValue++;
    result = state.queue->Signal(state.fence.Get(), fenceValue);
    if (FAILED(result))
    {
        return Result<void>::failure(gpu_error("ID3D12CommandQueue.Signal", result));
    }
    frame.fenceValue = fenceValue;
    state.lastFrame = a_frame;
    state.hasRendered = true;
    {
        std::lock_guard lock(state.surfaceMutex);
        ++state.progress.presentedFrames;
    }
    return Result<void>::success();
}

/// @brief Window Eventの最新Sizeと最小化状態をRender Threadへ渡す
Result<void> D3D12Renderer::request_surface(WindowSize a_clientSize, bool a_isMinimized)
{
    if (!m_state)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "D3D12Renderer.request_surface"});
    }
    std::lock_guard lock(m_state->surfaceMutex);
    if (!a_isMinimized)
    {
        m_state->requestedSize = a_clientSize;
    }
    m_state->isMinimized = a_isMinimized;
    return Result<void>::success();
}

/// @brief 適用済みSurfaceとPresent数を同期して返す
Result<D3D12RendererProgress> D3D12Renderer::progress() const
{
    if (!m_state)
    {
        return Result<D3D12RendererProgress>::failure({ErrorCategory::InvalidState, "D3D12Renderer.progress"});
    }
    std::lock_guard lock(m_state->surfaceMutex);
    return Result<D3D12RendererProgress>::success(m_state->progress);
}

/// @brief Adapter選択経路を診断可能にする
bool D3D12Renderer::is_warp() const noexcept
{
    return m_state && m_state->isWarp;
}
} // namespace cue
