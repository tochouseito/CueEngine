add_library(Cue.Platform STATIC)

target_sources(
    Cue.Platform
    PRIVATE
        Private/FileDialog.cpp
        Private/Window.cpp
        Private/WindowSystem.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS Public
        FILES
            Public/Cue/Platform/FileDialog.h
            Public/Cue/Platform/SystemCapabilities.h
            Public/Cue/Platform/Window.h
            Public/Cue/Platform/WindowEvent.h
            Public/Cue/Platform/WindowSystem.h
)

target_link_libraries(Cue.Platform PUBLIC Cue.Foundation)
target_compile_features(Cue.Platform PUBLIC cxx_std_20)

target_include_directories(
    Cue.Platform
    PUBLIC
        Public
    PRIVATE
        Private
)

set_target_properties(
    Cue.Platform
    PROPERTIES
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib/$<CONFIG>"
        CXX_EXTENSIONS OFF
)

if(MSVC)
    target_compile_options(
        Cue.Platform
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
        Cue.Platform
        PRIVATE
            $<$<CONFIG:Development>:/DEBUG>
    )
endif()

if(WIN32)
    add_subdirectory(Windows)
endif()
