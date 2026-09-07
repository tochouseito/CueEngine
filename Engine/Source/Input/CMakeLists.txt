add_library(Cue.Input STATIC)

target_sources(
    Cue.Input
    PRIVATE
        Private/InputEventQueue.cpp
        Private/InputState.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS Public
        FILES
            Public/Cue/Input/FrameInputSnapshot.h
            Public/Cue/Input/InputEvent.h
            Public/Cue/Input/InputEventQueue.h
            Public/Cue/Input/InputState.h
)

target_compile_features(Cue.Input PUBLIC cxx_std_20)
target_include_directories(Cue.Input PUBLIC Public)

set_target_properties(
    Cue.Input
    PROPERTIES
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib/$<CONFIG>"
        CXX_EXTENSIONS OFF
)

if(MSVC)
    target_compile_options(
        Cue.Input
        PRIVATE
            /W4
            /WX
            /permissive-
            /Zc:__cplusplus
            /utf-8
            $<$<CONFIG:Development>:/O2>
            $<$<CONFIG:Development>:/Zi>
    )
    target_link_options(Cue.Input PRIVATE $<$<CONFIG:Development>:/DEBUG>)
endif()

if(WIN32)
    add_subdirectory(Windows)
endif()
