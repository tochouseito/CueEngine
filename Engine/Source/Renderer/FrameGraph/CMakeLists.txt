# Backend 非依存の Pass 依存順と論理 Resource 寿命を計画する
add_library(Cue.Renderer.FrameGraph STATIC
    Private/FrameGraph.cpp
    Private/FrameGraphBuilder.cpp
)

target_include_directories(Cue.Renderer.FrameGraph PUBLIC Public)
target_link_libraries(Cue.Renderer.FrameGraph PUBLIC Cue.Foundation Cue.Renderer.RHI)
target_compile_features(Cue.Renderer.FrameGraph PUBLIC cxx_std_20)

if(MSVC)
    # Development でも他の Renderer Module と同じ最適化と Debug 情報を使う
    target_compile_options(Cue.Renderer.FrameGraph PRIVATE
        /utf-8
        $<$<CONFIG:Development>:/O2>
        $<$<CONFIG:Development>:/Zi>
    )
endif()
