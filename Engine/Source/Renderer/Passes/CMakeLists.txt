# Backend 非依存の Pass 定義を Header ごとに公開する
add_library(Cue.Renderer.Passes INTERFACE)

target_sources(Cue.Renderer.Passes INTERFACE
    Public/Passes/ClearFinalColorPass.h
    Public/Passes/MainFrameGraph.h
    Public/Passes/PresentToSwapChainPass.h
)
target_include_directories(Cue.Renderer.Passes INTERFACE Public)
target_link_libraries(Cue.Renderer.Passes INTERFACE Cue.Renderer.FrameGraph)
target_compile_features(Cue.Renderer.Passes INTERFACE cxx_std_20)
