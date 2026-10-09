# Host の起動・停止手順を実行 Entry Point から分離する
add_library(Cue.WindowsHost STATIC
    Private/WindowsHost.cpp
    Private/StoragePaths.cpp
)

target_include_directories(Cue.WindowsHost PUBLIC
    Public
)

target_link_libraries(Cue.WindowsHost
    # 公開 Header の Platform、Frame と抽象 Graph 構築契約を利用側へ伝える
    PUBLIC Cue.Platform Cue.Runtime.Frame Cue.Renderer.Passes
    # Win32 と DX12 の実装は Host 内部の依存に留める
    PRIVATE Cue.Platform.Windows Cue.Renderer.DX12 Cue.Runtime Cue.Logging
)

target_compile_features(Cue.WindowsHost PUBLIC cxx_std_20)
target_compile_definitions(Cue.WindowsHost PRIVATE $<$<CONFIG:Release>:CUE_SHIPPING=1>)

if(MSVC)
    target_compile_options(Cue.WindowsHost PRIVATE
        /utf-8
        $<$<CONFIG:Development>:/O2>
        $<$<CONFIG:Development>:/Zi>
    )
endif()
