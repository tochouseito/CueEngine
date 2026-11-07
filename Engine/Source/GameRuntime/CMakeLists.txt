# Play / 製品の実行 World と配布設定は Platform Host より下位に置く
add_library(Cue.GameRuntime STATIC Private/GameSession.cpp Private/GameStartup.cpp)
target_include_directories(Cue.GameRuntime PUBLIC Public)
target_link_libraries(Cue.GameRuntime PUBLIC Cue.SceneRuntime Cue.ScriptRuntime Cue.Renderer.SceneRender PRIVATE Cue.Platform)
target_compile_features(Cue.GameRuntime PUBLIC cxx_std_20)
if(MSVC)
    target_compile_options(Cue.GameRuntime PRIVATE /utf-8 $<$<CONFIG:Development>:/O2> $<$<CONFIG:Development>:/Zi>)
endif()
