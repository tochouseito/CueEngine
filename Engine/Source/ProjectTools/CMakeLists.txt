# Hub と CLI が利用する Project 作成処理は UI と Windows 型から分離する
add_library(Cue.ProjectTools STATIC Private/ProjectCreator.cpp Public/ProjectTools/ProjectCreator.h)
target_include_directories(Cue.ProjectTools PUBLIC Public)
target_link_libraries(Cue.ProjectTools PUBLIC Cue.Foundation Cue.Project PRIVATE Cue.Platform Cue.Scene Cue.EngineInstallation)
target_compile_features(Cue.ProjectTools PUBLIC cxx_std_20)
if(MSVC)
    target_compile_options(Cue.ProjectTools PRIVATE /utf-8
        $<$<CONFIG:Development>:/O2> $<$<CONFIG:Development>:/Zi>)
endif()
