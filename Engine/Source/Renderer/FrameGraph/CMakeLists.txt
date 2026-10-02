# Backend 非依存の Pass 依存順と論理 Resource 寿命を計画する
add_library(Cue.Renderer.FrameGraph STATIC
    Private/FrameGraph.cpp
    Private/FrameGraphBuilder.cpp
)

target_include_directories(Cue.Renderer.FrameGraph PUBLIC Public)
target_link_libraries(Cue.Renderer.FrameGraph PUBLIC Cue.Foundation Cue.Renderer.RHI)
target_compile_features(Cue.Renderer.FrameGraph PUBLIC cxx_std_20)

if(MSVC)
    target_compile_options(Cue.Renderer.FrameGraph PRIVATE /utf-8)
endif()
