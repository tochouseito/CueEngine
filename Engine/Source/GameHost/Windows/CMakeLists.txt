# Windows の起動と描画だけを具体化する。製品 exe は別途ゲームの Static Library を Link する
add_library(Cue.GameHost.Windows STATIC Private/WindowsGame.cpp Private/PlayerValidation.cpp)
target_include_directories(Cue.GameHost.Windows PUBLIC Public)
target_link_libraries(Cue.GameHost.Windows PUBLIC Cue.GameRuntime PRIVATE
    Cue.WindowsHost Cue.Renderer.DX12 Cue.Renderer.SceneRender Cue.Platform.Windows Cue.Foundation.Windows shell32)
target_compile_features(Cue.GameHost.Windows PUBLIC cxx_std_20)
if(MSVC)
    target_compile_options(Cue.GameHost.Windows PRIVATE /utf-8 $<$<CONFIG:Development>:/O2> $<$<CONFIG:Development>:/Zi>)
endif()
