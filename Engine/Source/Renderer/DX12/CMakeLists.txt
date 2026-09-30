# DX12資源とWindows PresentationをWindows専用Targetに閉じ込める
add_library(Cue.Renderer.DX12 STATIC
    Private/BackendFactory.cpp
    Private/DescriptorAllocator.cpp
    Private/DX12BufferManager.cpp
    Private/HLSLCompiler.cpp
    Private/DX12CommandRecorder.cpp
    Private/DX12FramePasses.cpp
    Private/DX12CommandPool.cpp
    Private/DX12RenderDevice.cpp
    Private/DX12GraphExecutor.cpp
    Private/DX12GraphResourceBindings.cpp
    Private/DX12PipelineCache.cpp
    Private/DX12PipelineManager.cpp
    Private/DX12SurfacePool.cpp
    Private/DX12StaticMeshPool.cpp
    Private/DX12TrianglePass.cpp
    Private/DX12TextureManager.cpp
    Private/DX12SwapChain.cpp
    Private/DX12GpuCommandQueue.cpp
    Private/DX12QueuePool.cpp
    Private/DX12ResourcePool.cpp
    Private/DX12Backend.cpp
    Private/DX12ViewManager.cpp
)

# Shader の変更時は CMake が再構成し、実行時に参照する Build Directory へ配置する
configure_file(${CMAKE_SOURCE_DIR}/Engine/Shader/FixedMesh.hlsl
    ${CMAKE_CURRENT_BINARY_DIR}/Shader/FixedMesh.hlsl COPYONLY)
configure_file(Private/FixedMeshShaderPath.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/Generated/FixedMeshShaderPath.h @ONLY)

target_include_directories(Cue.Renderer.DX12 PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}/Generated
)

target_include_directories(Cue.Renderer.DX12 PUBLIC
    Public
)

target_link_libraries(Cue.Renderer.DX12
    PUBLIC Cue.Platform Cue.Renderer.RHI
    PRIVATE Cue.Foundation.Windows Cue.Renderer.FrameGraph d3d12 d3dcompiler dxcompiler dxgi dxguid
)

# DXC の Import Library は Windows SDK から Link し、同じ SDK の実行時 DLL を配置する
find_file(CUE_DXC_COMPILER_DLL dxcompiler.dll REQUIRED
    PATHS "$ENV{WindowsSdkDir}/bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64"
          "C:/Program Files (x86)/Windows Kits/10/bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64"
    NO_DEFAULT_PATH)
find_file(CUE_DXC_VALIDATOR_DLL dxil.dll REQUIRED
    PATHS "$ENV{WindowsSdkDir}/bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64"
          "C:/Program Files (x86)/Windows Kits/10/bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64"
    NO_DEFAULT_PATH)

function(cue_stage_dxc_runtime a_target)
    add_custom_command(TARGET ${a_target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CUE_DXC_COMPILER_DLL}" "$<TARGET_FILE_DIR:${a_target}>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CUE_DXC_VALIDATOR_DLL}" "$<TARGET_FILE_DIR:${a_target}>"
        VERBATIM)
endfunction()

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
