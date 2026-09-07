add_library(Cue.IO STATIC)

target_sources(
    Cue.IO
    PRIVATE
        Private/Error.cpp
        Private/Filesystem.cpp
        Private/RelativePath.cpp
        Private/WorkspaceFilesystem.cpp
        Private/WorkspaceWatcher.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS Public
        FILES
            Public/Cue/IO/Error.h
            Public/Cue/IO/Filesystem.h
            Public/Cue/IO/RelativePath.h
            Public/Cue/IO/WorkspaceFilesystem.h
            Public/Cue/IO/WorkspaceWatcher.h
)

target_link_libraries(Cue.IO PUBLIC Cue.Foundation)
target_compile_features(Cue.IO PUBLIC cxx_std_20)

target_include_directories(Cue.IO PUBLIC Public)

set_target_properties(
    Cue.IO
    PROPERTIES
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib/$<CONFIG>"
        CXX_EXTENSIONS OFF
)

if(MSVC)
    target_compile_options(
        Cue.IO
        PRIVATE
            /W4
            /WX
            /permissive-
            /Zc:__cplusplus
            /utf-8
            $<$<CONFIG:Development>:/O2>
            $<$<CONFIG:Development>:/Zi>
    )

    target_link_options(Cue.IO PRIVATE $<$<CONFIG:Development>:/DEBUG>)
endif()

if(WIN32)
    add_subdirectory(Windows)
endif()
