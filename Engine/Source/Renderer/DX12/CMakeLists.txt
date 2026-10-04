# DX12 固有の Device と Queue／Command Pool の作成・所有を独立した Module にする
# 全画面表示 Shader は Build 時に DXIL へ変換し、実行時の File 読み込みを不要にする
file(GLOB CUE_DXC_CANDIDATES
    "$ENV{SystemDrive}/Program Files (x86)/Windows Kits/10/bin/*/x64/dxc.exe"
)
list(SORT CUE_DXC_CANDIDATES COMPARE NATURAL ORDER DESCENDING)
if(NOT CUE_DXC_CANDIDATES)
    message(FATAL_ERROR "Windows SDK dxc.exe is required for DX12 shaders")
endif()
list(GET CUE_DXC_CANDIDATES 0 CUE_DXC_EXECUTABLE)

set(CUE_FULLSCREEN_VS "${CMAKE_CURRENT_BINARY_DIR}/Generated/FullscreenTriangleVS.h")
set(CUE_FULLSCREEN_PS "${CMAKE_CURRENT_BINARY_DIR}/Generated/FullscreenTrianglePS.h")
set(CUE_FULLSCREEN_SHADER "${CMAKE_CURRENT_SOURCE_DIR}/../../../Shader/Hlsl/FullscreenTriangle.hlsl")
add_custom_command(OUTPUT "${CUE_FULLSCREEN_VS}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/Generated"
    COMMAND "${CUE_DXC_EXECUTABLE}" -T vs_6_0 -E vs_main -Fh "${CUE_FULLSCREEN_VS}"
            -Vn g_fullscreenTriangleVs "${CUE_FULLSCREEN_SHADER}"
    DEPENDS "${CUE_FULLSCREEN_SHADER}"
    VERBATIM
)
add_custom_command(OUTPUT "${CUE_FULLSCREEN_PS}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/Generated"
    COMMAND "${CUE_DXC_EXECUTABLE}" -T ps_6_0 -E ps_main -Fh "${CUE_FULLSCREEN_PS}"
            -Vn g_fullscreenTrianglePs "${CUE_FULLSCREEN_SHADER}"
    DEPENDS "${CUE_FULLSCREEN_SHADER}"
    VERBATIM
)
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
    Private/DX12FullscreenTriangle.cpp
    "${CUE_FULLSCREEN_VS}"
    "${CUE_FULLSCREEN_PS}"
)

target_include_directories(Cue.Renderer.DX12 PUBLIC Public)
target_include_directories(Cue.Renderer.DX12 PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/Generated")
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
