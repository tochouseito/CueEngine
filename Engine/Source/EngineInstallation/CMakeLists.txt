# 配布識別と利用中排他を Editor / Tool に共有し、ゲーム Runtime の依存へ持ち込まない
add_library(Cue.EngineInstallation STATIC Private/EngineInstallation.cpp)
target_include_directories(Cue.EngineInstallation PUBLIC Public)
target_link_libraries(Cue.EngineInstallation PUBLIC Cue.Foundation PRIVATE Cue.Platform Cue.EngineVersion)
target_compile_features(Cue.EngineInstallation PUBLIC cxx_std_20)
if(WIN32)
    target_link_libraries(Cue.EngineInstallation PRIVATE Cue.Foundation.Windows)
endif()
if(MSVC)
    target_compile_options(Cue.EngineInstallation PRIVATE /utf-8
        $<$<CONFIG:Development>:/O2> $<$<CONFIG:Development>:/Zi>)
endif()
