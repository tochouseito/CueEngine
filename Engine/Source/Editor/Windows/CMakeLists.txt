add_library(Cue.Editor.Windows STATIC)

target_sources(
    Cue.Editor.Windows
    PRIVATE
        Private/EditorSession.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS Public
        FILES
            Public/Cue/Editor/Windows/EditorSession.h
)

target_link_libraries(
    Cue.Editor.Windows
    PUBLIC
        Cue.EditorCore
    PRIVATE
        Cue.Foundation.Windows
        Cue.IO.Windows
        Cue.Renderer
        bcrypt
)
target_compile_features(Cue.Editor.Windows PUBLIC cxx_std_20)
target_include_directories(Cue.Editor.Windows PUBLIC Public)
target_compile_definitions(Cue.Editor.Windows PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)

set_target_properties(
    Cue.Editor.Windows
    PROPERTIES
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib/$<CONFIG>"
        CXX_EXTENSIONS OFF
)

if(MSVC)
    target_compile_options(
        Cue.Editor.Windows
        PRIVATE
            /W4
            /WX
            /permissive-
            /Zc:__cplusplus
            /utf-8
            $<$<CONFIG:Development>:/O2>
            $<$<CONFIG:Development>:/Zi>
    )
    target_link_options(Cue.Editor.Windows PRIVATE $<$<CONFIG:Development>:/DEBUG>)
endif()
