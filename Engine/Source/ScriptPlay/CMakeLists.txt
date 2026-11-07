# Process と IPC の具体実装は Windows に閉じ、Editor は所有値の API だけを利用する
add_library(Cue.ScriptPlay STATIC Private/PlayTransport.cpp)
target_include_directories(Cue.ScriptPlay PUBLIC Public)
target_link_libraries(Cue.ScriptPlay PUBLIC Cue.ScriptData Cue.Scene PRIVATE Cue.Foundation.Windows)
target_compile_features(Cue.ScriptPlay PUBLIC cxx_std_20)
if(MSVC)
    target_compile_options(Cue.ScriptPlay PRIVATE /utf-8 $<$<CONFIG:Development>:/O2> $<$<CONFIG:Development>:/Zi>)
endif()
