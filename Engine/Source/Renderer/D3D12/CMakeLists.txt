# D3D12資源とWindows PresentationをWindows専用Targetに閉じ込める
add_library(Cue.Renderer.D3D12 STATIC
    Private/D3D12CommandPool.cpp
    Private/D3D12DeviceContext.cpp
    Private/D3D12GraphExecutor.cpp
    Private/D3D12PipelineCache.cpp
    Private/D3D12SurfacePool.cpp
    Private/D3D12StaticMeshPool.cpp
    Private/D3D12TrianglePass.cpp
    Private/D3D12Presentation.cpp
    Private/D3D12QueueContext.cpp
    Private/D3D12Renderer.cpp
    Private/D3D12ViewManager.cpp
)

# Shader の変更時は CMake が再構成し、実行時に参照する Build Directory へ配置する
configure_file(${CMAKE_SOURCE_DIR}/Engine/Shader/FixedMesh.hlsl
    ${CMAKE_CURRENT_BINARY_DIR}/Shader/FixedMesh.hlsl COPYONLY)
configure_file(Private/FixedMeshShaderPath.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/Generated/FixedMeshShaderPath.h @ONLY)

target_include_directories(Cue.Renderer.D3D12 PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}/Generated
)

target_include_directories(Cue.Renderer.D3D12 PUBLIC
    Public
)

target_link_libraries(Cue.Renderer.D3D12
    PUBLIC Cue.Platform
    PRIVATE Cue.Renderer.FrameGraph d3d12 d3dcompiler dxgi dxguid
)

target_compile_features(Cue.Renderer.D3D12 PUBLIC cxx_std_20)

if(MSVC)
    target_compile_options(Cue.Renderer.D3D12 PRIVATE
        /utf-8
        $<$<CONFIG:Development>:/O2>
        $<$<CONFIG:Development>:/Zi>
    )
endif()

target_compile_definitions(Cue.Renderer.D3D12 PRIVATE
    $<$<CONFIG:Release>:CUE_SHIPPING=1>
)
