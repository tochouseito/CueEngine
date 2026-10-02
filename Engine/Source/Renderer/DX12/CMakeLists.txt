# DX12 固有の Device と Queue／Command Pool の作成・所有を独立した Module にする
add_library(Cue.Renderer.DX12 STATIC
    Private/BackendFactory.cpp
    Private/DX12Backend.cpp
    Private/DX12CommandPool.cpp
    Private/DX12DescriptorAllocator.cpp
    Private/DX12FrameGraphFrames.cpp
    Private/DX12FrameGraphExecutor.cpp
    Private/DX12FrameGraphContext.cpp
    Private/DX12FrameGraphResources.cpp
    Private/DX12GpuResource.cpp
    Private/DX12GpuResourcePool.cpp
    Private/DX12MainFrameGraph.cpp
    Private/DX12PlacedResourceAllocator.cpp
    Private/DX12RenderDevice.cpp
    Private/DX12QueuePool.cpp
    Private/DX12ResourceLeakChecker.cpp
    Private/DX12SwapChain.cpp
)

target_include_directories(Cue.Renderer.DX12 PUBLIC Public)
target_link_libraries(Cue.Renderer.DX12
    PUBLIC Cue.Foundation Cue.Renderer.FrameGraph Cue.Renderer.Passes Cue.Renderer.RHI
    PRIVATE Cue.Platform.Windows d3d12 dxgi dxguid
)
target_compile_features(Cue.Renderer.DX12 PUBLIC cxx_std_20)

if(MSVC)
    target_compile_options(Cue.Renderer.DX12 PRIVATE
        /utf-8
        $<$<CONFIG:Development>:/O2>
        $<$<CONFIG:Development>:/Zi>
    )
endif()

target_compile_definitions(Cue.Renderer.DX12 PRIVATE
    $<$<CONFIG:Release>:CUE_SHIPPING=1>
)
