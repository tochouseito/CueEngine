# Host の起動・停止手順を実行 Entry Point から分離する
add_library(Cue.WindowsHost STATIC
    Private/WindowsHost.cpp
)

target_include_directories(Cue.WindowsHost PUBLIC
    Public
)

target_link_libraries(Cue.WindowsHost
    # 公開 Header の Runtime と Platform 契約だけを利用側へ伝える
    PUBLIC Cue.Runtime Cue.Platform
    # Win32 実装は Host 内部の依存に留める
    PRIVATE Cue.Platform.Windows Cue.Renderer.D3D12
)

target_compile_features(Cue.WindowsHost PUBLIC cxx_std_20)

if(MSVC)
    target_compile_options(Cue.WindowsHost PRIVATE
        /utf-8
        $<$<CONFIG:Development>:/O2>
        $<$<CONFIG:Development>:/Zi>
    )
endif()
