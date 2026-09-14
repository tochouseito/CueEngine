add_executable(CueProjectHubTool)

target_sources(
    CueProjectHubTool
    PRIVATE
        Main.cpp
        Resources/CueProjectHubTool.rc
        Resources/CueProjectHubTool.ico
        Resources/CueProjectHubToolIcon.png
)

# 原本 PNG は編集用 Source として保持し、Build 時の画像変換 Tool 依存を避けるため ICO も追跡する
set_source_files_properties(
    Resources/CueProjectHubTool.ico
    Resources/CueProjectHubToolIcon.png
    PROPERTIES
        HEADER_FILE_ONLY TRUE
)

target_link_libraries(
    CueProjectHubTool
    PRIVATE
        Cue.Foundation.Windows
        Cue.ImGui.Core
        Cue.IO.Windows
        Cue.ProjectHub.ImGui
        Cue.ProjectHub.Windows
        Cue.ToolHost.WindowsD3D12
)
target_compile_features(CueProjectHubTool PRIVATE cxx_std_20)
target_compile_definitions(CueProjectHubTool PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)

set_target_properties(
    CueProjectHubTool
    PROPERTIES
        CXX_EXTENSIONS OFF
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
)

if(MSVC)
    target_compile_options(
        CueProjectHubTool
        PRIVATE
            /W4
            /WX
            /permissive-
            /Zc:__cplusplus
            /utf-8
            $<$<CONFIG:Development>:/O2>
            $<$<CONFIG:Development>:/Zi>
    )
    target_link_options(CueProjectHubTool PRIVATE $<$<CONFIG:Development>:/DEBUG>)
endif()
