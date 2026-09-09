add_executable(CueEditorTool)

target_sources(
    CueEditorTool
    PRIVATE
        Main.cpp
)

target_link_libraries(
    CueEditorTool
    PRIVATE
        Cue.Editor.ImGui
        Cue.Editor.Windows
        Cue.Build.Windows
        Cue.Foundation.Windows
        Cue.ImGui.Core
        Cue.IO.Windows
        Cue.Platform.Windows
        Cue.ToolHost.WindowsD3D12
        bcrypt
)
target_compile_features(CueEditorTool PRIVATE cxx_std_20)
target_compile_definitions(CueEditorTool PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)

set_target_properties(
    CueEditorTool
    PROPERTIES
        CXX_EXTENSIONS OFF
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
)

if(MSVC)
    target_compile_options(
        CueEditorTool
        PRIVATE
            /W4
            /WX
            /permissive-
            /Zc:__cplusplus
            /utf-8
            $<$<CONFIG:Development>:/O2>
            $<$<CONFIG:Development>:/Zi>
    )
    target_link_options(CueEditorTool PRIVATE $<$<CONFIG:Development>:/DEBUG>)
endif()
