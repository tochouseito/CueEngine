# Scene 描画の Interface を Backend 実装と Host で共有する
add_library(Cue.Renderer.SceneRender INTERFACE)
target_include_directories(Cue.Renderer.SceneRender INTERFACE Public)
target_link_libraries(Cue.Renderer.SceneRender INTERFACE Cue.RenderData Cue.Renderer.FrameGraph)
