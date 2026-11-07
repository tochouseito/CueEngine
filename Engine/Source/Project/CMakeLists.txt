# Project の永続設定と配置解決は Editor / Renderer の具体実装から独立させる
add_library(Cue.Project STATIC Private/Project.cpp Public/Project/Project.h)
target_include_directories(Cue.Project PUBLIC Public)
target_link_libraries(Cue.Project PUBLIC Cue.Foundation PRIVATE Cue.Platform Cue.EngineInstallation)
target_compile_features(Cue.Project PUBLIC cxx_std_20)

if(MSVC)
    target_compile_options(Cue.Project PRIVATE
        /utf-8
        $<$<CONFIG:Development>:/O2>
        $<$<CONFIG:Development>:/Zi>
    )
endif()
