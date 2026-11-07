# 保存 Metadata は Runtime / Game DLL / Editor の共通の所有値とする
add_library(Cue.ScriptData STATIC Private/ScriptData.cpp)
target_include_directories(Cue.ScriptData PUBLIC Public)
target_link_libraries(Cue.ScriptData PUBLIC Cue.Foundation)
target_compile_features(Cue.ScriptData PUBLIC cxx_std_20)
if(MSVC)
    target_compile_options(Cue.ScriptData PRIVATE /utf-8 $<$<CONFIG:Development>:/O2> $<$<CONFIG:Development>:/Zi>)
endif()
