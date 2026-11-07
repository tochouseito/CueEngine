# World からの抽出を Renderer の所有値へ変換し、具体 GPU API は参照しない
add_library(Cue.Renderer.SceneRender STATIC Private/RenderScene.cpp)
target_include_directories(Cue.Renderer.SceneRender PUBLIC Public)
target_link_libraries(Cue.Renderer.SceneRender PUBLIC Cue.Math Cue.Renderer.RHI Cue.Renderer.FrameGraph PRIVATE Cue.Runtime)
target_compile_features(Cue.Renderer.SceneRender PUBLIC cxx_std_20)
if(MSVC)
    target_compile_options(Cue.Renderer.SceneRender PRIVATE /utf-8 $<$<CONFIG:Development>:/O2> $<$<CONFIG:Development>:/Zi>)
endif()
