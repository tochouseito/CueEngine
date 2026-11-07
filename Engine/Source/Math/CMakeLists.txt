# 数学型は Header で公開し、Platform / Renderer / ECS には依存しない
add_library(Cue.Math INTERFACE)
target_include_directories(Cue.Math INTERFACE Public)
target_link_libraries(Cue.Math INTERFACE Cue.Foundation)
target_compile_features(Cue.Math INTERFACE cxx_std_20)
