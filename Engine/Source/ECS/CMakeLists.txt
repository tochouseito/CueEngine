# 旧 Engine の ECS を独立させ、Platform / Math / Renderer へ依存させない
add_library(Cue.ECS STATIC
    Public/ECS/ECSManager.h
    Public/ECS/ComponentIDHelper.h
    Private/ECSManager.cpp
    Private/EngineComponentID.cpp
    LICENSE.txt
)
target_include_directories(Cue.ECS PUBLIC Public)
target_link_libraries(Cue.ECS PUBLIC Cue.Foundation)
target_compile_features(Cue.ECS PUBLIC cxx_std_20)

if(MSVC)
    target_compile_options(Cue.ECS PRIVATE
        $<$<CONFIG:Development>:/O2>
        $<$<CONFIG:Development>:/Zi>
    )
endif()
