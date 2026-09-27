# Resource と Pass の計画は Platform 非依存 Target に置く
add_library(Cue.Renderer.FrameGraph STATIC
    Private/FrameGraph.cpp
)

target_include_directories(Cue.Renderer.FrameGraph PUBLIC Public)
target_link_libraries(Cue.Renderer.FrameGraph PUBLIC Cue.Foundation)
target_compile_features(Cue.Renderer.FrameGraph PUBLIC cxx_std_20)
