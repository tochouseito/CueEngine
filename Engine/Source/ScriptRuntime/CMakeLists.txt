# C ABI と World を接続する。ゲーム SDK から Runtime への逆依存を作らない
add_library(Cue.ScriptRuntime STATIC Private/ScriptModule.cpp Private/ScriptSystem.cpp)
target_include_directories(Cue.ScriptRuntime PUBLIC Public)
target_link_libraries(Cue.ScriptRuntime PUBLIC Cue.Runtime Cue.ScriptData Cue.Script Cue.Scene)
target_compile_features(Cue.ScriptRuntime PUBLIC cxx_std_20)
target_compile_definitions(Cue.ScriptRuntime PRIVATE
    $<$<CONFIG:Debug>:CUE_SCRIPT_CONFIGURATION=1>
    $<$<CONFIG:Development>:CUE_SCRIPT_CONFIGURATION=2>
    $<$<CONFIG:Release>:CUE_SCRIPT_CONFIGURATION=3>
)
if(WIN32)
    target_link_libraries(Cue.ScriptRuntime PRIVATE Cue.Foundation.Windows)
endif()
if(MSVC)
    target_compile_options(Cue.ScriptRuntime PRIVATE /utf-8 $<$<CONFIG:Development>:/O2> $<$<CONFIG:Development>:/Zi>)
endif()
