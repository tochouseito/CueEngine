# GPU 実行契約を Backend の実装 Header から分離する
add_library(Cue.Renderer.RHI INTERFACE)
target_include_directories(Cue.Renderer.RHI INTERFACE Public)
target_link_libraries(Cue.Renderer.RHI INTERFACE Cue.Foundation)
target_compile_features(Cue.Renderer.RHI INTERFACE cxx_std_20)
