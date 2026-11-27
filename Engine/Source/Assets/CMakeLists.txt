# CPU 側の Mesh とアセット索引は Native API から独立して扱う
add_library(Cue.Assets STATIC
    Private/AssetCatalog.cpp
    Private/AssetPack.cpp
    Private/MeshData.cpp
    Private/BuiltinMesh.cpp
)
target_include_directories(Cue.Assets PUBLIC Public)
target_link_libraries(Cue.Assets PUBLIC Cue.Math PRIVATE Cue.Platform)
target_compile_features(Cue.Assets PUBLIC cxx_std_20)
if(MSVC)
    target_compile_options(Cue.Assets PRIVATE /utf-8 $<$<CONFIG:Development>:/O2> $<$<CONFIG:Development>:/Zi>)
endif()
