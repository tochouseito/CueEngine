add_executable(CueRuntimeHost)

target_sources(
    CueRuntimeHost
    PRIVATE
        Main.cpp
        RuntimeHostApplication.cpp
        RuntimeHostApplication.h
)

target_link_libraries(
    CueRuntimeHost
    PRIVATE
        Cue.Foundation
        Cue.GameCore
        Cue.Input.Windows
        Cue.Platform.Windows
        Cue.RHI.D3D12.Windows
        Cue.Runtime
        Cue.Scene
        Cue.Schema
)
target_compile_features(CueRuntimeHost PRIVATE cxx_std_20)

if(BUILD_TESTING)
    target_link_libraries(CueRuntimeHost PRIVATE Cue.Platform.Windows.TestSupport)
    target_compile_definitions(CueRuntimeHost PRIVATE CUE_RUNTIME_RESIZE_SMOKE_SUPPORT=1)
endif()

target_compile_definitions(
    CueRuntimeHost
    PRIVATE
        $<$<CONFIG:Debug>:CUE_RUNTIME_GRAPHICS_DIAGNOSTICS_DEFAULT=1>
        $<$<CONFIG:Development>:CUE_RUNTIME_GRAPHICS_DIAGNOSTICS_DEFAULT=1>
        $<$<CONFIG:Release>:CUE_RUNTIME_GRAPHICS_DIAGNOSTICS_DEFAULT=0>
)

set_target_properties(
    CueRuntimeHost
    PROPERTIES
        CXX_EXTENSIONS OFF
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
)

if(MSVC)
    target_compile_options(
        CueRuntimeHost
        PRIVATE
            /W4
            /WX
            /permissive-
            /Zc:__cplusplus
            /utf-8
            $<$<CONFIG:Development>:/O2>
            $<$<CONFIG:Development>:/Zi>
    )

    target_link_options(
        CueRuntimeHost
        PRIVATE
            $<$<CONFIG:Development>:/DEBUG>
    )
endif()
