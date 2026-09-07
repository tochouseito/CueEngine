add_library(Cue.IO.Windows STATIC)

target_sources(
    Cue.IO.Windows
    PRIVATE
        Private/WindowsFilesystem.cpp
        Private/WindowsFilesystemIdentity.cpp
        Private/WindowsWorkspaceFilesystem.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS Public
        FILES
            Public/Cue/IO/Windows/WindowsFilesystem.h
            Public/Cue/IO/Windows/WindowsWorkspaceFilesystem.h
)

target_link_libraries(
    Cue.IO.Windows
    PUBLIC
        Cue.IO
    PRIVATE
        Cue.Foundation.Windows
        bcrypt
        Ole32
        Shell32
)

target_compile_features(Cue.IO.Windows PUBLIC cxx_std_20)
target_include_directories(Cue.IO.Windows PUBLIC Public)
target_compile_definitions(Cue.IO.Windows PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)

set_target_properties(
    Cue.IO.Windows
    PROPERTIES
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib/$<CONFIG>"
        CXX_EXTENSIONS OFF
)

if(MSVC)
    target_compile_options(
        Cue.IO.Windows
        PRIVATE
            /W4
            /WX
            /permissive-
            /Zc:__cplusplus
            /utf-8
            $<$<CONFIG:Development>:/O2>
            $<$<CONFIG:Development>:/Zi>
    )

    target_link_options(Cue.IO.Windows PRIVATE $<$<CONFIG:Development>:/DEBUG>)
endif()
