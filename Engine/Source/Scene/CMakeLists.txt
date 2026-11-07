# 保存用 Scene の正本は Runtime の Entity / GPU Resource と分離する
add_library(Cue.Scene STATIC Private/Scene.cpp)
target_include_directories(Cue.Scene PUBLIC Public)
target_link_libraries(Cue.Scene PUBLIC Cue.Math Cue.Assets Cue.ScriptData PRIVATE Cue.Platform)
target_compile_features(Cue.Scene PUBLIC cxx_std_20)
if(MSVC)
    target_compile_options(Cue.Scene PRIVATE /utf-8 $<$<CONFIG:Development>:/O2> $<$<CONFIG:Development>:/Zi>)
endif()
