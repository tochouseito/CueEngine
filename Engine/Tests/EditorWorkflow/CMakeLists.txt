add_executable(CueEditorWorkflowProcessTests EditorWorkflowProcessTests.cpp)
target_link_libraries(
    CueEditorWorkflowProcessTests
    PRIVATE
        Cue.Editor.Windows
        Cue.Foundation.Windows
        Cue.IO.Windows
        Cue.Project
)
target_compile_features(CueEditorWorkflowProcessTests PRIVATE cxx_std_20)
target_compile_definitions(
    CueEditorWorkflowProcessTests
    PRIVATE
        $<$<CONFIG:Debug>:CUE_TEST_BUILD_CONFIGURATION=1>
        $<$<CONFIG:Development>:CUE_TEST_BUILD_CONFIGURATION=2>
        $<$<CONFIG:Release>:CUE_TEST_BUILD_CONFIGURATION=3>
)

set_target_properties(
    CueEditorWorkflowProcessTests
    PROPERTIES
        CXX_EXTENSIONS OFF
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
)

if(MSVC)
    target_compile_options(
        CueEditorWorkflowProcessTests
        PRIVATE
            /W4
            /WX
            /permissive-
            /Zc:__cplusplus
            /utf-8
            $<$<CONFIG:Development>:/O2>
            $<$<CONFIG:Development>:/Zi>
    )
    target_link_options(CueEditorWorkflowProcessTests PRIVATE $<$<CONFIG:Development>:/DEBUG>)
endif()

add_custom_target(Cue.Editor.Workflow.Tests DEPENDS CueEditorWorkflowProcessTests)
add_dependencies(CueEditorWorkflowProcessTests CueEditorTool CueRuntimeHost)

get_filename_component(cueEditorWorkflowWorkspace "${CMAKE_SOURCE_DIR}/../.cew" ABSOLUTE)
set(cueEditorWorkflowConfiguration "$<IF:$<CONFIG:Debug>,d,$<IF:$<CONFIG:Development>,v,r>>")
add_test(
    NAME Cue.Editor.Workflow.ProcessRoundTrip
    COMMAND
        CueEditorWorkflowProcessTests
        $<TARGET_FILE:CueEditorTool>
        # MSBuild FileTracker still uses MAX_PATH for compiler-identification logs.
        "${cueEditorWorkflowWorkspace}/${cueEditorWorkflowConfiguration}"
)
set_tests_properties(
    Cue.Editor.Workflow.ProcessRoundTrip
    PROPERTIES
        LABELS "Editor;Process"
        RUN_SERIAL TRUE
        TIMEOUT 660
)
