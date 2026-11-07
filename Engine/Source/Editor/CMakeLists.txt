# Scene 編集の正本と Preview 接続を Native UI / GPU から分離する
add_library(Cue.Editor STATIC Private/SceneDocument.cpp Private/EditorApplication.cpp)
target_include_directories(Cue.Editor PUBLIC Public)
target_link_libraries(Cue.Editor PUBLIC Cue.Scene Cue.Platform Cue.Runtime PRIVATE Cue.SceneRuntime)
target_compile_features(Cue.Editor PUBLIC cxx_std_20)
if(MSVC)
    target_compile_options(Cue.Editor PRIVATE /utf-8 $<$<CONFIG:Development>:/O2> $<$<CONFIG:Development>:/Zi>)
endif()
