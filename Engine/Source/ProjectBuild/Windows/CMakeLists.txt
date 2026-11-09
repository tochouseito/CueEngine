# Windows の非同期 Build / 製品起動を具体化し、Runtime の公開契約へ Win32 を漏らさない
add_library(Cue.ProjectBuild.Windows STATIC
    Private/BuildProcess.cpp Private/GameWorkspace.cpp Private/WindowsGameBuildService.cpp)
target_include_directories(Cue.ProjectBuild.Windows PUBLIC Public)
target_link_libraries(Cue.ProjectBuild.Windows PUBLIC Cue.ProjectBuild PRIVATE
    Cue.Platform.Windows Cue.Foundation.Windows Cue.GameRuntime Cue.EngineInstallation Cue.EngineVersion)
target_compile_features(Cue.ProjectBuild.Windows PUBLIC cxx_std_20)
if(MSVC)
    target_compile_options(Cue.ProjectBuild.Windows PRIVATE /utf-8
        $<$<CONFIG:Development>:/O2> $<$<CONFIG:Development>:/Zi>)
endif()
