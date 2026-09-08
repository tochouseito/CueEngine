add_library(Cue.Runtime STATIC)

target_sources(
    Cue.Runtime
    PRIVATE
        Private/Error.cpp
        Private/RuntimeApplicationSession.cpp
        Private/RuntimeSceneSession.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS Public
        FILES
            Public/Cue/Runtime/Error.h
            Public/Cue/Runtime/RuntimeApplicationSession.h
            Public/Cue/Runtime/RuntimeSceneSession.h
)

target_compile_features(Cue.Runtime PUBLIC cxx_std_20)
target_link_libraries(Cue.Runtime PUBLIC Cue.Foundation Cue.GameCore Cue.Scene Cue.Schema Cue.Input)
target_include_directories(Cue.Runtime PUBLIC Public)

set_target_properties(
    Cue.Runtime
    PROPERTIES
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib/$<CONFIG>"
        CXX_EXTENSIONS OFF
)

if(MSVC)
    target_compile_options(
        Cue.Runtime
        PRIVATE
            /W4
            /WX
            /permissive-
            /Zc:__cplusplus
            /utf-8
            $<$<CONFIG:Development>:/O2>
            $<$<CONFIG:Development>:/Zi>
    )
    target_link_options(Cue.Runtime PRIVATE $<$<CONFIG:Development>:/DEBUG>)
endif()

get_target_property(cueRuntimeLinks Cue.Runtime LINK_LIBRARIES)
get_target_property(cueRuntimeInterfaceLinks Cue.Runtime INTERFACE_LINK_LIBRARIES)

if(NOT "${cueRuntimeLinks}" STREQUAL "Cue.Foundation;Cue.GameCore;Cue.Scene;Cue.Schema;Cue.Input")
    message(FATAL_ERROR "Cue.Runtime must directly link only Cue.Foundation, Cue.GameCore, Cue.Scene, Cue.Schema, and Cue.Input: ${cueRuntimeLinks}")
endif()

if(NOT "${cueRuntimeInterfaceLinks}" STREQUAL "Cue.Foundation;Cue.GameCore;Cue.Scene;Cue.Schema;Cue.Input")
    message(FATAL_ERROR "Cue.Runtime must expose only Cue.Foundation, Cue.GameCore, Cue.Scene, Cue.Schema, and Cue.Input: ${cueRuntimeInterfaceLinks}")
endif()
