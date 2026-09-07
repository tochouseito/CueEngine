add_library(Cue.Input.Windows STATIC)

target_sources(
    Cue.Input.Windows
    PRIVATE
        Private/WindowsInputMessageSink.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS Public
        FILES
            Public/Cue/Input/Windows/WindowsInputMessageSink.h
)

target_link_libraries(
    Cue.Input.Windows
    PUBLIC
        Cue.Input
        Cue.Platform.Windows
    PRIVATE
        User32
)
target_compile_features(Cue.Input.Windows PUBLIC cxx_std_20)
target_include_directories(Cue.Input.Windows PUBLIC Public)

target_compile_definitions(
    Cue.Input.Windows
    PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
)

set_target_properties(
    Cue.Input.Windows
    PROPERTIES
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib/$<CONFIG>"
        CXX_EXTENSIONS OFF
)

if(MSVC)
    target_compile_options(
        Cue.Input.Windows
        PRIVATE
            /W4
            /WX
            /permissive-
            /Zc:__cplusplus
            /utf-8
            $<$<CONFIG:Development>:/O2>
            $<$<CONFIG:Development>:/Zi>
    )
    target_link_options(Cue.Input.Windows PRIVATE $<$<CONFIG:Development>:/DEBUG>)
endif()
