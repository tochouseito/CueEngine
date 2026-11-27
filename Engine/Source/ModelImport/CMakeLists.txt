# Assimp は OBJ を処理する Editor 用 Target に閉じ込め、製品 Runtime へ伝播させない
find_package(assimp 6.0.4 EXACT CONFIG REQUIRED)

add_library(Cue.ModelImport STATIC
    Private/ObjImporter.cpp
    Public/ModelImport/ObjImporter.h
)
target_include_directories(Cue.ModelImport PUBLIC Public)
target_link_libraries(Cue.ModelImport PUBLIC Cue.Foundation PRIVATE Cue.Assets Cue.Platform assimp::assimp)
target_compile_features(Cue.ModelImport PUBLIC cxx_std_20)
if(MSVC)
    target_compile_options(Cue.ModelImport PRIVATE /utf-8 $<$<CONFIG:Development>:/O2> $<$<CONFIG:Development>:/Zi>)
endif()
