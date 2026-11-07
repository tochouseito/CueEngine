# UI と独立した Tool は JSON 応答を stdout へ返し、Hub から別 Process として呼ぶ
add_executable(CueProjectTool Main.cpp)
target_link_libraries(CueProjectTool PRIVATE Cue.ProjectTools Cue.Project Cue.EngineInstallation Cue.Platform.Windows Cue.Foundation.Windows ole32)
target_compile_features(CueProjectTool PRIVATE cxx_std_20)
target_compile_definitions(CueProjectTool PRIVATE CUE_PROJECT_TEMPLATES="${PROJECT_SOURCE_DIR}/Templates")
set_target_properties(CueProjectTool PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
if(MSVC)
    target_compile_options(CueProjectTool PRIVATE /utf-8
        $<$<CONFIG:Development>:/O2> $<$<CONFIG:Development>:/Zi>)
    target_link_options(CueProjectTool PRIVATE $<$<CONFIG:Development>:/DEBUG>)
endif()
