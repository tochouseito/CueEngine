# Project の Build 成果物解決は Authoring Tool とし、Runtime / Game SDK から依存させない
add_library(Cue.ProjectBuild STATIC Private/GameBuild.cpp Private/BuildTarget.cpp Private/WindowsBuildPlatform.cpp)
target_include_directories(Cue.ProjectBuild PUBLIC Public)
target_link_libraries(Cue.ProjectBuild PUBLIC Cue.Project PRIVATE Cue.Platform)
target_compile_features(Cue.ProjectBuild PUBLIC cxx_std_20)
target_compile_definitions(Cue.ProjectBuild PRIVATE
    $<$<CONFIG:Debug>:CUE_GAME_BUILD_DEBUG=1>
    $<$<CONFIG:Development>:CUE_GAME_BUILD_DEVELOPMENT=1>
    $<$<CONFIG:Release>:CUE_GAME_BUILD_RELEASE=1>
)
if(WIN32)
    target_link_libraries(Cue.ProjectBuild PRIVATE bcrypt)
    # Process の実装は汎用の成果物契約から分け、Authoring Host が明示して接続する
    add_subdirectory(Windows)
endif()
if(MSVC)
    target_compile_options(Cue.ProjectBuild PRIVATE /utf-8 $<$<CONFIG:Development>:/O2> $<$<CONFIG:Development>:/Zi>)
endif()
