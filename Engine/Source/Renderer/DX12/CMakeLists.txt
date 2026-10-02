# DX12 固有の Device と Queue／Command Pool の作成・所有を独立した Module にする
add_library(Cue.Renderer.DX12 STATIC
    Private/BackendFactory.cpp
    Private/DX12Backend.cpp
    Private/DX12CommandPool.cpp
    Private/DX12DescriptorAllocator.cpp
    Private/DX12FinalColorFrames.cpp
    Private/DX12FrameGraphExecutor.cpp
    Private/DX12FrameGraphResources.cpp
    Private/DX12FullscreenPipeline.cpp
    Private/DX12GpuResource.cpp
    Private/DX12GpuResourcePool.cpp
    Private/DX12MainFrameGraph.cpp
    Private/DX12PlacedResourceAllocator.cpp
    Private/DX12RenderDevice.cpp
    Private/DX12QueuePool.cpp
    Private/DX12ResourceLeakChecker.cpp
    Private/DX12SwapChain.cpp
)

# Windows SDK の DXC で HLSL をビルド成果物に変換し、Pipeline へ埋め込む
set(_cueDxcSdkPath
    "C:/Program Files (x86)/Windows Kits/10/bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64"
)
find_program(CUE_DXC_EXECUTABLE NAMES dxc.exe dxc
    HINTS "${_cueDxcSdkPath}" "$ENV{WindowsSdkDir}/bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64"
    REQUIRED
)
set(_cueShaderSource "${CMAKE_CURRENT_SOURCE_DIR}/../../../Shader/FullscreenTriangle.hlsl")
set(_cueShaderOutputDir "${CMAKE_CURRENT_BINARY_DIR}/Generated")
foreach(_cueStage IN ITEMS VS PS)
    string(TOLOWER "${_cueStage}" _cueStageLower)
    set(_cueHeader "${_cueShaderOutputDir}/Fullscreen${_cueStage}.h")
    set(_cueBytecode "${_cueShaderOutputDir}/Fullscreen${_cueStage}.cso")
    add_custom_command(
        OUTPUT "${_cueHeader}" "${_cueBytecode}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${_cueShaderOutputDir}"
        COMMAND "${CUE_DXC_EXECUTABLE}" -T "${_cueStageLower}_6_0"
                -E "${_cueStage}Main" -WX
                -Fo "${_cueBytecode}" -Fh "${_cueHeader}"
                -Vn "g_fullscreen${_cueStage}" "${_cueShaderSource}"
        DEPENDS "${_cueShaderSource}"
        VERBATIM
    )
    target_sources(Cue.Renderer.DX12 PRIVATE "${_cueHeader}")
endforeach()
target_include_directories(Cue.Renderer.DX12 PRIVATE "${_cueShaderOutputDir}")

target_include_directories(Cue.Renderer.DX12 PUBLIC Public)
target_link_libraries(Cue.Renderer.DX12
    PUBLIC Cue.Foundation Cue.Renderer.FrameGraph Cue.Renderer.RHI
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
