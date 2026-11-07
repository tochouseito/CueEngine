# CPU 側の Mesh は Renderer / Platform から独立して生成する
add_library(Cue.Assets STATIC Private/MeshData.cpp Private/BuiltinMesh.cpp)
target_include_directories(Cue.Assets PUBLIC Public)
target_link_libraries(Cue.Assets PUBLIC Cue.Math)
target_compile_features(Cue.Assets PUBLIC cxx_std_20)
if(MSVC)
    target_compile_options(Cue.Assets PRIVATE /utf-8 $<$<CONFIG:Development>:/O2> $<$<CONFIG:Development>:/Zi>)
endif()
