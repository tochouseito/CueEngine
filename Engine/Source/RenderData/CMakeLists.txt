# 描画 Snapshot の所有値だけを更新側と描画側で共有する
add_library(Cue.RenderData INTERFACE)
target_include_directories(Cue.RenderData INTERFACE Public)
target_link_libraries(Cue.RenderData INTERFACE Cue.Math)
