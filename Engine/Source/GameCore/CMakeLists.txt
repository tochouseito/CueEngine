add_library(Cue.GameCore STATIC)

target_sources(
    Cue.GameCore
    PRIVATE
        Private/Clock.cpp
        Private/ClockInternals.h
        Private/CommandBuffer.cpp
        Private/Error.cpp
        Private/RuntimeWorld.cpp
        Private/World.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS Public
        FILES
            Public/Cue/GameCore/Clock.h
            Public/Cue/GameCore/CommandBuffer.h
            Public/Cue/GameCore/Entity.h
            Public/Cue/GameCore/Error.h
            Public/Cue/GameCore/RuntimeWorld.h
            Public/Cue/GameCore/UpdateContext.h
            Public/Cue/GameCore/World.h
)

target_compile_features(Cue.GameCore PUBLIC cxx_std_20)
target_link_libraries(Cue.GameCore PUBLIC Cue.Foundation Cue.Math Cue.Schema)
target_include_directories(Cue.GameCore PUBLIC Public)

set_target_properties(
    Cue.GameCore
    PROPERTIES
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib/$<CONFIG>"
        CXX_EXTENSIONS OFF
)

if(MSVC)
    target_compile_options(
        Cue.GameCore
        PRIVATE
            /W4
            /WX
            /permissive-
            /Zc:__cplusplus
            /utf-8
            $<$<CONFIG:Development>:/O2>
            $<$<CONFIG:Development>:/Zi>
    )
    target_link_options(Cue.GameCore PRIVATE $<$<CONFIG:Development>:/DEBUG>)
endif()

get_target_property(cueGameCoreLinks Cue.GameCore LINK_LIBRARIES)
get_target_property(cueGameCoreInterfaceLinks Cue.GameCore INTERFACE_LINK_LIBRARIES)

if(NOT "${cueGameCoreLinks}" STREQUAL "Cue.Foundation;Cue.Math;Cue.Schema")
    message(FATAL_ERROR "Cue.GameCore must directly link only Cue.Foundation, Cue.Math, and Cue.Schema: ${cueGameCoreLinks}")
endif()

if(NOT "${cueGameCoreInterfaceLinks}" STREQUAL "Cue.Foundation;Cue.Math;Cue.Schema")
    message(FATAL_ERROR "Cue.GameCore must expose only Cue.Foundation, Cue.Math, and Cue.Schema: ${cueGameCoreInterfaceLinks}")
endif()
