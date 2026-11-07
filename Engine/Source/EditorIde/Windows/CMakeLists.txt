# Native Process の所有を Windows Adapter に閉じ込める
add_library(Cue.EditorIde.Windows STATIC Private/IdeProcess.cpp Private/WindowsIdeService.cpp)
target_include_directories(Cue.EditorIde.Windows PUBLIC Public)
target_link_libraries(Cue.EditorIde.Windows PUBLIC Cue.EditorIde PRIVATE Cue.Platform.Windows Cue.Foundation.Windows)
target_compile_features(Cue.EditorIde.Windows PUBLIC cxx_std_20)
if(MSVC)
    target_compile_options(Cue.EditorIde.Windows PRIVATE /utf-8)
endif()

# Framework 4.8 の既存 Reference Assemblies だけを使い、NuGet Restore を起動しない
if(NOT CMAKE_VS_MSBUILD_COMMAND)
    message(FATAL_ERROR "VisualStudioBridge requires the configured Visual Studio MSBuild")
endif()
set(taskBridgeRoot "${PROJECT_SOURCE_DIR}/Tools/VisualStudioBridge")
set(taskBridgeOutput "${CMAKE_CURRENT_BINARY_DIR}/Bridge/$<CONFIG>")
add_custom_target(Cue.VisualStudioBridge
    COMMAND "${CMAKE_VS_MSBUILD_COMMAND}" "${taskBridgeRoot}/VisualStudioBridge.csproj"
        /nologo /verbosity:minimal "/p:Configuration=$<CONFIG>" /p:Platform=AnyCPU
        "/p:OutputPath=${taskBridgeOutput}/"
        "/p:IntermediateOutputPath=${CMAKE_CURRENT_BINARY_DIR}/BridgeIntermediate/$<CONFIG>/"
    BYPRODUCTS "${taskBridgeOutput}/VisualStudioBridge.exe" "${taskBridgeOutput}/VisualStudioBridge.exe.config"
    VERBATIM
)
set_property(GLOBAL PROPERTY CUE_VISUAL_STUDIO_BRIDGE_EXE "${taskBridgeOutput}/VisualStudioBridge.exe")
set_property(GLOBAL PROPERTY CUE_VISUAL_STUDIO_BRIDGE_CONFIG "${taskBridgeOutput}/VisualStudioBridge.exe.config")

# Editor と起動 Tool の配置を一致させる。製品 Target からは依存させない
function(cue_copy_visual_studio_bridge a_target)
    get_property(taskBridge GLOBAL PROPERTY CUE_VISUAL_STUDIO_BRIDGE_EXE)
    get_property(taskConfig GLOBAL PROPERTY CUE_VISUAL_STUDIO_BRIDGE_CONFIG)
    add_custom_target(${a_target}.VisualStudioBridgeRuntime
        COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:${a_target}>"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${taskBridge}" "$<TARGET_FILE_DIR:${a_target}>"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${taskConfig}" "$<TARGET_FILE_DIR:${a_target}>"
        VERBATIM
    )
    add_dependencies(${a_target}.VisualStudioBridgeRuntime Cue.VisualStudioBridge)
    add_dependencies(${a_target} ${a_target}.VisualStudioBridgeRuntime)
endfunction()
