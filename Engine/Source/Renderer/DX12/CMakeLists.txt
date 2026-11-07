# DX12 固有の Device と Queue／Command Pool の作成・所有を独立した Module にする
include("${PROJECT_SOURCE_DIR}/ThirdParty/WinPixEventRuntime.cmake")
# Pass の Shader 設定は再配布条件を保持した固定 DXC Package でコンパイルする
include("${PROJECT_SOURCE_DIR}/ThirdParty/DirectXShaderCompiler.cmake")
get_filename_component(CUE_SHADER_SOURCE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../../../Shader" ABSOLUTE)
if(CUE_DISTRIBUTION_BUILD)
    set(CUE_SHADER_ALLOW_DEVELOPMENT_FALLBACK false)
else()
    set(CUE_SHADER_ALLOW_DEVELOPMENT_FALLBACK true)
endif()
configure_file(Private/DX12ShaderConfig.h.in "${CMAKE_CURRENT_BINARY_DIR}/Generated/DX12ShaderConfig.h" @ONLY)
set_property(GLOBAL PROPERTY CUE_DXC_RUNTIME_DIR "${CUE_DXC_RUNTIME_DIR}")
set_property(GLOBAL PROPERTY CUE_SHADER_SOURCE_DIRECTORY "${CUE_SHADER_SOURCE_DIRECTORY}")

# 実行時の作業 Directory に依存せず DXC と HLSL を読み込めるよう実行 File の隣へ配置する
function(cue_copy_shader_runtime a_target)
    get_property(taskDxcDirectory GLOBAL PROPERTY CUE_DXC_RUNTIME_DIR)
    get_property(taskDxcPackage GLOBAL PROPERTY CUE_DXC_PACKAGE_DIRECTORY)
    get_property(taskShaderDirectory GLOBAL PROPERTY CUE_SHADER_SOURCE_DIRECTORY)
    # 同じ Directory へ配置する Test 群は一つの Target にまとめる
    # HLSL だけの編集でも Build ごとに更新し、並列 Copy の競合を避ける
    add_custom_target(${a_target}.ShaderRuntime
        COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:${a_target}>"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${taskDxcDirectory}/dxcompiler.dll" "${taskDxcDirectory}/dxil.dll" "$<TARGET_FILE_DIR:${a_target}>"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${taskDxcPackage}/LICENSE-MS.txt" "$<TARGET_FILE_DIR:${a_target}>/DirectXShaderCompiler-MS.txt"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${taskDxcPackage}/LICENSE-LLVM.txt" "$<TARGET_FILE_DIR:${a_target}>/DirectXShaderCompiler-LLVM.txt"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${taskDxcPackage}/LICENCE-MIT.txt" "$<TARGET_FILE_DIR:${a_target}>/DirectXShaderCompiler-MIT.txt"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${taskDxcPackage}/inc/hlsl/LICENCE.txt" "$<TARGET_FILE_DIR:${a_target}>/DirectXShaderCompiler-HLSL.txt"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory_if_different
                "${taskShaderDirectory}" "$<TARGET_FILE_DIR:${a_target}>/EngineResources/Shader"
        VERBATIM
    )
    foreach(taskTarget IN ITEMS ${a_target} ${ARGN})
        add_dependencies(${taskTarget} ${a_target}.ShaderRuntime)
    endforeach()
endfunction()

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
    Private/DX12MeshPool.cpp
    Private/DX12SceneRenderer.cpp
    Private/DX12PlacedResourceAllocator.cpp
    Private/DX12RenderDevice.cpp
    Private/DX12QueuePool.cpp
    Private/DX12ResourceLeakChecker.cpp
    Private/DX12ViewManager.cpp
    Private/DX12SwapChain.cpp
    Private/DX12PipelineManager.cpp
    Private/DX12ShaderCompiler.cpp
)

target_include_directories(Cue.Renderer.DX12 PUBLIC Public)
target_include_directories(Cue.Renderer.DX12 PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/Generated")
target_link_libraries(Cue.Renderer.DX12
    PUBLIC Cue.Foundation Cue.Profiler Cue.Renderer.FrameGraph Cue.Renderer.Passes Cue.Renderer.RHI Cue.Renderer.SceneRender
    PRIVATE Cue.Foundation.Windows Cue.Platform.Windows Cue.WinPixEventRuntime d3d12 dxgi dxguid
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
