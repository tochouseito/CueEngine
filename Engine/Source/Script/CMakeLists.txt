# Game SDK は ECS / Runtime / Native Platform を参照せず、C ABI を介して操作する
add_library(Cue.Script STATIC
    Private/GameObject.cpp
    Private/Module.cpp
    Private/ScriptTransform.cpp
)
target_include_directories(Cue.Script PUBLIC Public)
target_link_libraries(Cue.Script PUBLIC Cue.Math Cue.ScriptData)
target_compile_features(Cue.Script PUBLIC cxx_std_20)
target_compile_definitions(Cue.Script PRIVATE
    $<$<CONFIG:Debug>:CUE_SCRIPT_CONFIGURATION=1>
    $<$<CONFIG:Development>:CUE_SCRIPT_CONFIGURATION=2>
    $<$<CONFIG:Release>:CUE_SCRIPT_CONFIGURATION=3>
)
if(MSVC)
    target_compile_options(Cue.Script PRIVATE /utf-8 $<$<CONFIG:Development>:/O2> $<$<CONFIG:Development>:/Zi>)
endif()
