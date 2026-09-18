add_library(Cue.Renderer STATIC)

target_sources(
    Cue.Renderer
    PRIVATE
        Private/Camera.cpp
        Private/Error.cpp
        Private/RenderExtraction.cpp
        Private/RendererRuntimeSystem.cpp
        Private/RendererSchema.cpp
        Private/RenderSnapshot.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS Public
        FILES
            Public/Cue/Renderer/Camera.h
            Public/Cue/Renderer/Error.h
            Public/Cue/Renderer/RenderExtraction.h
            Public/Cue/Renderer/RendererRuntimeSystem.h
            Public/Cue/Renderer/RendererSchema.h
            Public/Cue/Renderer/RenderSnapshot.h
)

target_compile_features(Cue.Renderer PUBLIC cxx_std_20)
target_link_libraries(
    Cue.Renderer
    PUBLIC
        Cue.EngineAssets
        Cue.Foundation
        Cue.GameCore
        Cue.Math
        Cue.Runtime
        Cue.Scene
        Cue.Schema
)
target_include_directories(Cue.Renderer PUBLIC Public)

set_target_properties(
    Cue.Renderer
    PROPERTIES
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib/$<CONFIG>"
        CXX_EXTENSIONS OFF
)

if(MSVC)
    target_compile_options(
        Cue.Renderer
        PRIVATE
            /W4
            /WX
            /permissive-
            /Zc:__cplusplus
            /utf-8
            $<$<CONFIG:Development>:/O2>
            $<$<CONFIG:Development>:/Zi>
    )
    target_link_options(Cue.Renderer PRIVATE $<$<CONFIG:Development>:/DEBUG>)
endif()

get_target_property(cueRendererLinks Cue.Renderer LINK_LIBRARIES)
get_target_property(cueRendererInterfaceLinks Cue.Renderer INTERFACE_LINK_LIBRARIES)
set(cueRendererExpectedLinks Cue.EngineAssets Cue.Foundation Cue.GameCore Cue.Math Cue.Runtime Cue.Scene Cue.Schema)

if(NOT "${cueRendererLinks}" STREQUAL "${cueRendererExpectedLinks}")
    message(FATAL_ERROR "Cue.Renderer has unexpected direct dependencies: ${cueRendererLinks}")
endif()

if(NOT "${cueRendererInterfaceLinks}" STREQUAL "${cueRendererExpectedLinks}")
    message(FATAL_ERROR "Cue.Renderer has unexpected public dependencies: ${cueRendererInterfaceLinks}")
endif()
