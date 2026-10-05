# DX12 固有の Device と Queue／Command Pool の作成・所有を独立した Module にする
# Pass の Shader 設定は FrameGraph Build 中に DXC でコンパイルする
file(GLOB CUE_DXC_CANDIDATES
    "$ENV{SystemDrive}/Program Files (x86)/Windows Kits/10/bin/*/x64/dxc.exe"
)
list(SORT CUE_DXC_CANDIDATES COMPARE NATURAL ORDER DESCENDING)
if(NOT CUE_DXC_CANDIDATES)
    message(FATAL_ERROR "Windows SDK dxc.exe is required for DX12 shaders")
endif()
list(GET CUE_DXC_CANDIDATES 0 CUE_DXC_EXECUTABLE)

get_filename_component(CUE_DXC_RUNTIME_DIR "${CUE_DXC_EXECUTABLE}" DIRECTORY)
get_filename_component(CUE_SHADER_SOURCE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../../../Shader" ABSOLUTE)
configure_file(Private/DX12ShaderConfig.h.in "${CMAKE_CURRENT_BINARY_DIR}/Generated/DX12ShaderConfig.h" @ONLY)
set_property(GLOBAL PROPERTY CUE_DXC_RUNTIME_DIR "${CUE_DXC_RUNTIME_DIR}")
set_property(GLOBAL PROPERTY CUE_SHADER_SOURCE_DIRECTORY "${CUE_SHADER_SOURCE_DIRECTORY}")

# 実行時の作業 Directory に依存せず DXC と HLSL を読み込めるよう実行 File の隣へ配置する
function(cue_copy_shader_runtime a_target)
    get_property(taskDxcDirectory GLOBAL PROPERTY CUE_DXC_RUNTIME_DIR)
    get_property(taskShaderDirectory GLOBAL PROPERTY CUE_SHADER_SOURCE_DIRECTORY)
    # 同じ Directory へ配置する Test 群は一つの Target にまとめる
    # HLSL だけの編集でも Build ごとに更新し、並列 Copy の競合を避ける
    add_custom_target(${a_target}.ShaderRuntime
        COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:${a_target}>"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${taskDxcDirectory}/dxcompiler.dll" "${taskDxcDirectory}/dxil.dll" "$<TARGET_FILE_DIR:${a_target}>"
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
    PUBLIC Cue.Foundation Cue.Renderer.FrameGraph Cue.Renderer.Passes Cue.Renderer.RHI
    PRIVATE Cue.Foundation.Windows Cue.Platform.Windows d3d12 dxgi dxguid
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
