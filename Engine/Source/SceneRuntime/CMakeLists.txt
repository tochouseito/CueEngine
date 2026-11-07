# 保存用 Scene と実行 World の接続を独立させ、両者から逆依存させない
add_library(Cue.SceneRuntime STATIC Private/SceneInstantiation.cpp)
target_include_directories(Cue.SceneRuntime PUBLIC Public)
target_link_libraries(Cue.SceneRuntime PUBLIC Cue.Scene Cue.Runtime)
target_compile_features(Cue.SceneRuntime PUBLIC cxx_std_20)
if(MSVC)
    target_compile_options(Cue.SceneRuntime PRIVATE /utf-8 $<$<CONFIG:Development>:/O2> $<$<CONFIG:Development>:/Zi>)
endif()
