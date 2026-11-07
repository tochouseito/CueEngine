# Game Code は Editor と異なる Process でだけ実行する
add_executable(CuePlayHost WIN32 Main.cpp)
target_link_libraries(CuePlayHost PRIVATE
    Cue.ScriptPlay Cue.GameRuntime Cue.WindowsHost Cue.Renderer.SceneRender Cue.Renderer.DX12
    Cue.Platform.Windows Cue.Foundation.Windows Cue.EngineInstallation shell32)
target_compile_features(CuePlayHost PRIVATE cxx_std_20)
target_compile_definitions(CuePlayHost PRIVATE CUE_REPOSITORY_ROOT="${PROJECT_SOURCE_DIR}")
set_target_properties(CuePlayHost PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
if(MSVC)
    target_compile_options(CuePlayHost PRIVATE /utf-8 $<$<CONFIG:Development>:/O2> $<$<CONFIG:Development>:/Zi>)
    target_link_options(CuePlayHost PRIVATE $<$<CONFIG:Development>:/DEBUG>)
endif()
cue_copy_shader_runtime(CuePlayHost)
